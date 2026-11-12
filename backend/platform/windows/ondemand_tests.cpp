// SPDX-License-Identifier: GPL-3.0-only
// On-demand folder contracts against the real Cloud Files API on temporary
// NTFS roots: registration identity, ownership, the request gate a switch
// waits on, and complete unregistration. No server is contacted; hydration
// requests fail against a closed loopback port.
// windows.h and winternl.h (NTSTATUS) must precede cfapi.h.
#include <windows.h>
#include <winternl.h>

#include <cfapi.h>

#include "account.h"
#include "clientsideencryption.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "creds/abstractcredentials.h"
#include "platform/windows/ondemand.h"
#include "root.h"
#include "vfs/cfapi/cfapiwrapper.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <sddl.h>
#include <stdexcept>

namespace {
/// require keeps contract checks active in release builds.
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

/// step names each case so a failure says where it stopped.
void step(const char *name) { std::fprintf(stderr, "STEP %s\n", name); }

/// OfflineCredentials gives the account a network manager without secrets;
/// every request it creates goes to a closed loopback port.
class OfflineCredentials final : public OCC::AbstractCredentials {
public:
  QString authType() const override { return "http"; }
  QString user() const override { return "fixture"; }
  QString password() const override { return {}; }
  QNetworkAccessManager *createQNAM() const override {
    return new QNetworkAccessManager;
  }
  bool ready() const override { return true; }
  void fetchFromKeychain(const QString & = {}) override {
    _wasFetched = true;
    Q_EMIT fetched();
  }
  void askFromUser() override { Q_EMIT asked(); }
  bool stillValid(QNetworkReply *) override { return true; }
  void persist() override {}
  void invalidateToken() override {}
  void forgetSensitiveData() override {}
};

/// syncRootKey is the HKLM registration key of an identity.
QString syncRootKey(const OnDemandRoot::Identity &identity) {
  HANDLE token = nullptr;
  OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  QByteArray data(int(size), 0);
  GetTokenInformation(token, TokenUser, data.data(), size, &size);
  CloseHandle(token);
  LPWSTR sid = nullptr;
  ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,
                         &sid);
  const auto text = QString::fromWCharArray(sid);
  LocalFree(sid);
  return QString(
             R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager\)") +
         OnDemandRoot::provider + '!' + text + '!' + identity.account + '!' +
         identity.folder;
}

/// hasValue reports whether a registration value exists.
bool hasValue(const QString &key, const wchar_t *name) {
  return RegGetValueW(HKEY_LOCAL_MACHINE,
                      reinterpret_cast<LPCWSTR>(key.utf16()), name, RRF_RT_ANY,
                      nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

/// hasKey reports whether a registration key exists.
bool hasKey(const QString &key) {
  HKEY handle = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reinterpret_cast<LPCWSTR>(key.utf16()),
                    0, KEY_READ, &handle) != ERROR_SUCCESS)
    return false;
  RegCloseKey(handle);
  return true;
}

/// underSyncRoot reports whether Cloud Files still knows a path.
bool underSyncRoot(const QString &path) {
  CF_SYNC_ROOT_PROVIDER_INFO info{};
  DWORD length = 0;
  return CfGetSyncRootInfoByPath(
             reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
             CF_SYNC_ROOT_INFO_PROVIDER, &info, sizeof(info), &length) !=
         HRESULT_FROM_WIN32(ERROR_CLOUD_FILE_NOT_UNDER_SYNC_ROOT);
}

/// attributes reads a path's attributes without opening its content.
DWORD attributes(const QString &path) {
  return GetFileAttributesW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
}

/// unsupported reports a lease refusal for an unsupported folder mode.
bool unsupported(const std::function<void()> &lease) {
  try {
    lease();
    return false;
  } catch (const std::exception &e) {
    return QByteArray(e.what()) == "UNSUPPORTED_FOLDER_MODE";
  }
}

/// startReader opens a placeholder from another process, as an application
/// would; the provider never hydrates for its own process.
void startReader(QProcess &reader, const QString &path) {
  reader.setProgram("cmd.exe");
  reader.setNativeArguments("/d /c copy /b /y \"" +
                            QDir::toNativeSeparators(path) + "\" nul > nul");
  reader.start();
  require(reader.waitForStarted(10000), "reader did not start");
}

/// finish serves the control thread until the reader exits.
/// Returns: its exit code, or -1 after the deadline.
int finish(QProcess &reader, int milliseconds) {
  const QDeadlineTimer deadline(milliseconds);
  while (reader.state() != QProcess::NotRunning && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    reader.waitForFinished(10);
  }
  if (reader.state() != QProcess::NotRunning) {
    reader.kill();
    reader.waitForFinished(10000);
    return -1;
  }
  return reader.exitStatus() == QProcess::NormalExit ? reader.exitCode() : -1;
}

/// virtualRecord registers a placeholder in the journal as upstream discovery
/// would, so a hydration request reaches the network.
void virtualRecord(OCC::SyncJournalDb &journal, const QByteArray &path,
                   qint64 size) {
  OCC::SyncJournalFileRecord record;
  record._path = path;
  record._type = ItemTypeVirtualFile;
  record._fileId = "fixture-" + path;
  record._etag = "etag";
  record._modtime = 1700000000;
  record._fileSize = size;
  record._remotePerm = OCC::RemotePermissions::fromDbValue("WDNVR");
  require(bool(journal.setFileRecord(record)), "journal record not written");
}

/// convertInPlace makes an ordinary file with content a hydrated placeholder.
void convertInPlace(const QString &path) {
  auto handle = CreateFileW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
      nullptr);
  require(handle != INVALID_HANDLE_VALUE, "hydrated fixture not opened");
  const QByteArray id = "fixture-kept";
  const auto result =
      CfConvertToPlaceholder(handle, id.constData(), DWORD(id.size()),
                             CF_CONVERT_FLAG_MARK_IN_SYNC, nullptr, nullptr);
  CloseHandle(handle);
  require(SUCCEEDED(result), "hydrated placeholder not created");
}
} // namespace

/// main runs the cases in order; every exit path unregisters what it created.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QElapsedTimer total;
  total.start();
  QTemporaryDir fixture;
  QString root, foreign;
  OnDemandRoot::Identity identity{
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      QUuid::createUuid().toString(QUuid::WithoutBraces),
      "Nextcloud-MyGo - contract"};
  const auto cleanup = qScopeGuard([&] {
    // Unregister whatever a failed case left, never another provider's root.
    if (!root.isEmpty() && underSyncRoot(root))
      OnDemandRoot::unregister(root, identity);
    if (!foreign.isEmpty() && underSyncRoot(foreign))
      CfUnregisterSyncRoot(
          reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(foreign).utf16()));
  });
  try {
    require(fixture.isValid(), "fixture directory missing");
    root = QDir::cleanPath(fixture.filePath("root"));
    foreign = QDir::cleanPath(fixture.filePath("foreign"));
    require(QDir().mkpath(root + "/populated") && QDir().mkpath(foreign),
            "fixture roots missing");
    {
      QFile plain(root + "/plain.txt"), kept(root + "/kept.txt");
      require(plain.open(QIODevice::WriteOnly) && plain.write("plain") == 5,
              "plain fixture missing");
      require(kept.open(QIODevice::WriteOnly) && kept.write("kept") == 4,
              "kept fixture missing");
    }
    require(OnDemandRoot::available(), "CFAPI plugin did not load");

    step("ownership before registration");
    require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
            "an unregistered folder was refused");

    auto account = OCC::Account::create(false);
    const auto releaseAccount =
        qScopeGuard([&] { account->e2e()->setAccount({}); });
    account->setUrl(QUrl("https://127.0.0.1:9"));
    account->setCredentials(new OfflineCredentials);
    account->setDavUser("fixture");
    OCC::SyncJournalDb journal(fixture.filePath("journal.db"));
    {
      step("registration identity (PLAN §2.3, INV-02)");
      OnDemandRoot connected(identity, root, "/", account, &journal);
      const auto key = syncRootKey(identity);
      require(hasKey(key), "sync root not registered under this folder's ID");
      require(!hasValue(key, L"CustomStateHandler") &&
                  !hasValue(key, L"ThumbnailProvider") &&
                  !hasValue(key, L"NamespaceCLSID"),
              "sync root names a shell handler that was not registered");
      require(OnDemandRoot::ownershipError(root, identity).isEmpty(),
              "own registration refused");
      auto other = identity;
      other.folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
      require(OnDemandRoot::ownershipError(root, other) == "FOLDER_OWNED",
              "another folder ID adopted this sync root");
      require(OnDemandRoot::ownershipError(root + "/populated", identity) ==
                  "FOLDER_OWNED",
              "a folder inside the sync root was accepted");

      step("leases admit only the own cloud root (INV-01)");
      {
        RootLease lease(root, true);
        require(lease.verify(), "on-demand lease refused its own root");
        require(lease.validateTree(), "on-demand tree refused");
      }
      require(unsupported([&] { RootLease lease(root); }) ||
                  !(attributes(root) & FILE_ATTRIBUTE_REPARSE_POINT),
              "full-file lease accepted a cloud root");

      step("placeholders are admitted only on demand");
      require(bool(OCC::CfApiWrapper::createPlaceholderInfo(
                  QDir::toNativeSeparators(root + "/online.bin"), 1700000000,
                  4096, "fixture-online.bin")),
              "placeholder not created");
      virtualRecord(journal, "online.bin", 4096);
      require(attributes(root + "/online.bin") &
                  FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
              "placeholder is not online-only");
      convertInPlace(root + "/kept.txt");
      {
        RootLease lease(root, true);
        require(lease.validateTree(), "own placeholders refused");
      }

      step("a running hydration keeps the epoch busy (§2.3, INV-05)");
      QProcess reader;
      // Do not serve the control thread yet: the request stays in flight.
      startReader(reader, root + "/online.bin");
      const QDeadlineTimer arrival(15000);
      while (!connected.busy() && !arrival.hasExpired())
        QThread::msleep(5);
      require(connected.busy(), "hydration request never reached SyncD");
      const auto code = finish(reader, 60000);
      std::fprintf(stderr, "reader exit %d: %s\n", code,
                   reader.readAllStandardError().trimmed().constData());
      require(code > 0, "hydration from an unreachable server succeeded");
      require(!connected.busy(), "finished hydration kept the epoch busy");
      require(attributes(root + "/online.bin") &
                  FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS,
              "failed hydration left local content");

      step("a closed root refuses requests without network");
      connected.close();
      QProcess refused;
      startReader(refused, root + "/online.bin");
      const auto started = total.elapsed();
      require(finish(refused, 30000) > 0, "closed root served a hydration");
      require(total.elapsed() - started < 10000,
              "refusal waited for a request");
      require(!connected.busy(), "refused request kept the epoch busy");
    }

    step("a foreign provider's root is never adopted (INV-01)");
    {
      const std::wstring name = L"SyncD-Contract-Foreign", version = L"1";
      CF_SYNC_REGISTRATION registration{};
      registration.StructSize = sizeof(registration);
      registration.ProviderName = name.c_str();
      registration.ProviderVersion = version.c_str();
      CF_SYNC_POLICIES policies{};
      policies.StructSize = sizeof(policies);
      policies.Hydration.Primary = CF_HYDRATION_POLICY_FULL;
      policies.Population.Primary = CF_POPULATION_POLICY_ALWAYS_FULL;
      require(SUCCEEDED(CfRegisterSyncRoot(
                  reinterpret_cast<LPCWSTR>(
                      QDir::toNativeSeparators(foreign).utf16()),
                  &registration, &policies, CF_REGISTER_FLAG_NONE)),
              "foreign fixture root not registered");
      auto stranger = identity;
      stranger.folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
      require(OnDemandRoot::ownershipError(foreign, stranger) == "FOLDER_OWNED",
              "a foreign sync root was accepted");
      require(OnDemandRoot::unregister(foreign, stranger) == "FOLDER_OWNED",
              "a foreign sync root was unregistered");
      require(underSyncRoot(foreign), "foreign sync root was touched");
    }

    step("unregistration releases everything it owns");
    require(OnDemandRoot::unregister(root, identity).isEmpty(),
            "unregistration failed");
    require(!hasKey(syncRootKey(identity)), "registration key left behind");
    require(!underSyncRoot(root), "Cloud Files still knows the root");
    require(!QFileInfo::exists(root + "/online.bin"),
            "online-only placeholder left without a provider");
    QFile kept(root + "/kept.txt"), plain(root + "/plain.txt");
    require(
        kept.open(QIODevice::ReadOnly) && kept.readAll() == "kept" &&
            !(attributes(root + "/kept.txt") & FILE_ATTRIBUTE_REPARSE_POINT),
        "hydrated content not kept as an ordinary file");
    require(plain.open(QIODevice::ReadOnly) && plain.readAll() == "plain",
            "ordinary file changed");
    require(QFileInfo(root + "/populated").isDir(), "directory removed");
    require(OnDemandRoot::unregister(root, identity).isEmpty(),
            "repeated unregistration failed");
    {
      RootLease lease(root);
      require(lease.validateTree(), "released folder is not a full-file root");
    }
    std::fprintf(stderr, "PASS ondemand contracts %lld ms\n",
                 static_cast<long long>(total.elapsed()));
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
