#pragma once
#include "control.h"
#include <memory>

/// PipeServer serves local, session-authenticated overlapped pipes on the Qt control thread.
/// Notes: bounded buffers and deadlines prevent a stalled peer from blocking sync control.
class PipeServer {
public:
    /// PipeServer reserves the first instance and starts accepting the profile's UI connections.
    PipeServer(Control &control, const QString &profile);
    /// ~PipeServer cancels all overlapped I/O before releasing its buffers.
    ~PipeServer();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
