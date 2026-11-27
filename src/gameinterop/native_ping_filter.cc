#include "modlock/gameinterop/native_ping_filter.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "modlock/gameinterop/connection_tracker.h"
#include "native_network_messages.h"
#include "src/gameinterop/engine/ping.pb.h"

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

// kPingIds are the messages the filter registers for.
constexpr std::array<int32_t, 4> kPingIds = {kMapPingMessageId, kPingWheelMessageId,
                                             kQuickResponseMessageId, kAbilityPingMessageId};

// Filter reads a posted ping, asks the handler what to do with it and posts
// what the handler leaves: nothing for a dropped ping, the original to the
// recipients who hear it and a copy without the voice line to those muted.
// A ping it cannot read, or one from a player who is not a connected human,
// goes through unchanged, as does every recipient when the voiceless copy
// cannot be made, since a voice line is better than no ping.
void Filter(const NativePingFilter::Handler& handler, int32_t id,
            const NativePostFilter::Post& post) {
  const auto forward = [&] {
    post.Forward(post.recipients());
  };
  std::array<uint8_t, kMaxUserMessageSize> bytes{};
  const auto wire = post.Serialize(bytes);
  if (!wire) {
    std::fprintf(stderr, "[modlock] native ping serialization failed\n");
    return forward();
  }
  const auto ping = NativePingFilter::Decode(id, *wire);
  if (!ping) {
    return forward();
  }
  const auto sender = ConnectionTracker::StateForSlot(ping->slot);
  if (!sender.occupied || !sender.fully_connected || sender.is_bot || !sender.xuid) {
    return forward();
  }
  uint64_t voice_muted = 0;
  if (handler(*ping, voice_muted)) {
    return;
  }
  const uint64_t muted = post.recipients()[0] & voice_muted;
  if (!muted) {
    return forward();
  }

  // Post the voiceless copy before the rest, so a failure leaves the ping
  // whole for everyone.
  const auto voiceless = NativePingFilter::WithoutVoice(id, *wire);
  NativePostFilter::Recipients quiet{};
  quiet[0] = muted;
  if (!voiceless || !post.ForwardCopy(quiet, *voiceless)) {
    std::fprintf(stderr, "[modlock] native ping voice removal failed\n");
    return forward();
  }
  auto rest = post.recipients();
  rest[0] &= ~muted;
  if (std::ranges::any_of(rest, [](uint64_t word) { return word != 0; })) {
    post.Forward(rest);
  }
}

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
  NativePostFilter* posts = nullptr;
  std::vector<int32_t> ids;

  // Unregister before the handler the filters borrow is destroyed.
  ~Impl() {
    for (const auto id : ids) {
      posts->Unregister(id);
    }
  }
};

NativePingFilter::NativePingFilter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NativePingFilter::NativePingFilter(NativePingFilter&&) noexcept = default;
NativePingFilter& NativePingFilter::operator=(NativePingFilter&&) noexcept = default;
NativePingFilter::~NativePingFilter() = default;

std::expected<NativePingFilter, std::string> NativePingFilter::Install(NativePostFilter& posts,
                                                                       Handler handler) {
  if (!handler) {
    return std::unexpected("native ping filter handler is empty");
  }
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  impl->posts = &posts;
  for (const auto id : kPingIds) {
    auto registered = posts.Register(
        id, [handler = &impl->handler](int32_t id, const NativePostFilter::Post& post) {
          Filter(*handler, id, post);
        });
    if (!registered) {
      return std::unexpected(registered.error());
    }
    impl->ids.push_back(id);
  }
  return NativePingFilter(std::move(impl));
}

}  // namespace modlock::gameinterop
