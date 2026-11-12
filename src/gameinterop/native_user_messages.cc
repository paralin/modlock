#include "modlock/gameinterop/native_user_messages.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"
#include "src/gameinterop/engine/camera.pb.h"
#include "src/gameinterop/engine/chat.pb.h"
#include "src/gameinterop/engine/hud.pb.h"
#include "src/gameinterop/engine/sound.pb.h"

namespace modlock::gameinterop {
namespace {

// kHudPrintCenter is HUD_PRINTCENTER, the text message destination that
// centers the text on screen.
constexpr int32_t kHudPrintCenter = 4;

// MurmurHash2 is the hash the sound system names sound events and their
// fields by, over the lowercased bytes of the name.
uint32_t MurmurHash2(std::string_view text, uint32_t seed) {
  constexpr uint32_t m = 0x5bd1e995;
  const auto byte = [&](size_t i) {
    const auto c = static_cast<uint8_t>(text[i]);
    return static_cast<uint32_t>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  };
  uint32_t h = seed ^ static_cast<uint32_t>(text.size());
  size_t i = 0;
  for (; text.size() - i >= 4; i += 4) {
    uint32_t k = byte(i) | byte(i + 1) << 8 | byte(i + 2) << 16 | byte(i + 3) << 24;
    k *= m;
    k ^= k >> 24;
    k *= m;
    h = h * m ^ k;
  }
  switch (text.size() - i) {
    case 3:
      h ^= byte(i + 2) << 16;
      [[fallthrough]];
    case 2:
      h ^= byte(i + 1) << 8;
      [[fallthrough]];
    case 1:
      h ^= byte(i);
      h *= m;
  }
  h ^= h >> 13;
  h *= m;
  return h ^ h >> 15;
}

// kSoundNameSeed and kSoundFieldSeed seed the hashes of sound event names
// and of their field names.
constexpr uint32_t kSoundNameSeed = 0x53524332;
constexpr uint32_t kSoundFieldSeed = 0x31415926;

// next_sound_id allocates sound ids. The game numbers its own sounds upward
// from 1 and wraps below 2^31, so starting at 2^30 keeps ours apart from
// every sound a session plays; the game's count would need a billion sounds
// to reach ours.
std::atomic<uint32_t> next_sound_id{uint32_t{1} << 30};

// kSoundFloatField is the packed field type of a float.
constexpr uint8_t kSoundFloatField = 8;

// PackSoundFields packs the present fields as the sound system reads them:
// per field, its name hash, its type, its payload size, a zero byte and the
// payload, all little-endian.
std::string PackSoundFields(const NativeUserMessages::SoundFields& fields) {
  std::string packed;
  const auto add = [&](std::string_view name, std::optional<float> value) {
    if (!value) {
      return;
    }
    const uint32_t hash = MurmurHash2(name, kSoundFieldSeed);
    const auto bits = std::bit_cast<uint32_t>(*value);
    for (int shift = 0; shift < 32; shift += 8) {
      packed.push_back(static_cast<char>(hash >> shift));
    }
    packed.push_back(static_cast<char>(kSoundFloatField));
    packed.push_back(static_cast<char>(sizeof(bits)));
    packed.push_back(0);
    for (int shift = 0; shift < 32; shift += 8) {
      packed.push_back(static_cast<char>(bits >> shift));
    }
  };
  // The stock stacks multiply both factors into the event's own level and
  // pitch, and no event sets them, so 1 leaves the sound as authored.
  // public.volume and public.pitch would replace the authored values, and
  // public.volume is in decibels.
  add("public.volume_mult", fields.volume);
  add("public.pitch_adjust", fields.pitch);
  return packed;
}

#if defined(_WIN32)

// BitReader matches the installed networksystem's bf_read. Unserialize checks
// data bits at +12 and cursor at +16 before reading the length-prefixed proto.
// The native serializer and message own protobuf allocation, parsing, and
// deallocation.
struct BitReader {
  const uint8_t* data;
  int32_t bytes;
  int32_t bits;
  int32_t cursor = 0;
  const char* debug_name = "modlock user message";
  bool overflow = false;
  bool assert_overflow = false;
};

static_assert(offsetof(BitReader, cursor) == 16);
static_assert(offsetof(BitReader, debug_name) == 24);
static_assert(sizeof(BitReader) == 40);

#endif

}  // namespace

std::expected<NativeUserMessages, std::string> NativeUserMessages::TryCreate() {
  // Find the network messages and game event interfaces.
  const auto messages = ResolveEngineInterface(L"networksystem.dll", "NetworkMessagesVersion001");
  if (!messages) {
    return std::unexpected(messages.error());
  }
  const auto events = ResolveEngineInterface(L"engine2.dll", "GameEventSystemServerV001");
  if (!events) {
    return std::unexpected(events.error());
  }
  return NativeUserMessages(*messages, *events);
}

std::expected<void, std::string> NativeUserMessages::Chat(int32_t slot,
                                                          std::string_view text) const {
  // Send all chat from no player.
  engine::ChatMessage message;
  message.set_player_slot(-1);
  message.set_text(text);
  message.set_all_chat(true);
  return Send(slot, kChatMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::CenterText(int32_t slot,
                                                                std::string_view text) const {
  engine::HudTextMessage message;
  message.set_dest(kHudPrintCenter);
  message.add_param(text);
  return Send(slot, kHudTextMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::Announce(int32_t slot, std::string_view title,
                                                              std::string_view description) const {
  engine::HudAnnouncement message;
  message.set_title_locstring(title);
  message.set_description_locstring(description);
  return Send(slot, kHudAnnouncementMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::ScreenEffect(
    int32_t slot, int32_t owner, engine::ScreenEffectState state,
    const ScreenEffectTiming& timing) const {
  // Name the effect's owner and state.
  engine::ScreenEffect message;
  message.set_entindex_owner(owner);
  message.set_state(state);

  // Time and scale it, then send it.
  message.set_delay(timing.delay);
  message.set_fade_in_time(timing.fade_in);
  message.set_hold_time(timing.hold);
  message.set_fade_out_time(timing.fade_out);
  message.set_scale(timing.scale);
  return Send(slot, kScreenEffectMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::ClearScreenEffect(
    int32_t slot, int32_t owner, engine::ScreenEffectState state) const {
  // Clear the owner's effect in this state.
  engine::ScreenEffect message;
  message.set_entindex_owner(owner);
  message.set_clear_all_states(true);
  message.set_state(state);
  return Send(slot, kScreenEffectMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::CameraController(
    int32_t slot, const engine::CameraController& message) const {
  return Send(slot, kCameraControllerMessageId, message);
}

std::expected<uint32_t, std::string> NativeUserMessages::StartSound(
    uint64_t listeners, std::string_view name, int32_t source, const SoundFields& fields) const {
  // The id also seeds the sound's random choices, as the game's own do.
  const uint32_t id = next_sound_id.fetch_add(1, std::memory_order_relaxed);
  engine::SoundStart message;
  message.set_soundevent_guid(static_cast<int32_t>(id));
  message.set_soundevent_hash(MurmurHash2(name, kSoundNameSeed));
  message.set_source_entity_index(source);
  message.set_seed(static_cast<int32_t>(id));
  if (auto packed = PackSoundFields(fields); !packed.empty()) {
    message.set_packed_params(std::move(packed));
  }
  if (auto posted = Post(listeners, kSoundStartMessageId, message); !posted) {
    return std::unexpected(posted.error());
  }
  return id;
}

std::expected<void, std::string> NativeUserMessages::SetSoundFields(
    uint64_t listeners, uint32_t id, const SoundFields& fields) const {
  engine::SoundParams message;
  message.set_soundevent_guid(static_cast<int32_t>(id));
  message.set_packed_params(PackSoundFields(fields));
  return Post(listeners, kSoundParamsMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::StopSound(uint64_t listeners,
                                                               uint32_t id) const {
  engine::SoundStop message;
  message.set_soundevent_guid(static_cast<int32_t>(id));
  return Post(listeners, kSoundStopMessageId, message);
}

std::expected<void, std::string> NativeUserMessages::Send(
    int32_t slot, int32_t id, const google::protobuf::MessageLite& message) const {
  if (!IsPlayerSlot(slot)) {
    return std::unexpected("native user recipient slot is invalid");
  }
  return Post(uint64_t{1} << slot, id, message);
}

std::expected<void, std::string> NativeUserMessages::Post(
    uint64_t recipients, int32_t id, const google::protobuf::MessageLite& message) const {
  // A message to nobody is already delivered; refuse a bad size.
  if (recipients == 0) {
    return {};
  }
  const auto size = message.ByteSizeLong();
  if (size == 0 || size > kMaxUserMessageSize) {
    return std::unexpected("native user message size is invalid");
  }

#if defined(_WIN32)
  // Find the three engine slots.
  const auto find_slot = ResolveVirtualSlot("network-messages.find-by-id");
  const auto unserialize_slot = ResolveVirtualSlot("network-messages.unserialize");
  const auto post_slot = ResolveVirtualSlot("game-event-system.post-event");
  if (!find_slot) {
    return std::unexpected(find_slot.error());
  }
  if (!unserialize_slot) {
    return std::unexpected(unserialize_slot.error());
  }
  if (!post_slot) {
    return std::unexpected(post_slot.error());
  }

  // The installed engine compares all 32 bits of the message ID. A 16-bit
  // declaration leaves the upper argument bits unspecified on Windows x64.
  using Find = void* (*)(void*, int32_t);
  auto* serializer = NetworkMessagesMethod<Find>(messages_, *find_slot)(messages_, id);
  if (!serializer) {
    return std::unexpected("native user message serializer unavailable");
  }

  // Allocate the engine's message.
  using Allocate = void* (*)();
  Allocate allocate = nullptr;
  std::memcpy(&allocate, static_cast<std::byte*>(serializer) + kSerializerAllocateOffset,
              sizeof(allocate));
  if (!allocate) {
    return std::unexpected("native user message allocator unavailable");
  }
  auto* native_message = allocate();
  if (!native_message) {
    return std::unexpected("native user message allocation failed");
  }

  // The message's scalar deleting destructor frees it with the game's allocator.
  const auto release = [](void* value) {
    using Destroy = void* (*)(void*, uint32_t);
    reinterpret_cast<Destroy>((*static_cast<void***>(value))[0])(value, 1);
  };
  const std::unique_ptr<void, decltype(release)> owned_message(native_message, release);

  // Encode the message after its length.
  std::string encoded;
  {
    google::protobuf::io::StringOutputStream output(&encoded);
    google::protobuf::io::CodedOutputStream coded(&output);
    coded.WriteVarint32(static_cast<uint32_t>(size));
    if (!message.SerializeToCodedStream(&coded) || coded.HadError()) {
      return std::unexpected("native user message serialization failed");
    }
  }

  // The bit reader can fetch the final word; padding is addressable but is not
  // part of the declared stream length.
  std::vector<uint8_t> bytes(encoded.size() + 4);
  std::memcpy(bytes.data(), encoded.data(), encoded.size());
  BitReader reader{bytes.data(), static_cast<int32_t>(encoded.size()),
                   static_cast<int32_t>(encoded.size() * 8)};
  using Unserialize = bool (*)(void*, BitReader&, void*, void*);
  if (!NetworkMessagesMethod<Unserialize>(messages_, *unserialize_slot)(messages_, reader,
                                                                        native_message, nullptr) ||
      reader.overflow || reader.cursor != reader.bits) {
    return std::unexpected("native user message decoding failed");
  }

  // Post it to the recipients.
  const auto event_methods = *static_cast<void***>(events_);
  using PostEvent = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                             unsigned long, int32_t);

  // BUF_RELIABLE=1. PostEventAbstract copies the message before returning.
  // The client count is the mask's bit extent, including unselected lower slots.
  const auto clients = static_cast<int32_t>(std::bit_width(recipients));
  reinterpret_cast<PostEvent>(event_methods[*post_slot])(events_, -1, false, clients, &recipients,
                                                         serializer, native_message, 0, 1);
  return {};
#else
  (void)id;
  return std::unexpected("native user messages require the Windows host");
#endif
}

}  // namespace modlock::gameinterop
