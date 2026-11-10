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
#include <memory>

/// main owns a per-user SyncD profile and establishes authenticated IPC before
/// admitting credential recovery, root attachment or network work (PLAN §14.1).
int main(int argc, char **argv) {
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
    const auto started = clock.msecsSinceReference();
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
          if (!control->activate(clock.msecsSinceReference()) &&
              !startup.decision().diagnostic)
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
            qMax<qint64>(0, startup.decision().delayMs -
                                (clock.msecsSinceReference() - started));
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
