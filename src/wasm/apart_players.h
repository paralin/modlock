#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "modlock/gameinterop/transmit_hook.h"
#include "wasm/hook_sources.h"
#include "wasm/module.h"
#include "wasm/roster.h"

namespace modlock::wasm {

class GameServices;

// ApartPlayers keeps a set of players out of sight of everyone else: the
// heroes of the players inside are withheld from every player outside, and
// every other hero from them. The set follows the mod's scope rather than what
// the mod placed, so it survives Clear and world changes. The transmit hook
// reads it through its TransmitSource while it lives.
class ApartPlayers final : public Module, private TransmitSource {
 public:
  // services and players outlive the apart players.
  ApartPlayers(GameServices& services, const Roster& players);
  ~ApartPlayers();

  ApartPlayers(const ApartPlayers&) = delete;
  ApartPlayers& operator=(const ApartPlayers&) = delete;

  // Set keeps the players in the slot bits apart until the next call; zero
  // shows every hero to everyone again.
  std::expected<void, std::string> Set(uint64_t slots);

 private:
  void Withhold(gameinterop::TransmitSet& set, const Sources<CameraSource>& cameras) override;

  GameServices& services_;
  const Roster& players_;
  // apart_ is the slot bits of the players kept out of sight.
  uint64_t apart_ = 0;
};

}  // namespace modlock::wasm
