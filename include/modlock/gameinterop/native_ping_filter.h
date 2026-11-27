#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/native_post_filter.h"

namespace modlock::gameinterop {

// NativePingFilter offers each ping a human player sends to its handler before
// the server posts it to the recipients: a map ping, a comms wheel line, a
// quick response or an ability ping. The handler runs synchronously on the
// installing engine thread and returns true to drop the ping for every
// recipient, the sender included, with its marker, sound, voice line and chat
// line. Otherwise it may set the bits of player slots in voice_muted: those
// recipients get the ping without its voice line, since the server names the
// line in the ping and each client speaks it, and the rest get it unchanged.
// The filter is a NativePostFilter route, so it runs where that filter does.
class MODLOCK_API NativePingFilter {
 public:
  // Kind is the ping's user message.
  enum class Kind : uint8_t { kMap, kWheel, kQuickResponse, kAbility };

  // Ping is one ping a player sent.
  struct Ping {
    int32_t slot = -1;
    Kind kind = Kind::kMap;
    // option is the comms wheel line, the m_unPingWheelOptionID of
    // scripts/ping_wheel_messages.vdata, for a wheel ping; zero otherwise.
    uint32_t option = 0;
    // concept is the voice line the ping speaks, or zero for none.
    int32_t concept_value = 0;
  };

  using Handler = std::function<bool(const Ping&, uint64_t& voice_muted)>;

  // Decode reads one length-prefixed ping user message. The live filter
  // separately verifies the decoded slot's current authenticated connection.
  static std::optional<Ping> Decode(int32_t message_id, std::span<const uint8_t> serialized);

  // WithoutVoice rewrites one length-prefixed ping user message without its
  // voice line, clearing the speech concept and the chosen response and
  // keeping every other field, the ones this build does not know included.
  // It returns nothing for bytes Decode would refuse.
  static std::optional<std::vector<uint8_t>> WithoutVoice(int32_t message_id,
                                                          std::span<const uint8_t> serialized);

  // Install registers handler with posts for the four ping messages until
  // destruction. posts must outlive the filter.
  static std::expected<NativePingFilter, std::string> Install(NativePostFilter& posts,
                                                              Handler handler);
  NativePingFilter(NativePingFilter&&) noexcept;
  NativePingFilter& operator=(NativePingFilter&&) noexcept;
  ~NativePingFilter();

 private:
  struct Impl;
  explicit NativePingFilter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
