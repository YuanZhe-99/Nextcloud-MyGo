#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "control.h"
#include "pipe_windows.h"
#include "startup.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {
/// restrictLoading limits DLL and plugin loading of an installed SyncD to its
/// version directory and Windows (PLAN §16.1).
/// Notes: an installed layout is recognized by the qt.conf the stager writes
/// beside the executable; a development build has none and keeps the
/// developer's PATH and QT_PLUGIN_PATH. Runs before any library is loaded.
void restrictLoading() {
  wchar_t path[MAX_PATH * 4]{};
  const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
  if (length == 0 || length >= MAX_PATH * 4)
    return;
  std::wstring conf(path, length);
  conf.resize(conf.find_last_of(L'\\') + 1);
  conf += L"qt.conf";
  if (GetFileAttributesW(conf.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  // Neither PATH nor the current directory may supply a DLL, and Qt adds
  // QT_PLUGIN_PATH entries to qt.conf's plugin directory: drop them.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                           LOAD_LIBRARY_SEARCH_SYSTEM32);
  SetDllDirectoryW(L"");
  SetEnvironmentVariableW(L"QT_PLUGIN_PATH", nullptr);
  _wputenv_s(L"QT_PLUGIN_PATH", L"");
}
} // namespace

/// main owns a per-user SyncD profile and establishes authenticated IPC before
/// admitting credential recovery, root attachment or network work (PLAN §14.1).
/// Notes: `--version` prints this build's version, protocol and Control
/// schema as one JSON line and starts nothing (packaging.md).
int main(int argc, char **argv) {
  // A plugin or DLL that cannot load later fails the call, never with a modal
  // system error box in a background process (its own imports are resolved
  // before main; the parent's inherited error mode covers those).
  SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
               SEM_NOOPENFILEERRORBOX);
  restrictLoading();
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("{\"version\":\"%s\",\"protocol\":{\"major\":1,\"minor\":%d},"
                "\"controlSchema\":%d,\"engine\":%s}\n",
                SYNCD_VERSION, Control::protocolMinor, Control::schemaVersion,
#ifdef SYNCD_WITH_ENGINE
                "true"
#else
                "false"
#endif
    );
    return 0;
  }
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Nextcloud-MyGo");
  QCoreApplication::setApplicationVersion(SYNCD_VERSION);
  QString profile;
  const auto args = app.arguments();
  if (args.size() == 3 && args.at(1) == "--profile")
    profile = args.at(2);
  else if (args.size() != 1)
    return 2;
  const auto hash =
      QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256)
          .toHex()
          .left(24);
  const auto directory =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
      "/Nextcloud-MyGo/syncd/" + QString::fromLatin1(hash);
  if (!QDir().mkpath(directory))
    return 1;
  try {
    QElapsedTimer clock;
    clock.start();
    // msecsSinceReference() is the fixed reference time at which the clock
    // was started, not the current time: the admission clock adds elapsed().
    const auto started = clock.msecsSinceReference();
    const auto now = [&] { return started + clock.elapsed(); };
    Startup startup(directory, started);
    // Existing entrypoints are explicit initial launches: Remote launches once
    // per UI instance, not a crash supervisor. Recovery never clears this
    // intent.
    if (startup.decision().automaticRestartSuppressed)
      startup.recordStopIntent(false);
    const auto mode =
        startup.decision().diagnostic    ? Control::StartupMode::Diagnostic
        : startup.decision().delayMs > 0 ? Control::StartupMode::Backoff
                                         : Control::StartupMode::Deferred;
    std::unique_ptr<Control> control;
    try {
      control = std::make_unique<Control>(directory + "/control.sqlite", mode,
                                          &startup);
    } catch (...) {
      // Never replace a failed database with an empty configuration.
      control = std::make_unique<Control>(
          directory + "/control.sqlite",
          Control::StartupMode::DatabaseUnavailable, &startup);
    }
    int result = 1;
    bool clean = false;
    {
      PipeServer server(*control, profile);
      QTimer activation;
      activation.setSingleShot(true);
      activation.setTimerType(Qt::PreciseTimer);
      QObject::connect(&activation, &QTimer::timeout, &app, [&] {
        if (control->shutdownRequested())
          return;
        try {
          if (!control->activate(now()) && !startup.decision().diagnostic)
            activation.start(50);
        } catch (...) {
          // An unexpected recovery failure remains an unclean run. Any runtime
          // created by restoreFolders is still idle until its queued
          // attachment.
          QCoreApplication::exit(1);
        }
      });
      const auto state = control->projection()
                             .value("snapshot")
                             .toObject()
                             .value("daemon")
                             .toObject()
                             .value("startupState");
      if (state == "backoff") {
        const auto remaining =
            qMax<qint64>(0, startup.decision().delayMs - (now() - started));
        activation.start(static_cast<int>(remaining));
      }
      result = app.exec();
      clean = result == 0 && control->cleanShutdownReady();
    }
    control.reset();
    if (clean && !startup.finishClean())
      return 1;
    return result;
  } catch (...) {
    std::fputs("SyncD could not start.\n", stderr);
    return 1;
  }
}
