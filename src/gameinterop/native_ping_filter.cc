#include "modlock/gameinterop/native_ping_filter.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <algorithm>
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
// g_serialize_slot and g_unserialize_slot are INetworkMessages'
// SerializeAbstract and UnserializeAbstract resolved indexes.
size_t g_serialize_slot = 0;
size_t g_unserialize_slot = 0;
NativePingFilter::Handler* g_handler = nullptr;
DWORD g_thread = 0;

// Preserve call-through for a dispatch already entering the old thunk.
void ClearThunkState() {
  g_handler = nullptr;
  g_messages = nullptr;
  g_thread = 0;
}

// kMaxRecipientWords bounds the recipient mask a post may carry, one bit a
// client slot.
constexpr int32_t kMaxRecipientWords = 4;

// PingPost is one ping post the filter forwards, with the recipients' mask copied
// so the filter can split it.
struct PingPost {
  void* self;
  int32_t split_screen;
  bool local_only;
  int32_t clients;
  std::array<uint64_t, kMaxRecipientWords> recipients{};
  void* serializer;
  const void* message;
  unsigned long size;
  int32_t buffer;

  void Forward(const void* native_message) const {
    g_original(self, split_screen, local_only, clients, recipients.data(), serializer,
               native_message, size, buffer);
  }
};

// Filter reads a posted ping, asks the handler what to do with it and posts
// what the handler leaves: nothing for a dropped ping, the original to the
// recipients who hear it and a copy without the voice line to those muted.
// Any other message, a ping it cannot read, or one from a player who is not a
// connected human goes through unchanged, as does every recipient when the
// voiceless copy cannot be made, since a voice line is better than no ping.
void Filter(PingPost post) {
  const auto forward = [&] {
    post.Forward(post.message);
  };
  if (!g_handler || GetCurrentThreadId() != g_thread || !post.serializer || !post.message) {
    return forward();
  }
  const auto id = NetworkMessageIdOf(post.serializer);
  if (!KindOf(id)) {
    return forward();
  }
  std::array<uint8_t, kMaxUserMessageSize> bytes{};
  const auto size = SerializeNetworkMessage(g_messages, g_serialize_slot, post.message, bytes);
  if (!size) {
    std::fprintf(stderr, "[modlock] native ping serialization failed\n");
    return forward();
  }
  const auto wire = std::span(bytes.data(), *size);
  const auto ping = NativePingFilter::Decode(id, wire);
  if (!ping) {
    return forward();
  }
  const auto sender = ConnectionTracker::StateForSlot(ping->slot);
  if (!sender.occupied || !sender.fully_connected || sender.is_bot || !sender.xuid) {
    return forward();
  }
  uint64_t voice_muted = 0;
  if ((*g_handler)(*ping, voice_muted)) {
    return;
  }
  const uint64_t muted = post.recipients[0] & voice_muted;
  if (!muted) {
    return forward();
  }

  // Build the voiceless copy before splitting, so a failure leaves the ping
  // whole for everyone.
  const auto voiceless = NativePingFilter::WithoutVoice(id, wire);
  const auto copy =
      voiceless ? ParseNetworkMessage(g_messages, g_unserialize_slot, post.serializer, *voiceless)
                : nullptr;
  if (!copy) {
    std::fprintf(stderr, "[modlock] native ping voice removal failed\n");
    return forward();
  }
  auto quiet = post;
  quiet.recipients = {};
  quiet.recipients[0] = muted;
  quiet.Forward(copy.get());
  post.recipients[0] &= ~muted;
  if (std::ranges::any_of(post.recipients, [](uint64_t word) { return word != 0; })) {
    forward();
  }
}

void PostThunk(void* self, int32_t split_screen, bool local_only, int32_t clients,
               const uint64_t* recipients, void* serializer, const void* message,
               unsigned long size, int32_t buffer) {
  // Decide while the native message remains borrowed. A post whose mask this
  // filter cannot copy goes through untouched.
  const int32_t words = clients > 0 ? (clients + 63) / 64 : 0;
  if (!recipients || words == 0 || words > kMaxRecipientWords) {
    g_original(self, split_screen, local_only, clients, recipients, serializer, message, size,
               buffer);
    return;
  }
  PingPost post{self, split_screen, local_only, clients, {}, serializer, message, size, buffer};
  std::copy_n(recipients, words, post.recipients.begin());
  Filter(post);
}
#endif

}  // namespace

namespace {

// ParsePing reads one length-prefixed ping user message whose common data
// names a player slot as its sender.
std::optional<engine::PingMessage> ParsePing(int32_t message_id,
                                             std::span<const uint8_t> serialized) {
  if (!KindOf(message_id) || serialized.empty() || serialized.size() > kMaxUserMessageSize) {
    return std::nullopt;
  }
  google::protobuf::io::CodedInputStream input(serialized.data(),
                                               static_cast<int>(serialized.size()));
  uint32_t size = 0;
  if (!input.ReadVarint32(&size) || size != serialized.size() - input.CurrentPosition()) {
    return std::nullopt;
  }
  engine::PingMessage message;
  if (!message.ParseFromArray(serialized.data() + input.CurrentPosition(),
                              static_cast<int>(size)) ||
      !IsPlayerSlot(message.ping_data().sender_player_slot())) {
    return std::nullopt;
  }
  return message;
}

}  // namespace

std::optional<NativePingFilter::Ping> NativePingFilter::Decode(
    int32_t message_id, std::span<const uint8_t> serialized) {
  const auto message = ParsePing(message_id, serialized);
  if (!message) {
    return std::nullopt;
  }
  const auto kind = *KindOf(message_id);
  return Ping{
      .slot = message->ping_data().sender_player_slot(),
      .kind = kind,
      .option = kind == Kind::kWheel ? message->ping_wheel_option_id() : 0,
      .concept_value = message->ping_data().speech_concept(),
  };
}

std::optional<std::vector<uint8_t>> NativePingFilter::WithoutVoice(
    int32_t message_id, std::span<const uint8_t> serialized) {
  // Clear the line; the parsed message keeps the fields it does not name.
  auto message = ParsePing(message_id, serialized);
  if (!message) {
    return std::nullopt;
  }
  message->mutable_ping_data()->clear_speech_concept();
  message->mutable_ping_data()->clear_response_chosen();

  // Frame it with its length again.
  std::string wire;
  {
    google::protobuf::io::StringOutputStream stream(&wire);
    google::protobuf::io::CodedOutputStream output(&stream);
    output.WriteVarint32(static_cast<uint32_t>(message->ByteSizeLong()));
    if (!message->SerializeToCodedStream(&output) || output.HadError()) {
      return std::nullopt;
    }
  }
  return std::vector<uint8_t>(wire.begin(), wire.end());
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
  const auto unserialize_slot = ResolveVirtualSlot("network-messages.unserialize");
  if (!unserialize_slot) {
    return std::unexpected(unserialize_slot.error());
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
  g_unserialize_slot = *unserialize_slot;
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
