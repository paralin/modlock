#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>

#include "src/wasm/players/players.pb.h"
#include "wasm/hook_sources.h"
#include "wasm/module.h"
#include "wasm/players.h"
#include "wasm/world.h"

namespace modlock::wasm {

class GameServices;

// Camera answers a mod's calls on its players' cameras: spectating, watching
// a target in its own view or from behind it, roaming free, moving the camera
// and clearing the moves. It tells the host-wide hooks which hero each
// watching player follows, and returns every camera to its hero when the mod
// stops.
class Camera final : public Module, private CameraSource {
 public:
  // services, players and world outlive the camera. The camera registers as
  // a camera source while it lives.
  Camera(GameServices& services, Players& players, World& world);
  ~Camera() override;

  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;

  std::expected<void, std::string> Spectate(const players::PlayerRequest& request);
  std::expected<void, std::string> Watch(const players::WatchRequest& request);
  std::expected<void, std::string> FreeCamera(const players::FreeCameraRequest& request);
  std::expected<void, std::string> MoveCamera(const players::CameraOptions& request);
  std::expected<void, std::string> ClearCamera(const players::ClearCameraRequest& request);
  std::expected<void, std::string> Unwatch(const players::PlayerRequest& request);

  // Leave forgets the hero the slot's player watched.
  void Leave(int32_t slot) override;

  // Clear returns every watching player's camera to their hero.
  void Clear() override;

 private:
  std::optional<int32_t> WatchedSlot(int32_t slot) const override;

  // ObserveFrom binds slot's camera, as Watch and FreeCamera do, sets its
  // observer mode and target and replicates them, and returns the camera pawn.
  std::expected<void*, std::string> ObserveFrom(int32_t slot, uint8_t mode, uint32_t target);

  GameServices& services_;
  Players& players_;
  // world_ removes the camera entity a player watched through.
  World& world_;
  // watched_ maps each player Watch points at a hero to that hero's entity
  // handle, until another camera call or Unwatch. A player watching a hero
  // inside an apart game receives that game's heroes.
  std::map<int32_t, uint32_t> watched_;
};

}  // namespace modlock::wasm
