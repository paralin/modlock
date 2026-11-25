#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "src/wasm/wasm.pb.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"

namespace modlock::wasm {

class GameServices;

// Match is a mod's hold on the match: the clock it holds at a time and the
// pause it set. Clear releases both, so a stopped mod leaves the match running
// on its own clock.
class Match final : public Module, public MatchCalls {
 public:
  // services and world outlive the match.
  Match(GameServices& services, WorldSource& world) : services_(services), world_(world) {}

  std::expected<void, std::string> Pause(const wasm::PauseRequest& request) override;
  std::expected<wasm::MatchClockResponse, std::string> MatchClock() override;
  std::expected<void, std::string> HoldMatchClock(
      const wasm::HoldMatchClockRequest& request) override;
  std::expected<wasm::RiftResponse, std::string> Rift() override;
  std::expected<void, std::string> StartRift(const wasm::StartRiftRequest& request) override;

  // Frame sets the held clock again, since the game advances it each frame.
  void Frame() override;

  // WorldEnding drops the hold and forgets the pause without resuming, since
  // the paused world is going.
  void WorldEnding() override;

  // Clear drops the hold and resumes a game the mod paused.
  void Clear() override;

 private:
  // HoldClock sets the held match clock again; a hold the game refuses ends.
  void HoldClock();

  // SetPaused pauses or resumes the game.
  std::expected<void, std::string> SetPaused(bool paused);

  // ReplicateClock sends the restored match clock to every client. The start
  // time lives in the game rules, which clients receive through the rules
  // entity; a plain write there reaches only clients that join later.
  std::expected<void, std::string> ReplicateClock();

  GameServices& services_;
  WorldSource& world_;
  // held_clock_ is the match time the mod holds; paused_ is true while the
  // mod has the game paused.
  std::optional<float> held_clock_;
  // rules_entity_ is the handle of the game rules entity, found on first use.
  uint32_t rules_entity_ = 0;
  bool paused_ = false;
};

}  // namespace modlock::wasm
