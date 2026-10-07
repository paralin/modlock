#include "host_app/windows/vconsole_port.h"

#include <winsock2.h>
#include <ws2tcpip.h>

// Winsock declarations must precede windows.h.
#include <windows.h>

#include <cstring>
#include <iostream>

#include "host_app/windows/import_patch.h"

namespace modlock::host_app {
namespace {

// kBindOrdinal is bind's export ordinal in ws2_32.dll; vconcomm imports it by
// ordinal rather than by name.
constexpr WORD kBindOrdinal = 2;

using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);

// original_bind is ws2_32's bind, read from the patched import slot. It is
// written once before the engine starts and read by the listener thread.
BindFn original_bind = nullptr;

// SharedBind binds as requested and, when the address is taken, retries the
// same address on port 0 so the system picks a free port. vconcomm sets
// SO_REUSEADDR, which lets two processes bind the port and fails only the later
// listen, where the port can no longer change; clearing it makes bind report the
// conflict.
int WSAAPI SharedBind(SOCKET socket, const sockaddr* address, int length) {
  const BOOL reuse = FALSE;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  const int result = original_bind(socket, address, length);
  if (result == 0 || WSAGetLastError() != WSAEADDRINUSE) return result;

  sockaddr_storage retry{};
  if (length <= 0 || static_cast<size_t>(length) > sizeof(retry)) return result;
  std::memcpy(&retry, address, length);
  if (retry.ss_family == AF_INET) {
    reinterpret_cast<sockaddr_in&>(retry).sin_port = 0;
  } else if (retry.ss_family == AF_INET6) {
    reinterpret_cast<sockaddr_in6&>(retry).sin6_port = 0;
  } else {
    return result;
  }
  const int retried = original_bind(socket, reinterpret_cast<const sockaddr*>(&retry), length);
  if (retried == 0) {
    std::cerr << "[modlock] VConsole port in use; listening on an ephemeral port\n";
  }
  return retried;
}

}  // namespace

std::expected<void, std::string> ShareVConsolePort(const std::filesystem::path& vconcomm_dll) {
  // The engine loads this same module later and keeps it for the process
  // lifetime, so this load reference is never released.
  HMODULE module = LoadLibraryExW(vconcomm_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) {
    return std::unexpected("VConsole port sharing: loading " + vconcomm_dll.string() +
                           " failed with error code " + std::to_string(GetLastError()));
  }
  auto original =
      PatchImport(module, "ws2_32.dll", "bind", kBindOrdinal, reinterpret_cast<void*>(&SharedBind));
  if (!original) return std::unexpected("VConsole port sharing: vconcomm.dll: " + original.error());
  original_bind = reinterpret_cast<BindFn>(*original);
  return {};
}

}  // namespace modlock::host_app
