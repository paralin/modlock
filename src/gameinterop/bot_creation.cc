#include "modlock/gameinterop/bot_creation.h"

#include <array>
#include <cmath>

#include "modlock/gameinterop/connection_tracker.h"

namespace modlock::gameinterop {

std::expected<BotCreation, std::string> BotCreation::Resolve(const ModuleImage& server) {
  // CreateCitadelBot takes name, native team, hero ID and a position vector.
  // The factory publishes the fake client before requesting hero selection.
  const auto address = ResolveSignature(server, "bot.create");
  if (!address) {
    return std::unexpected(address.error());
  }
  BotCreation result;
  result.create_ = reinterpret_cast<decltype(create_)>(*address);
  return result;
}

std::expected<int32_t, std::string> BotCreation::Create(
    const char* name, int32_t team, uint32_t hero_id, const std::array<float, 3>& position) const {
  // Validate the request.
  if (!create_ || !name || !*name || team < 2 || team > 3 || !hero_id) {
    return std::unexpected("native bot creation requires a name, team and hero");
  }
  for (const float value : position) {
    if (!std::isfinite(value)) {
      return std::unexpected("native bot position is not finite");
    }
  }

  // Note each slot's connection before creating the bot.
  std::array<ConnectionTracker::SlotState, 64> before;
  for (int32_t slot = 0; slot < 64; ++slot) {
    before[slot] = ConnectionTracker::StateForSlot(slot);
  }

  // Create the bot and find the slot its new connection took.
  create_(name, team, hero_id, position.data());
  for (int32_t slot = 0; slot < 64; ++slot) {
    const auto state = ConnectionTracker::StateForSlot(slot);
    if (state.occupied && state.is_bot &&
        (!before[slot].occupied || before[slot].generation != state.generation)) {
      return slot;
    }
  }
  return std::unexpected("native bot factory did not publish a connection");
}

}  // namespace modlock::gameinterop
