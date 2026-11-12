#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

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

}  // namespace modlock::gameinterop
