// SPDX-License-Identifier: GPL-3.0-only
#include "engine_runtime.h"
#include "login.h"
#include "namespace.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <stdexcept>

/// main runs one adapter cycle against a caller-created disposable fixture;
/// secrets stay in the environment/vault.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("Nextcloud-MyGo-engine-fixture");
  const auto args = app.arguments();
  if (args.size() != 5)
    return 2;
  const auto reference = "Nextcloud-MyGo/tests/" +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         "/app-password";
  int result = 1;
  try {
    const auto base =
        canonicalServer(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
    const auto user = qEnvironmentVariable("NEXTCLOUD_TEST_USER"),
               secret = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
    if (user.isEmpty() || secret.isEmpty())
      throw std::runtime_error("Approved fixture credentials required");
    LoginFlow verification(base, QUuid::createUuid().toString(),
                           QUuid::createUuid().toString());
    verification.saveCredential = [&](const QString &name,
                                      const QString &password) {
      try {
        writeCredential(reference, name, password);
        return true;
      } catch (...) {
        return false;
      }
    };
    QEventLoop loginLoop;
    verification.changed = [&](LoginFlow &) {
      loginLoop.quit();
      return true;
    };
    verification.verifyCredentials(user, secret);
    if (verification.status() == "verifying")
      loginLoop.exec();
    if (verification.status() != "succeeded")
      throw std::runtime_error("Fixture namespace verification failed");
    EngineRuntime runtime;
    const bool rebind = qEnvironmentVariableIntValue("SYNCD_TEST_REBIND") == 1;
    unsigned rebindings = 0;
    bool shuttingDown = false;
    runtime.add("fixture-folder", "fixture-account",
                {base, user, verification.uid, reference, args.at(1),
                 args.at(2), args.at(3), args.at(4)});
    runtime.cycle = [&](const QString &, const EngineAdapter::Result &done) {
      if (done.epoch != rebindings) {
        result = 1;
        shuttingDown = true;
        runtime.shutdown();
        return;
      }
      result = done.success ? 0 : 1;
      const QStringList known{"",
                              "SYNC_FAILED",
                              "JOURNAL_ERROR",
                              "AUTH_REQUIRED",
                              "TLS_ERROR",
                              "ENDPOINT_UNTRUSTED",
                              "CANCELLED",
                              "UNSUPPORTED_FOLDER_MODE",
                              "DISCOVERY_INCOMPLETE"};
      const auto reason = known.contains(done.reason) ? done.reason : "OTHER";
      std::fprintf(stderr, "SYNCD_FIXTURE success=%d reason=%s\n", done.success,
                   reason.toUtf8().constData());
      if (done.success && rebind && rebindings < 2) {
        try {
          if (!runtime.quiesceAccount("fixture-account", false))
            throw std::runtime_error("Fixture account not quiescent");
          const auto alternateBase = canonicalServer(
              qEnvironmentVariable("NEXTCLOUD_TEST_ALTERNATE_URL"));
          const auto target = rebindings == 0 ? alternateBase : base;
          const auto previous = rebindings == 0 ? base : alternateBase;
          LoginFlow alternate(target, QUuid::createUuid().toString(),
                              QUuid::createUuid().toString());
          alternate.saveCredential = verification.saveCredential;
          QEventLoop loop;
          alternate.changed = [&](LoginFlow &) {
            loop.quit();
            return true;
          };
          alternate.verifyCredentials(user, secret);
          if (alternate.status() == "verifying")
            loop.exec();
          if (alternate.status() != "succeeded" ||
              alternate.uid != verification.uid ||
              alternate.rootId != verification.rootId)
            throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
          const auto oldRoot = readCollections(previous, verification.uid, user,
                                               reference, args.at(2), false);
          const auto newRoot = readCollections(target, verification.uid, user,
                                               reference, args.at(2), false);
          if (oldRoot.front().identity != newRoot.front().identity ||
              newRoot.front().encrypted)
            throw std::runtime_error("ENDPOINT_IDENTITY_MISMATCH");
          runtime.rebindAccount("fixture-account", target, rebindings + 1);
          ++rebindings;
          runtime.resumeAccount("fixture-account");
          return;
        } catch (const std::exception &error) {
          result = 1;
          const QString reason = QString::fromUtf8(error.what());
          if (reason == "RESUME_CONTEXT_UNSUPPORTED") {
            {
              auto db =
                  QSqlDatabase::addDatabase("QSQLITE", "fixture-resume-counts");
              db.setDatabaseName(args.at(3));
              db.setConnectOptions("QSQLITE_OPEN_READONLY");
              if (db.open()) {
                QSqlQuery q(db);
                if (q.exec("SELECT (SELECT COUNT(*) FROM async_poll), (SELECT "
                           "COUNT(*) FROM uploadinfo), (SELECT COUNT(*) FROM "
                           "downloadinfo)") &&
                    q.next())
                  std::fprintf(
                      stderr,
                      "SYNCD_FIXTURE polls=%lld uploads=%lld downloads=%lld\n",
                      q.value(0).toLongLong(), q.value(1).toLongLong(),
                      q.value(2).toLongLong());
              }
            }
            QSqlDatabase::removeDatabase("fixture-resume-counts");
          }
          const QStringList allowed{"JOURNAL_ERROR",
                                    "RESUME_CONTEXT_UNSUPPORTED",
                                    "QUIESCE_STALLED",
                                    "FOLDER_ROOT_CHANGED",
                                    "REVISION_CONFLICT",
                                    "ENDPOINT_UNTRUSTED",
                                    "ENDPOINT_IDENTITY_MISMATCH",
                                    "NETWORK_UNAVAILABLE",
                                    "AUTH_REQUIRED",
                                    "TLS_ERROR",
                                    "INVALID_PARAMS"};
          std::fprintf(stderr, "SYNCD_FIXTURE success=0 reason=%s\n",
                       allowed.contains(reason) ? error.what()
                                                : "REBIND_FAILED");
          shuttingDown = true;
          runtime.shutdown();
          return;
        }
      }
      const auto counts = runtime.requests("fixture-folder");
      const QJsonObject output{{"Requests", qint64(counts.total)},
                               {"Discovery", qint64(counts.discovery)},
                               {"Downloads", qint64(counts.downloads)},
                               {"Writes", qint64(counts.writes)}};
      std::puts(
          QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
      shuttingDown = true;
      runtime.shutdown();
    };
    runtime.stopped = [&] {
      const auto counts = runtime.requests("fixture-folder");
      if (counts.active || counts.alive || counts.managers || counts.accounts)
        result = 1;
      else
        std::puts("PASS actual account/network/reply shutdown drainage");
      if (rebind && rebindings != 2)
        result = 1;
      app.quit();
    };
    QTimer deadline;
    QTimer observation;
    QObject::connect(&observation, &QTimer::timeout, &runtime, [&] {
      const auto counts = runtime.requests("fixture-folder");
      std::fprintf(stderr,
                   "SYNCD_FIXTURE idle=%d active=%llu alive=%llu managers=%llu "
                   "accounts=%llu\n",
                   runtime.idle(),
                   static_cast<unsigned long long>(counts.active),
                   static_cast<unsigned long long>(counts.alive),
                   static_cast<unsigned long long>(counts.managers),
                   static_cast<unsigned long long>(counts.accounts));
      std::fflush(stderr);
      if (shuttingDown && runtime.idle() &&
          (counts.accounts || counts.managers || counts.alive)) {
        result = 1;
        app.quit();
      }
    });
    observation.start(10000);
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &runtime, [&] {
      shuttingDown = true;
      result = 1;
      runtime.shutdown();
    });
    deadline.start(90000);
    app.exec();
  } catch (...) {
    std::fputs("Engine fixture failed\n", stderr);
  }
  try {
    removeCredential(reference);
  } catch (...) {
    std::fputs("Fixture credential cleanup failed\n", stderr);
    return 1;
  }
  return result;
}
