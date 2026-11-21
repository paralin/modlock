#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {

// Since game build 6711 a message serializer is a plain record rather than a
// virtual class: its allocator constructs messages, the message's deleting
// destructor frees them, and INetworkMessages serializes and parses them.
// Source SDK networksystem/inetworkserializer.h.
inline constexpr size_t kSerializerAllocateOffset = 0x8;
inline constexpr size_t kSerializerMessageIdOffset = 0x18;

// The game events that start, change and stop a sound event:
// GE_SosStartSoundEvent, GE_SosStopSoundEvent and GE_SosSetSoundEventParams.
inline constexpr int16_t kSoundStartMessageId = 208;
inline constexpr int16_t kSoundStopMessageId = 209;
inline constexpr int16_t kSoundParamsMessageId = 210;

// The user message ids the framework sends or reads: CUserMessageSayText2,
// CUserMessageTextMsg, Deadlock's chat message, CameraController,
// PostProcessingAnim and HudGameAnnouncement.
inline constexpr int16_t kPlayerChatMessageId = 118;
inline constexpr int16_t kHudTextMessageId = 124;
inline constexpr int16_t kChatMessageId = 314;
inline constexpr int16_t kCameraControllerMessageId = 326;
inline constexpr int16_t kScreenEffectMessageId = 332;
inline constexpr int16_t kHudAnnouncementMessageId = 363;

// The ping user messages, each carrying PingCommonData as field 1:
// CCitadelUserMsg_MapPing, CCitadelUserMsg_PingWheel (the comms wheel, which
// the enum still names k_EUserMsg_ChatWheel), CCitadelUserMsg_QuickResponse
// and CCitadelUserMsg_AbilityPing. The server's CNetMessagePB template
// arguments give 312 and 315 to PingWheel and QuickResponse.
inline constexpr int16_t kMapPingMessageId = 303;
inline constexpr int16_t kPingWheelMessageId = 312;
inline constexpr int16_t kQuickResponseMessageId = 315;
inline constexpr int16_t kAbilityPingMessageId = 331;

// kMaxUserMessageSize bounds one serialized user message.
inline constexpr size_t kMaxUserMessageSize = 4096;

// NetworkMessageIdOf reads the 32-bit message ID from a native serializer.
inline int32_t NetworkMessageIdOf(const void* serializer) {
  int32_t id = 0;
  std::memcpy(&id, static_cast<const std::byte*>(serializer) + kSerializerMessageIdOffset,
              sizeof(id));
  return id;
}

// NetworkMessagesMethod returns one INetworkMessages virtual method.
template <typename Method>
Method NetworkMessagesMethod(void* messages, size_t slot) {
  return reinterpret_cast<Method>((*static_cast<void***>(messages))[slot]);
}

#if defined(_WIN32)
// NetworkBitWriter is the Windows bf_write of Source SDK tier1/bitbuf.h, which
// INetworkMessages::SerializeAbstract writes a message into.
struct NetworkBitWriter {
  uint8_t* data;
  int32_t bytes;
  int32_t bits;
  int32_t cursor = 0;
  const char* debug_name = "modlock";
  bool overflow = false;
  bool assert_overflow = false;
  bool aligned = true;
};

static_assert(offsetof(NetworkBitWriter, cursor) == 16);
static_assert(offsetof(NetworkBitWriter, debug_name) == 24);
static_assert(offsetof(NetworkBitWriter, overflow) == 32);
static_assert(sizeof(NetworkBitWriter) == 40);

// SerializeNetworkMessage writes one native message's wire bytes, a varint
// byte count and then the protobuf, into out through the INetworkMessages
// serialize method at serialize_slot. It returns the bytes written, or nothing
// when the serializer refuses, overflows or stops inside a byte. The engine
// serializes its own protobuf, so only bytes cross the protobuf ABI.
inline std::optional<size_t> SerializeNetworkMessage(void* messages, size_t serialize_slot,
                                                     const void* message, std::span<uint8_t> out) {
  if (!messages || !message || out.empty()) {
    return std::nullopt;
  }
  NetworkBitWriter writer{out.data(), static_cast<int32_t>(out.size()),
                          static_cast<int32_t>(out.size() * 8)};
  using Serialize = bool (*)(void*, NetworkBitWriter&, const void*);
  if (!NetworkMessagesMethod<Serialize>(messages, serialize_slot)(messages, writer, message) ||
      writer.overflow || writer.cursor <= 0 || writer.cursor > writer.bits || writer.cursor % 8) {
    return std::nullopt;
  }
  return static_cast<size_t>(writer.cursor / 8);
}

// NetworkBitReader is the Windows bf_read of Source SDK tier1/bitbuf.h, which
// INetworkMessages::UnserializeAbstract reads a message from. Unserialize
// checks the data bits at +12 and the cursor at +16 before reading the
// length-prefixed protobuf.
struct NetworkBitReader {
  const uint8_t* data;
  int32_t bytes;
  int32_t bits;
  int32_t cursor = 0;
  const char* debug_name = "modlock";
  bool overflow = false;
  bool assert_overflow = false;
};

static_assert(offsetof(NetworkBitReader, cursor) == 16);
static_assert(offsetof(NetworkBitReader, debug_name) == 24);
static_assert(sizeof(NetworkBitReader) == 40);

// ReleaseNetworkMessage frees an engine message through its scalar deleting
// destructor, so the game's allocator frees what it allocated.
struct ReleaseNetworkMessage {
  void operator()(void* message) const {
    using Destroy = void* (*)(void*, uint32_t);
    reinterpret_cast<Destroy>((*static_cast<void***>(message))[0])(message, 1);
  }
};

// NetworkMessage owns one message the engine allocated.
using NetworkMessage = std::unique_ptr<void, ReleaseNetworkMessage>;

// ParseNetworkMessage allocates a message from serializer's allocator and
// reads wire bytes, a varint byte count and then the protobuf, into it through
// the INetworkMessages unserialize method at unserialize_slot. It returns
// nothing when allocation fails or the engine reads anything but the whole
// stream. The engine parses its own protobuf, so only bytes cross the
// protobuf ABI.
inline NetworkMessage ParseNetworkMessage(void* messages, size_t unserialize_slot, void* serializer,
                                          std::span<const uint8_t> wire) {
  if (!messages || !serializer || wire.empty()) {
    return nullptr;
  }
  using Allocate = void* (*)();
  Allocate allocate = nullptr;
  std::memcpy(&allocate, static_cast<std::byte*>(serializer) + kSerializerAllocateOffset,
              sizeof(allocate));
  if (!allocate) {
    return nullptr;
  }
  NetworkMessage message(allocate());
  if (!message) {
    return nullptr;
  }

  // The bit reader can fetch the final word; padding is addressable but is
  // not part of the declared stream length.
  std::vector<uint8_t> bytes(wire.size() + 4);
  std::memcpy(bytes.data(), wire.data(), wire.size());
  NetworkBitReader reader{bytes.data(), static_cast<int32_t>(wire.size()),
                          static_cast<int32_t>(wire.size() * 8)};
  using Unserialize = bool (*)(void*, NetworkBitReader&, void*, void*);
  if (!NetworkMessagesMethod<Unserialize>(messages, unserialize_slot)(messages, reader,
                                                                      message.get(), nullptr) ||
      reader.overflow || reader.cursor != reader.bits) {
    return nullptr;
  }
  return message;
}
#endif

}  // namespace modlock::gameinterop
