#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// BotLeash walks game-driven bots back to a point the session chooses. The bot
// brain asks its retreat check before any other goal but a pending ability,
// whenever it picks a schedule; the leash answers that check for the bot's hero
// pawn, either as the game would or by walking the bot to a goal with the
// game's own SCHED_BOT_RETREAT_WALK. Install before play and retain until the
// engine stops. The callback runs on the engine thread and must outlive the
// leash; only one leash may be installed.
class MODLOCK_API BotLeash {
 public:
  // Decide returns the goal for the bot whose hero pawn has the handle, or
  // nothing to let the game decide.
  using Decide = std::function<std::optional<std::array<float, 3>>(uint32_t)>;

  static std::expected<BotLeash, std::string> Install(const ModuleImage& server, Decide decide);
  BotLeash(BotLeash&&) noexcept;
  BotLeash& operator=(BotLeash&&) noexcept;
  ~BotLeash();

 private:
  struct Impl;
  explicit BotLeash(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
