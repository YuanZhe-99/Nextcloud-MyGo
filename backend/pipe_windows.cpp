#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include "pipe_windows.h"
#include "wire.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>
#include <QtEndian>
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
/// tokenSID returns the current logon SID, binding pipe access to this logon session.
QString tokenSID(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD size = 0; GetTokenInformation(token, type, nullptr, 0, &size);
    std::vector<char> bytes(size);
    if (!size || !GetTokenInformation(token, type, bytes.data(), size, &size)) throw std::runtime_error("Cannot inspect token");
    PSID sid = nullptr;
    if (type == TokenUser) sid = reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid;
    else {
        auto groups = reinterpret_cast<TOKEN_GROUPS *>(bytes.data());
        for (DWORD i = 0; i < groups->GroupCount; ++i)
            if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) sid = groups->Groups[i].Sid;
    }
    LPWSTR text = nullptr;
    if (!sid || !ConvertSidToStringSidW(sid, &text)) throw std::runtime_error("Cannot inspect logon identity");
    auto result = QString::fromWCharArray(text); LocalFree(text); return result;
}
}

/// Impl keeps all transport objects and control dispatch on one event-loop thread.
struct PipeServer::Impl {
    /// Peer owns stable overlapped buffers until cancellation has completed.
    struct Peer {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        OVERLAPPED read{}, write{};
        bool accepting = true, reading = false, writing = false, dead = false;
        bool negotiated = false, subscribed = false;
        qint64 seq = -1;
        std::array<char, 16384> chunk{};
        QByteArray input, output, sending;
        QElapsedTimer stalled;
        /// Peer initializes manual-reset events used by overlapped I/O.
        Peer() { read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); stalled.start(); }
        /// ~Peer drains cancellation before freeing overlapped memory.
        ~Peer() {
            if (pipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(pipe, nullptr); DWORD ignored;
                if (reading) GetOverlappedResult(pipe, &read, &ignored, TRUE);
                if (writing) GetOverlappedResult(pipe, &write, &ignored, TRUE);
                CloseHandle(pipe);
            }
            CloseHandle(read.hEvent); CloseHandle(write.hEvent);
        }
    };
    Control &control;
    QString name, userSID, logonSID;
    DWORD session = 0;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QTimer timer;
    std::vector<std::unique_ptr<Peer>> peers;
    bool first = true;
    /// Impl derives a profile-specific pipe and a DACL without client instance-creation rights.
    Impl(Control &c, const QString &profile) : control(c) {
        HANDLE token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot open process token");
        try { userSID = tokenSID(token, TokenUser); logonSID = tokenSID(token, TokenGroups); }
        catch (...) { CloseHandle(token); throw; }
        CloseHandle(token);
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw std::runtime_error("Cannot identify session");
        const auto hash = QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
        name = "\\\\.\\pipe\\Nextcloud-MyGo-" + userSID + '-' + QString::number(session) + '-' + QString::fromLatin1(hash) + "-v1";
        // 0x0012019b grants read/write/synchronize but excludes FILE_CREATE_PIPE_INSTANCE (0x4).
        const auto sddl = "D:P(A;;0x0012019b;;;" + logonSID + ')';
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("Cannot restrict pipe access");
        listen();
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { tick(); }); timer.start(10);
    }
    /// ~Impl releases the explicit pipe descriptor after peer shutdown.
    ~Impl() { timer.stop(); peers.clear(); LocalFree(descriptor); }
    /// listen reserves an overlapped listener; only the first instance may create the pipe name.
    void listen() {
        auto peer = std::make_unique<Peer>(); SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
        peer->pipe = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(name.utf16()),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
        if (peer->pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve SyncD pipe");
        first = false;
        if (!ConnectNamedPipe(peer->pipe, &peer->read)) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) SetEvent(peer->read.hEvent);
            else if (error != ERROR_IO_PENDING) throw std::runtime_error("Cannot accept pipe");
        }
        peer->reading = true; peers.push_back(std::move(peer));
    }
    /// authenticate verifies the local client's process user, Windows session and impersonated logon SID.
    bool authenticate(Peer &p) {
        ULONG pid = 0; DWORD otherSession = 0;
        if (!GetNamedPipeClientProcessId(p.pipe, &pid) || !ProcessIdToSessionId(pid, &otherSession) || otherSession != session) return false;
        if (!ImpersonateNamedPipeClient(p.pipe)) return false;
        HANDLE token = nullptr; bool ok = false;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
            try { ok = tokenSID(token, TokenUser) == userSID && tokenSID(token, TokenGroups) == logonSID; } catch (...) {}
            CloseHandle(token);
        }
        if (!RevertToSelf()) std::terminate();
        return ok;
    }
    /// queue enforces a bounded output budget before allocating a response frame.
    void queue(Peer &p, const QJsonDocument &d) {
        auto frame = frameMessage(d);
        if (frame.size() > 1048580 || p.output.size() + p.sending.size() + frame.size() > 4194304) { p.dead = true; return; }
        p.output += frame;
    }
    /// messages dispatches bounded batches in arrival order and subscribes at the bootstrap cursor.
    void messages(Peer &p) {
        int processed = 0;
        while (p.input.size() >= 4 && processed++ < 32) {
            auto size = qFromLittleEndian<quint32>(p.input.constData());
            if (!size || size > 1048576) { p.dead = true; return; }
            if (p.input.size() < size + 4) return;
            QJsonDocument d;
            if (!decodeMessage(p.input.mid(4, size), d)) { p.dead = true; return; }
            p.input.remove(0, size + 4);
            if (d.isArray()) {
                if (d.array().isEmpty() || d.array().size() > 32) { p.dead = true; return; }
                QJsonArray replies;
                for (const auto &request : d.array()) {
                    const auto reply = control.dispatch(request, p.negotiated, p.subscribed);
                    if (!reply.isNull()) replies.append(reply);
                }
                if (!replies.isEmpty()) queue(p, QJsonDocument(replies));
            } else {
                const auto reply = control.dispatch(d.object(), p.negotiated, p.subscribed);
                if (!reply.isNull()) queue(p, QJsonDocument(reply.toObject()));
            }
        }
    }
    /// tick advances nonblocking I/O and publishes committed projections without cross-thread Qt calls.
    void tick() {
        for (auto &ptr : peers) {
            auto &p = *ptr; if (p.dead) continue;
            DWORD n = 0;
            if (p.accepting) {
                if (WaitForSingleObject(p.read.hEvent, 0) != WAIT_OBJECT_0) continue;
                p.reading = false; p.accepting = false; p.stalled.restart();
                // Impersonation becomes available after the first client message has been read.
            }
            if (p.reading && GetOverlappedResult(p.pipe, &p.read, &n, FALSE)) {
                p.reading = false;
                if (!n || !authenticate(p)) { p.dead = true; continue; }
                p.input.append(p.chunk.data(), n); p.stalled.restart();
            } else if (p.reading && GetLastError() != ERROR_IO_INCOMPLETE) { p.dead = true; continue; }
            messages(p);
            if (p.subscribed && p.seq != control.sequence()) {
                queue(p, QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", "event"}, {"params", control.projection()}})); p.seq = control.sequence();
            }
            if (p.writing && GetOverlappedResult(p.pipe, &p.write, &n, FALSE)) {
                p.writing = false; p.sending.remove(0, n); p.stalled.restart();
            } else if (p.writing && GetLastError() != ERROR_IO_INCOMPLETE) { p.dead = true; continue; }
            if (!p.writing) {
                if (p.sending.isEmpty()) p.sending.swap(p.output);
                if (!p.sending.isEmpty()) {
                    ResetEvent(p.write.hEvent); p.writing = true;
                    if (!WriteFile(p.pipe, p.sending.constData(), DWORD(p.sending.size()), nullptr, &p.write) && GetLastError() != ERROR_IO_PENDING) p.dead = true;
                }
            }
            if (!p.reading && !control.shutdownRequested()) {
                ResetEvent(p.read.hEvent); p.reading = true;
                if (!ReadFile(p.pipe, p.chunk.data(), DWORD(p.chunk.size()), nullptr, &p.read) && GetLastError() != ERROR_IO_PENDING) p.dead = true;
            }
            if ((!p.input.isEmpty() || p.writing || !p.negotiated) && p.stalled.elapsed() > 15000) p.dead = true;
        }
        for (auto &p : peers) if (p->dead && !control.shutdownRequested()) {
            // Retain the sole instance handle: no client can create another instance or claim the name between connections.
            CancelIoEx(p->pipe, nullptr); DWORD ignored;
            if (p->reading) GetOverlappedResult(p->pipe, &p->read, &ignored, TRUE);
            if (p->writing) GetOverlappedResult(p->pipe, &p->write, &ignored, TRUE);
            DisconnectNamedPipe(p->pipe);
            auto next = std::make_unique<Peer>(); next->pipe = p->pipe; p->pipe = INVALID_HANDLE_VALUE;
            if (!ConnectNamedPipe(next->pipe, &next->read)) {
                auto error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) SetEvent(next->read.hEvent);
                else if (error != ERROR_IO_PENDING) { QCoreApplication::exit(1); return; }
            }
            next->reading = true; p = std::move(next);
        }
        if (control.shutdownRequested()) {
            bool drained = true;
            for (auto &p : peers) if (!p->dead && (p->writing || !p->output.isEmpty() || !p->sending.isEmpty())) drained = false;
            if (drained) QCoreApplication::quit();
        }
    }
};

PipeServer::PipeServer(Control &control, const QString &profile) : impl_(std::make_unique<Impl>(control, profile)) {}
PipeServer::~PipeServer() = default;
