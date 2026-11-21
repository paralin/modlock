#include "modlock/gameinterop/native_ping_filter.h"

#include <google/protobuf/io/coded_stream.h>

#include <array>
#include <cstdio>
#include <optional>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"
#include "src/gameinterop/engine/ping.pb.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

// KindOf names a ping message id, or nothing for any other message.
std::optional<NativePingFilter::Kind> KindOf(int32_t id) {
  switch (id) {
    case kMapPingMessageId:
      return NativePingFilter::Kind::kMap;
    case kPingWheelMessageId:
      return NativePingFilter::Kind::kWheel;
    case kQuickResponseMessageId:
      return NativePingFilter::Kind::kQuickResponse;
    case kAbilityPingMessageId:
      return NativePingFilter::Kind::kAbility;
    default:
      return std::nullopt;
  }
}

#if defined(_WIN32)
using Post = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                      unsigned long, int32_t);
Post g_original = nullptr;
void* g_messages = nullptr;
// g_serialize_slot is INetworkMessages::SerializeAbstract's resolved index.
size_t g_serialize_slot = 0;
NativePingFilter::Handler* g_handler = nullptr;
DWORD g_thread = 0;

// Preserve call-through for a dispatch already entering the old thunk.
void ClearThunkState() {
  g_handler = nullptr;
  g_messages = nullptr;
  g_thread = 0;
}

// Dropped reads a posted ping and asks the handler whether to drop it. Any
// other message, a ping it cannot read, or one from a player who is not a
// connected human goes through.
bool Dropped(void* serializer, const void* native_message) {
  if (!g_handler || GetCurrentThreadId() != g_thread || !serializer || !native_message) {
    return false;
  }
  const auto id = NetworkMessageIdOf(serializer);
  if (!KindOf(id)) {
    return false;
  }
  std::array<uint8_t, kMaxUserMessageSize> bytes{};
  const auto size = SerializeNetworkMessage(g_messages, g_serialize_slot, native_message, bytes);
  if (!size) {
    std::fprintf(stderr, "[modlock] native ping serialization failed\n");
    return false;
  }
  const auto ping = NativePingFilter::Decode(id, std::span(bytes.data(), *size));
  if (!ping) {
    return false;
  }
  const auto sender = ConnectionTracker::StateForSlot(ping->slot);
  if (!sender.occupied || !sender.fully_connected || sender.is_bot || !sender.xuid) {
    return false;
  }
  return (*g_handler)(*ping);
}

void PostThunk(void* self, int32_t split_screen, bool local_only, int32_t clients,
               const uint64_t* recipients, void* serializer, const void* message,
               unsigned long size, int32_t buffer) {
  // Decide while the native message remains borrowed; a dropped ping never
  // reaches the engine's post.
  if (Dropped(serializer, message)) {
    return;
  }
  g_original(self, split_screen, local_only, clients, recipients, serializer, message, size,
             buffer);
}
#endif

}  // namespace

std::optional<NativePingFilter::Ping> NativePingFilter::Decode(
    int32_t message_id, std::span<const uint8_t> serialized) {
  // Read the length prefix of one ping message.
  const auto kind = KindOf(message_id);
  if (!kind || serialized.empty() || serialized.size() > kMaxUserMessageSize) {
    return std::nullopt;
  }
  google::protobuf::io::CodedInputStream input(serialized.data(),
                                               static_cast<int>(serialized.size()));
  uint32_t size = 0;
  if (!input.ReadVarint32(&size) || size != serialized.size() - input.CurrentPosition()) {
    return std::nullopt;
  }

  // Every ping names its sender in its common data.
  engine::PingMessage message;
  if (!message.ParseFromArray(serialized.data() + input.CurrentPosition(),
                              static_cast<int>(size)) ||
      !IsPlayerSlot(message.ping_data().sender_player_slot())) {
    return std::nullopt;
  }
  return Ping{
      .slot = message.ping_data().sender_player_slot(),
      .kind = *kind,
      .option = *kind == Kind::kWheel ? message.ping_wheel_option_id() : 0,
      .concept_value = message.ping_data().speech_concept(),
  };
}

struct NativePingFilter::Impl {
  Handler handler;
  // Restore dispatch and release the borrowed handler before destroying it.
  std::optional<ThunkOwner> owner;
};

NativePingFilter::NativePingFilter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NativePingFilter::NativePingFilter(NativePingFilter&&) noexcept = default;
NativePingFilter& NativePingFilter::operator=(NativePingFilter&&) noexcept = default;
NativePingFilter::~NativePingFilter() = default;

std::expected<NativePingFilter, std::string> NativePingFilter::Install(Handler handler) {
#if defined(_WIN32)
  // Install once, resolving the event system and the message serializer.
  if (g_handler || !handler) {
    return std::unexpected("native ping filter already installed or empty");
  }
  const auto events = ResolveEngineInterface(L"engine2.dll", "GameEventSystemServerV001");
  if (!events) {
    return std::unexpected(events.error());
  }
  const auto messages = ResolveEngineInterface(L"networksystem.dll", "NetworkMessagesVersion001");
  if (!messages) {
    return std::unexpected(messages.error());
  }
  const auto post_slot = ResolveVirtualSlot("game-event-system.post-event");
  if (!post_slot) {
    return std::unexpected(post_slot.error());
  }
  const auto serialize_slot = ResolveVirtualSlot("network-messages.serialize");
  if (!serialize_slot) {
    return std::unexpected(serialize_slot.error());
  }

  // Hook the post slot and keep what the thunk needs.
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  auto hook = VtableSlotHook::Install(*events, *post_slot, reinterpret_cast<void*>(&PostThunk));
  if (!hook) {
    return std::unexpected(hook.error());
  }
  g_original = reinterpret_cast<Post>(hook->Original());
  g_messages = *messages;
  g_serialize_slot = *serialize_slot;
  impl->owner.emplace(std::move(*hook), &ClearThunkState);

  // Route pings on this thread to the handler.
  g_thread = GetCurrentThreadId();
  g_handler = &impl->handler;
  return NativePingFilter(std::move(impl));
#else
  (void)handler;
  return std::unexpected("the native ping filter requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
