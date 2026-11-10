#include "switch.h"
#include <limits>

QString SwitchCoordinator::begin(const Target &target, quint64 expectedEpoch,
                                 const QSet<QString> &folders) {
  if (!target_.isEmpty())
    return "QUIESCE_STALLED";
  if (expectedEpoch != epoch_)
    return "REVISION_CONFLICT";
  if (target.id.isEmpty() || !target.approved)
    return "ENDPOINT_UNTRUSTED";
  if (!target.tlsValid)
    return "TLS_ERROR";
  if (!target.identityVerified || target.blocked)
    return "ENDPOINT_IDENTITY_MISMATCH";
  target_ = target.id;
  waiting_ = folders;
  members_ = folders;
  return {};
}

bool SwitchCoordinator::observe(const QString &folder, const Proof &p) {
  if (target_.isEmpty() || !waiting_.contains(folder) || p.epoch != epoch_ ||
      !p.engineStopped || !p.propagatorDestroyed || !p.discoveryDestroyed ||
      !p.requestsDrained || !p.journalFlushed)
    return false;
  waiting_.remove(folder);
  return true;
}

QString SwitchCoordinator::commit(const Target &target) {
  if (target_.isEmpty() || target.id != target_ || !waiting_.isEmpty())
    return "QUIESCE_STALLED";
  if (!target.approved)
    return "ENDPOINT_UNTRUSTED";
  if (!target.tlsValid)
    return "TLS_ERROR";
  if (!target.identityVerified || target.blocked)
    return "ENDPOINT_IDENTITY_MISMATCH";
  if (epoch_ == std::numeric_limits<quint64>::max())
    return "INTERNAL_ERROR";
  active_ = target_;
  ++epoch_;
  recovering_ = members_;
  cancel();
  return {};
}

bool SwitchCoordinator::reconciled(const QString &folder, quint64 epoch) {
  return epoch == epoch_ && recovering_.remove(folder);
}

void SwitchCoordinator::cancel() {
  target_.clear();
  waiting_.clear();
  members_.clear();
}
