// SPDX-License-Identifier: GPL-3.0-only
#include "control.h"
#include "startup.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlQuery>
#include <stdexcept>

QJsonObject Control::startupDiagnostics() const {
  QJsonObject result{{"version", SYNCD_VERSION},
                     {"protocol", "1.8"},
                     {"stateAvailable", false},
                     {"startupState", startupState_},
                     {"reasonCode", "STATE_UNAVAILABLE"}};
  if (startup_) {
    const auto &decision = startup_->decision();
    result.insert("priorUnclean", decision.priorUnclean);
    result.insert("recentCrashes", QString::number(decision.crashes));
    result.insert("restartDelayMs", QString::number(decision.delayMs));
  }
  return result;
}

QJsonValue Control::startupDispatch(const QString &method,
                                    const QJsonObject &params) {
  if (method == "diagnostics.snapshot")
    return startupDiagnostics();
  if (method == "daemon.shutdown") {
    if (!startup_)
      throw std::runtime_error("STATE_UNAVAILABLE");
    if (db_.isOpen()) {
      QSqlQuery query(db_);
      query.prepare("SELECT digest,receipt FROM operations WHERE key=?");
      query.addBindValue(params.value("operationKey").toString());
      if (!query.exec())
        throw std::runtime_error("INTERNAL_ERROR");
      if (query.next()) {
        const auto digest = QCryptographicHash::hash(
            QByteArray("daemon.shutdown\n") +
                QJsonDocument(params).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256);
        if (query.value(0).toByteArray() != digest)
          throw std::runtime_error("OPERATION_KEY_CONFLICT");
        return QJsonDocument::fromJson(query.value(1).toByteArray()).object();
      }
    }
    bool accepted = false;
    const auto receipt = startup_->diagnosticStop(params, accepted);
    if (accepted) {
      shutdownOperation_ = receipt.value("operationId").toString();
      shutdownReady_ = true;
      shutdown_ = true;
      startupState_ = "stopped";
    }
    return receipt;
  }
  if (method == "operations.get") {
    if (db_.isOpen()) {
      QSqlQuery query(db_);
      query.prepare("SELECT receipt FROM operations WHERE id=?");
      query.addBindValue(params.value("operationId").toString());
      if (!query.exec())
        throw std::runtime_error("INTERNAL_ERROR");
      if (query.next())
        return QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    }
    if (startup_) {
      try {
        return startup_->diagnosticOperation(
            params.value("operationId").toString());
      } catch (const std::exception &error) {
        if (QString::fromUtf8(error.what()) != "NOT_FOUND")
          throw;
      }
    }
    // An unreadable Control DB cannot prove that an earlier mutation did not
    // commit. Only a fully checked normal DB plus retained history prove
    // absence.
    throw std::runtime_error(db_.isOpen() ? "NOT_FOUND" : "STATE_UNAVAILABLE");
  }
  throw std::runtime_error("STATE_UNAVAILABLE");
}
