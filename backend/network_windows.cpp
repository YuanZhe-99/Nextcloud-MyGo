// SPDX-License-Identifier: GPL-3.0-only
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <ws2ipdef.h>

#include <windows.h>

#include <iphlpapi.h>
#include <netioapi.h>

#include "network_monitor.h"
#include <QCryptographicHash>
#include <algorithm>
#include <stdexcept>
#include <vector>

/// State contains only thread-safe notification state and OS-owned registration
/// handles.
struct NetworkMonitor::State {
  std::atomic_bool dirty{true};
  HANDLE interfaces = nullptr, addresses = nullptr, routes = nullptr;
  HPOWERNOTIFY power = nullptr;
  HWND powerWindow = nullptr;
  QByteArray fingerprint;
  unsigned polls = 0;
  /// interfaceChanged records an interface event without touching Qt objects
  /// off-thread.
  static void CALLBACK interfaceChanged(void *p, MIB_IPINTERFACE_ROW *,
                                        MIB_NOTIFICATION_TYPE) {
    static_cast<State *>(p)->dirty.store(true);
  }
  /// addressChanged records an address event without retaining OS callback
  /// pointers.
  static void CALLBACK addressChanged(void *p, MIB_UNICASTIPADDRESS_ROW *,
                                      MIB_NOTIFICATION_TYPE) {
    static_cast<State *>(p)->dirty.store(true);
  }
  /// routeChanged records route/VPN changes without treating them as endpoint
  /// trust.
  static void CALLBACK routeChanged(void *p, MIB_IPFORWARD_ROW2 *,
                                    MIB_NOTIFICATION_TYPE) {
    static_cast<State *>(p)->dirty.store(true);
  }
  /// windowMessage receives power events only on the owning control thread;
  /// DestroyWindow ends its lifetime synchronously.
  static LRESULT CALLBACK windowMessage(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE)
      SetWindowLongPtrW(
          window, GWLP_USERDATA,
          reinterpret_cast<LONG_PTR>(
              reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams));
    auto *state =
        reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_POWERBROADCAST && state) {
      state->dirty.store(true);
      return TRUE;
    }
    if (message == WM_NCDESTROY)
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wparam, lparam);
  }
};

NetworkMonitor::NetworkMonitor() : state_(std::make_unique<State>()) {
  auto *s = state_.get();
  // A failed registration still has the DNS/adapter polling fallback; no
  // unsupported route guarantees are inferred.
  NotifyIpInterfaceChange(AF_UNSPEC, State::interfaceChanged, s, FALSE,
                          &s->interfaces);
  NotifyUnicastIpAddressChange(AF_UNSPEC, State::addressChanged, s, FALSE,
                               &s->addresses);
  NotifyRouteChange2(AF_UNSPEC, State::routeChanged, s, FALSE, &s->routes);
  static const ATOM windowClass = [] {
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = State::windowMessage;
    type.lpszClassName = L"NextcloudMyGo.NetworkMonitor";
    return RegisterClassW(&type);
  }();
  if (windowClass) {
    s->powerWindow =
        CreateWindowExW(0, MAKEINTATOM(windowClass), L"", 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), s);
    if (s->powerWindow)
      s->power = RegisterSuspendResumeNotification(s->powerWindow,
                                                   DEVICE_NOTIFY_WINDOW_HANDLE);
  }
  timer_.setInterval(250);
  QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
  timer_.start();
}

NetworkMonitor::~NetworkMonitor() {
  timer_.stop();
  for (auto handle : {state_->interfaces, state_->addresses, state_->routes})
    if (handle)
      CancelMibChangeNotify2(handle);
  if (state_->power)
    UnregisterSuspendResumeNotification(state_->power);
  if (state_->powerWindow)
    DestroyWindow(state_->powerWindow);
}

void NetworkMonitor::poll() {
  const bool notified = state_->dirty.exchange(false);
  if (!notified && ++state_->polls % 120 != 0)
    return;
  ULONG size = 16384;
  std::vector<unsigned char> bytes(size);
  ULONG code = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt != 3 && code == ERROR_BUFFER_OVERFLOW;
       ++attempt) {
    bytes.resize(size);
    code = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(bytes.data()), &size);
  }
  // Enumeration failure is unknown, not offline; still invalidate old health.
  bool online = code != NO_ERROR && code != ERROR_NO_DATA;
  QList<QByteArray> parts;
  if (code == NO_ERROR) {
    for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(bytes.data()); a;
         a = a->Next) {
      if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
          a->OperStatus != IfOperStatusUp)
        continue;
      online = online || a->FirstUnicastAddress;
      QByteArray part(reinterpret_cast<const char *>(&a->Luid.Value),
                      sizeof(a->Luid.Value));
      part.append(reinterpret_cast<const char *>(&a->Ipv4Metric),
                  sizeof(a->Ipv4Metric));
      part.append(reinterpret_cast<const char *>(&a->Ipv6Metric),
                  sizeof(a->Ipv6Metric));
      if (a->DnsSuffix)
        part.append(QString::fromWCharArray(a->DnsSuffix).toUtf8());
      for (auto *d = a->FirstDnsServerAddress; d; d = d->Next)
        part.append(reinterpret_cast<const char *>(d->Address.lpSockaddr),
                    d->Address.iSockaddrLength);
      for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
        part.append(reinterpret_cast<const char *>(u->Address.lpSockaddr),
                    u->Address.iSockaddrLength);
      for (auto *g = a->FirstGatewayAddress; g; g = g->Next)
        part.append(reinterpret_cast<const char *>(g->Address.lpSockaddr),
                    g->Address.iSockaddrLength);
      parts.append(part);
    }
  }
  std::sort(parts.begin(), parts.end());
  QByteArray joined;
  for (const auto &part : parts)
    joined.append(part);
  const auto hash =
      QCryptographicHash::hash(joined, QCryptographicHash::Sha256);
  if (notified || hash != state_->fingerprint) {
    state_->fingerprint = hash;
    if (changed)
      changed(online);
  }
}
