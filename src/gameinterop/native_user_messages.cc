#include "modlock/gameinterop/native_user_messages.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"
#include "proto/modlock/chat.pb.h"
#include "proto/modlock/hud.pb.h"

namespace modlock::gameinterop {
namespace {

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
  return Send(slot, 314, message);
}

std::expected<void, std::string> NativeUserMessages::CenterText(int32_t slot,
                                                                std::string_view text) const {
  engine::HudTextMessage message;
  message.set_dest(4);
  message.add_param(text);
  return Send(slot, 124, message);
}

std::expected<void, std::string> NativeUserMessages::Announce(int32_t slot, std::string_view title,
                                                              std::string_view description) const {
  engine::HudAnnouncement message;
  message.set_title_locstring(title);
  message.set_description_locstring(description);
  return Send(slot, 363, message);
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
  return Send(slot, 332, message);
}

std::expected<void, std::string> NativeUserMessages::ClearScreenEffect(
    int32_t slot, int32_t owner, engine::ScreenEffectState state) const {
  // Clear the owner's effect in this state.
  engine::ScreenEffect message;
  message.set_entindex_owner(owner);
  message.set_clear_all_states(true);
  message.set_state(state);
  return Send(slot, 332, message);
}

std::expected<void, std::string> NativeUserMessages::Send(
    int32_t slot, int32_t id, const google::protobuf::MessageLite& message) const {
  // Refuse a bad slot or size.
  if (slot < 0 || slot >= 64) {
    return std::unexpected("native user recipient slot is invalid");
  }
  const auto size = message.ByteSizeLong();
  if (size == 0 || size > 4096) {
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

  // Post it to the one recipient.
  const uint64_t recipients = uint64_t{1} << slot;
  const auto event_methods = *static_cast<void***>(events_);
  using Post = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                        unsigned long, int32_t);

  // BUF_RELIABLE=1. PostEventAbstract copies the message before returning.
  // The client count is the mask's bit extent, including unselected lower slots.
  reinterpret_cast<Post>(event_methods[*post_slot])(events_, -1, false, slot + 1, &recipients,
                                                    serializer, native_message, 0, 1);
  return {};
#else
  (void)id;
  return std::unexpected("native user messages require the Windows host");
#endif
}

}  // namespace modlock::gameinterop
