// SPDX-License-Identifier: GPL-3.0-only
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <sddl.h>

#include "control.h"
#include "pipe_windows.h"
#include "startup.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
/// require keeps assertions active in optimized native pipe regressions.
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
/// pipeName derives the existing authenticated transport name without logging
/// SID.
QString pipeName(const QString &profile) {
  HANDLE token = nullptr;
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token),
          "test token open failed");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<char> bytes(size);
  const bool inspected =
      size && GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
  CloseHandle(token);
  require(inspected, "test token inspection failed");
  LPWSTR sid = nullptr;
  require(ConvertSidToStringSidW(
              reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid),
          "test SID conversion failed");
  const auto user = QString::fromWCharArray(sid);
  LocalFree(sid);
  DWORD session = 0;
  require(ProcessIdToSessionId(GetCurrentProcessId(), &session),
          "test session inspection failed");
  const auto hash =
      QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256)
          .toHex()
          .left(24);
  return "\\\\.\\pipe\\Nextcloud-MyGo-" + user + '-' +
         QString::number(session) + '-' + QString::fromLatin1(hash) + "-v1";
}
/// transfer completes bounded synchronous client I/O; the parent owns its
/// deadline.
void transfer(HANDLE pipe, char *data, DWORD size, bool writing) {
  while (size) {
    DWORD done = 0;
    const bool ok = writing ? WriteFile(pipe, data, size, &done, nullptr)
                            : ReadFile(pipe, data, size, &done, nullptr);
    require(ok && done, "authenticated pipe I/O failed");
    data += done;
    size -= done;
  }
}
/// message reads one bounded actual response or availability event frame.
QJsonObject message(HANDLE pipe) {
  QByteArray header(4, '\0');
  transfer(pipe, header.data(), 4, false);
  const auto size = qFromLittleEndian<quint32>(header.constData());
  require(size && size <= 1048576, "pipe response exceeded framing limit");
  QByteArray response(size, '\0');
  transfer(pipe, response.data(), size, false);
  return QJsonDocument::fromJson(response).object();
}
/// call sends real protocol frames and skips already-known subscription events.
QJsonObject call(HANDLE pipe, int id, const QString &method,
                 const QJsonObject &params = {}) {
  const auto body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                              {"id", id},
                                              {"method", method},
                                              {"params", params}})
                        .toJson(QJsonDocument::Compact);
  QByteArray frame(4, '\0');
  qToLittleEndian<quint32>(static_cast<quint32>(body.size()), frame.data());
  frame += body;
  transfer(pipe, frame.data(), static_cast<DWORD>(frame.size()), true);
  for (int i = 0; i < 32; ++i) {
    const auto object = message(pipe);
    if (object.value("method") == "event")
      continue;
    require(object.value("id") == id, "unexpected pipe response ID");
    return object;
  }
  throw std::runtime_error("excessive subscription events before reply");
}
/// client proves diagnostic negotiation/bootstrap/read/shutdown over
/// authenticated IPC.
int client(const QString &profile, bool backoff) {
  const auto name = pipeName(profile);
  const auto pipe = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
  require(pipe != INVALID_HANDLE_VALUE,
          "diagnostic pipe did not accept a client");
  try {
    const auto hello = call(pipe, 1, "session.hello",
                            {{"protocol", QJsonObject{{"major", 1}}}});
    require(!hello.contains("error") && hello.value("result")
                                                .toObject()
                                                .value("protocol")
                                                .toObject()
                                                .value("minor") == 9,
            "authenticated diagnostic hello failed");
    const auto bootstrap = call(pipe, 2, "state.bootstrap");
    const auto snapshot =
        bootstrap.value("result").toObject().value("snapshot").toObject();
    require(!bootstrap.contains("error") && snapshot.size() == 1 &&
                snapshot.value("daemon").toObject().value("stateAvailable") ==
                    false &&
                snapshot.value("daemon").toObject().value("startupState") ==
                    (backoff ? "backoff" : "databaseUnavailable"),
            "real pipe bootstrap implied empty or normal state");
    const auto diagnostic =
        call(pipe, 3, "diagnostics.snapshot").value("result").toObject();
    require(diagnostic.value("stateAvailable") == false &&
                diagnostic.value("reasonCode") == "STATE_UNAVAILABLE" &&
                !diagnostic.contains("folders"),
            "real pipe diagnostic snapshot failed");
    const auto refused = call(pipe, 4, "accounts.list");
    require(refused.value("error").toObject().value("data").toObject().value(
                "reasonCode") == "STATE_UNAVAILABLE",
            "real pipe served unknown accounts as empty");
    if (backoff) {
      const auto before = bootstrap.value("result")
                              .toObject()
                              .value("seq")
                              .toString()
                              .toLongLong();
      std::puts("bootstrap-ready");
      std::fflush(stdout);
      bool available = false;
      for (int i = 0; i < 32; ++i) {
        const auto event = message(pipe);
        require(event.value("method") == "event",
                "availability was not an event");
        const auto projection = event.value("params").toObject();
        if (projection.value("seq").toString().toLongLong() <= before)
          continue;
        const auto state = projection.value("snapshot").toObject();
        require(state.value("daemon").toObject().value("stateAvailable") ==
                        true &&
                    state.value("daemon").toObject().value("startupState") ==
                        "normal",
                "availability event did not publish normal state");
        for (const auto &key : {"accounts", "folders", "endpoints", "networks",
                                "switches", "activity", "decisions"})
          require(state.value(key).isArray(),
                  "normal event omitted known state arrays");
        available = true;
        break;
      }
      require(available,
              "backoff did not advance to availability on the same connection");
      require(call(pipe, 6, "accounts.list").value("result").isArray(),
              "normal reads failed after availability without reconnect");
    }
    const auto stopped = call(
        pipe, 5, "daemon.shutdown",
        {{"operationKey", QUuid::createUuid().toString(QUuid::WithoutBraces)},
         {"reason", "user"}});
    require(stopped.value("result").toObject().value("state") == "succeeded",
            "real pipe diagnostic stop failed");
  } catch (...) {
    CloseHandle(pipe);
    throw;
  }
  CloseHandle(pipe);
  return 0;
}
/// fixture writes only owned disposable original bytes.
void fixture(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "pipe fixture write failed");
}
/// original verifies that degraded startup preserved exact source evidence.
bool original(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) && file.readAll() == bytes;
}
/// server runs production Control/PipeServer on its control thread against an
/// owned corrupt database, never a real account or sync-content fixture.
void server(QCoreApplication &app, bool backoff) {
  QTemporaryDir root;
  if (backoff) {
    Startup crashed(root.path(), 1000);
  }
  Startup startup(root.path(), backoff ? 2000 : 1000);
  const auto path = root.filePath("control.sqlite");
  const auto journal = root.filePath("journal.sqlite");
  if (!backoff)
    fixture(path, "original corrupt database bytes");
  fixture(journal, "original journal bytes");
  std::unique_ptr<Control> control;
  try {
    control = std::make_unique<Control>(path, Control::StartupMode::Deferred,
                                        &startup);
  } catch (...) {
    control = std::make_unique<Control>(
        path, Control::StartupMode::DatabaseUnavailable, &startup);
  }
  const auto profile = QUuid::createUuid().toString(QUuid::WithoutBraces);
  {
    PipeServer pipe(*control, profile);
    QProcess child;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { app.exit(2); });
    QObject::connect(&child, &QProcess::finished, &app,
                     [&](int code, QProcess::ExitStatus status) {
                       if (code || status != QProcess::NormalExit)
                         app.exit(3);
                     });
    QObject::connect(&child, &QProcess::readyReadStandardOutput, &app, [&] {
      const auto output = child.readAllStandardOutput();
      if (backoff && output.contains("bootstrap-ready")) {
        try {
          require(control->activate(4000),
                  "pipe-driven deferred activation failed");
        } catch (...) {
          app.exit(4);
        }
      }
    });
    deadline.start(30000);
    child.start(QCoreApplication::applicationFilePath(),
                {backoff ? "--backoff-client" : "--client", profile});
    require(child.waitForStarted(10000), "pipe client did not start");
    const auto result = app.exec();
    if (!child.waitForFinished(5000)) {
      child.kill();
      child.waitForFinished(5000);
    }
    const auto childError = child.readAllStandardError();
    if (!childError.isEmpty())
      std::fwrite(childError.constData(), 1, childError.size(), stderr);
    require(result == 0 && child.exitStatus() == QProcess::NormalExit &&
                child.exitCode() == 0 && control->cleanShutdownReady(),
            "authenticated diagnostic pipe regression failed");
  }
  control.reset();
  require((backoff || original(path, "original corrupt database bytes")) &&
              original(journal, "original journal bytes"),
          "real diagnostic pipe startup modified original evidence");
  require(startup.finishClean(),
          "real diagnostic pipe stop did not finish cleanly");
}
} // namespace

/// main runs native authenticated diagnostic-startup IPC without Go or content
/// fixtures.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    const auto args = app.arguments();
    if (args.size() == 3 &&
        (args[1] == "--client" || args[1] == "--backoff-client"))
      return client(args[2], args[1] == "--backoff-client");
    server(app, false);
    server(app, true);
    std::puts("2 authenticated startup pipe contract groups passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
