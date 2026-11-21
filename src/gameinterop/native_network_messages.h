#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

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
#endif

}  // namespace modlock::gameinterop
