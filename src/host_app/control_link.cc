#include "host_app/control_link.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <iostream>
#include <utility>

namespace modlock::host_app {
namespace {

// kMaxPacket bounds one framed message.
constexpr uint32_t kMaxPacket = 10'000'000;

// kMaxEvents bounds the events waiting for a slow controller; later events
// are dropped.
constexpr size_t kMaxEvents = 10'000;

#if defined(_WIN32)
constexpr intptr_t kNoSocket = static_cast<intptr_t>(INVALID_SOCKET);

void CloseSocket(intptr_t socket) { closesocket(static_cast<SOCKET>(socket)); }

void ShutdownSocket(intptr_t socket) { shutdown(static_cast<SOCKET>(socket), SD_BOTH); }
#else
constexpr intptr_t kNoSocket = -1;

void CloseSocket(intptr_t socket) { close(static_cast<int>(socket)); }

void ShutdownSocket(intptr_t socket) { shutdown(static_cast<int>(socket), SHUT_RDWR); }
#endif

// SendAll writes every byte of data, or returns false when the connection
// ended.
bool SendAll(intptr_t socket, const char* data, size_t size) {
  while (size > 0) {
#if defined(_WIN32)
    const int sent = send(static_cast<SOCKET>(socket), data, static_cast<int>(size), 0);
#else
    const ssize_t sent = send(static_cast<int>(socket), data, size, MSG_NOSIGNAL);
#endif
    if (sent <= 0) return false;
    data += sent;
    size -= static_cast<size_t>(sent);
  }
  return true;
}

// ReceiveAll reads exactly size bytes, or returns false when the connection
// ended.
bool ReceiveAll(intptr_t socket, char* data, size_t size) {
  while (size > 0) {
#if defined(_WIN32)
    const int received = recv(static_cast<SOCKET>(socket), data, static_cast<int>(size), 0);
#else
    const ssize_t received = recv(static_cast<int>(socket), data, size, 0);
#endif
    if (received <= 0) return false;
    data += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

// Connect opens a TCP connection to address, a host and port.
std::expected<intptr_t, std::string> Connect(std::string_view address) {
  const auto colon = address.rfind(':');
  if (colon == std::string_view::npos) {
    return std::unexpected("the control address must be a host and port");
  }
  const std::string host(address.substr(0, colon));
  const std::string port(address.substr(colon + 1));
#if defined(_WIN32)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return std::unexpected("cannot start Winsock");
#endif

  // Try each address the name resolves to.
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) {
    return std::unexpected("cannot resolve the control address " + std::string(address));
  }
  intptr_t connected = kNoSocket;
  for (auto* candidate = found; candidate && connected == kNoSocket;
       candidate = candidate->ai_next) {
    const auto socket = static_cast<intptr_t>(
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
    if (socket == kNoSocket) continue;
    if (connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      connected = socket;
    } else {
      CloseSocket(socket);
    }
  }
  freeaddrinfo(found);
  if (connected == kNoSocket) {
    return std::unexpected("cannot connect to the controller at " + std::string(address));
  }
  return connected;
}

}  // namespace

ControlLink::ControlLink(intptr_t socket)
    : socket_(socket),
      reader_([this] { Read(); }),
      sender_([this](std::stop_token stop) { Send(stop); }) {}

ControlLink::~ControlLink() {
  // Unblock the reader and stop the sender before closing the connection.
  ShutdownSocket(socket_);
  reader_.join();
  sender_.request_stop();
  sender_.join();
  CloseSocket(socket_);
}

std::expected<std::unique_ptr<ControlLink>, std::string> ControlLink::Dial(
    std::string_view address) {
  auto socket = Connect(address);
  if (!socket) return std::unexpected(socket.error());
  return std::unique_ptr<ControlLink>(new ControlLink(*socket));
}

std::expected<void, std::string> ControlLink::Watch(EngineHost& engine) {
  // Player joins need the network system, which loads with the first world.
  auto world = engine.OnWorld(nullptr, [this, &engine](std::string_view map) {
    control::HostEvent event;
    event.mutable_ready()->set_map(std::string(map));
    Push(std::move(event));
    if (joining_) return;
    // The link owns this subscription, so the sink does not own the link.
    auto connections = engine.OnConnection(
        std::shared_ptr<gameinterop::ConnectionEventSink>(std::shared_ptr<void>(), this));
    if (!connections) {
      std::cerr << "control: player joins are unavailable: " << connections.error() << '\n';
      return;
    }
    connections_ = std::move(*connections);
    joining_ = true;
  });
  if (!world) return std::unexpected(world.error());
  world_ = std::move(*world);
  return {};
}

void ControlLink::RunRequests(WasmHost& mods) {
  if (!pending_.load(std::memory_order_acquire)) return;
  std::vector<control::ControlRequest> requests;
  {
    std::lock_guard lock(mu_);
    requests.swap(requests_);
    pending_.store(false, std::memory_order_release);
  }

  for (const auto& request : requests) {
    switch (request.body_case()) {
      case control::ControlRequest::kReload: {
        // Report a reload that did not load; the previous build keeps running.
        const auto& path = request.reload().path();
        if (auto reloaded = mods.Reload(path); !reloaded) {
          std::cerr << "control: " << reloaded.error() << '\n';
          control::HostEvent event;
          event.mutable_failed()->set_mod(path);
          event.mutable_failed()->set_error(reloaded.error());
          Push(std::move(event));
        }
        break;
      }
      default:
        break;
    }
  }
}

void ControlLink::Started(std::string_view mod, bool reloaded) {
  control::HostEvent event;
  event.mutable_started()->set_mod(std::string(mod));
  event.mutable_started()->set_reloaded(reloaded);
  Push(std::move(event));
}

void ControlLink::Logged(std::string_view mod, std::string_view text) {
  control::HostEvent event;
  event.mutable_log()->set_mod(std::string(mod));
  event.mutable_log()->set_text(std::string(text));
  Push(std::move(event));
}

void ControlLink::Failed(std::string_view mod, std::string_view error) {
  control::HostEvent event;
  event.mutable_failed()->set_mod(std::string(mod));
  event.mutable_failed()->set_error(std::string(error));
  Push(std::move(event));
}

void ControlLink::Ui(std::string_view mod, int32_t slot, const ui::Change& change) {
  control::HostEvent event;
  auto* ui = event.mutable_ui();
  ui->set_mod(std::string(mod));
  ui->set_slot(slot);
  *ui->mutable_change() = change;
  Push(std::move(event));
}

void ControlLink::OnConnected(int32_t slot, uint64_t xuid, bool bot, const char* name) {
  if (bot) return;
  control::HostEvent event;
  auto* joined = event.mutable_joined();
  joined->set_slot(slot);
  joined->set_name(name ? name : "");
  joined->set_steam_id(xuid);
  Push(std::move(event));
}

void ControlLink::OnDisconnecting(int32_t slot, uint64_t) {
  control::HostEvent event;
  event.mutable_left()->set_slot(slot);
  Push(std::move(event));
}

void ControlLink::Push(control::HostEvent event) {
  {
    std::lock_guard lock(mu_);
    if (closed_ || events_.size() >= kMaxEvents) return;
    events_.push_back(std::move(event));
  }
  wake_.notify_one();
}

void ControlLink::Send(std::stop_token stop) {
  // Send events in order until the link closes or the host stops.
  std::unique_lock lock(mu_);
  while (wake_.wait(lock, stop, [this] { return !events_.empty() || closed_; }) &&
         !closed_) {
    auto event = std::move(events_.front());
    events_.pop_front();
    lock.unlock();
    const auto data = event.SerializeAsString();
    const auto size = static_cast<uint32_t>(data.size());
    const std::array<char, 4> header{static_cast<char>(size), static_cast<char>(size >> 8),
                                     static_cast<char>(size >> 16), static_cast<char>(size >> 24)};
    const bool sent = SendAll(socket_, header.data(), header.size()) &&
                      SendAll(socket_, data.data(), data.size());
    lock.lock();
    if (!sent) closed_ = true;
  }
  events_.clear();
}

void ControlLink::Read() {
  // Queue each request for the engine thread until the controller disconnects.
  std::string packet;
  for (;;) {
    std::array<unsigned char, 4> header{};
    if (!ReceiveAll(socket_, reinterpret_cast<char*>(header.data()), header.size())) break;
    const uint32_t size = static_cast<uint32_t>(header[0]) | static_cast<uint32_t>(header[1]) << 8 |
                          static_cast<uint32_t>(header[2]) << 16 |
                          static_cast<uint32_t>(header[3]) << 24;
    if (size > kMaxPacket) break;
    packet.resize(size);
    if (!ReceiveAll(socket_, packet.data(), packet.size())) break;
    control::ControlRequest request;
    if (!request.ParseFromString(packet)) break;
    std::lock_guard lock(mu_);
    requests_.push_back(std::move(request));
    pending_.store(true, std::memory_order_release);
  }
  {
    std::lock_guard lock(mu_);
    closed_ = true;
  }
  wake_.notify_one();
}

}  // namespace modlock::host_app
