//go:build deps_only

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/native_memory.h"
#include "modlock/gameinterop/native_user_messages.h"
#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {
std::expected<void, std::string> Game::Teleport(const movement::TeleportRequest& request) {
  // Resolve the teleport call and the player's live hero.
  auto teleport = services_.Teleport();
  if (!teleport) {
    return std::unexpected(teleport.error());
  }
  auto pawn = players_.LivePawn(request.player());
  if (!pawn) {
    return std::unexpected(pawn.error());
  }

  // Teleport the hero, with a velocity when the request sets one.
  const auto position = Floats(request.position());
  const auto angles = Floats(request.facing());
  if (request.has_velocity()) {
    gameinterop::TeleportEntity(*pawn, position, angles, Floats(request.velocity()));
  } else {
    (*teleport)(nullptr, *pawn, position.data(), angles.data());
  }

  // A teleport recomputes the hero's modifier states, dropping held ones.
  modifiers_.Apply();
  return {};
}

std::expected<void, std::string> Game::Zipline(const movement::ZiplineRequest& request) {
  // Resolve the warp, the player's live hero and its zipline ability.
  const auto lane = request.lane();
  if (lane == movement::LANE_NONE) {
    return std::unexpected("zipline needs a lane");
  }
  auto warp = services_.Zipline();
  if (!warp) {
    return std::unexpected(warp.error());
  }
  auto pawn = players_.LivePawn(request.player());
  if (!pawn) {
    return std::unexpected(pawn.error());
  }
  auto owned = players_.Observer().CurrentAbilitiesForSlot(request.player());
  if (!owned) {
    return std::unexpected(owned.error());
  }
  const auto zipline = std::ranges::find(*owned, gameinterop::kZipLineSlot,
                                         &gameinterop::PawnObserver::Ability::slot);
  void* ability = zipline == owned->end()
                      ? nullptr
                      : players_.Observer().AbilityInstance(request.player(), zipline->handle);
  if (ability == nullptr) {
    return std::unexpected("the hero has no zipline ability");
  }

  // The warp falls back to any lane's zipline when the asked lane has none
  // within the ability's reach, so start it at the lane's nearest node.
  auto node = NearestZiplineNode(*pawn, lane);
  if (!node) {
    return std::unexpected(node.error());
  }
  const std::array<float, 3> forward{};
  if (!(*warp)(ability, lane, node->data(), forward.data(), 0)) {
    return std::unexpected("the hero cannot ride the zipline now");
  }
  return {};
}

std::expected<std::array<float, 3>, std::string> Game::NearestZiplineNode(void* pawn,
                                                                          int32_t lane) {
  // Find the map's zipline nodes and read the hero's team and position.
  auto world = world_.Entities();
  if (!world) {
    return std::unexpected(world.error());
  }
  auto nodes = (*world)->Find("zip_line_node");
  if (!nodes) {
    return std::unexpected(nodes.error());
  }
  auto team_field = services_.Field("CBaseEntity", "m_iTeamNum");
  auto lane_field = services_.Field("CCitadelZipLineNode", "m_iPrimaryLane");
  if (!team_field) {
    return std::unexpected(team_field.error());
  }
  if (!lane_field) {
    return std::unexpected(lane_field.error());
  }
  const auto team = [&](void* entity) {
    uint8_t value = 0;
    std::memcpy(&value, static_cast<unsigned char*>(entity) + team_field->offset, sizeof(value));
    return value;
  };
  const auto hero_team = team(pawn);
  const auto from = Origin(services_, pawn);
  if (!from) {
    return std::unexpected("the hero has no position");
  }

  // Take the nearest node on the lane that the hero's team may ride; team 4
  // marks a node every team rides.
  std::optional<std::array<float, 3>> nearest;
  float nearest_distance = 0;
  for (const auto handle : *nodes) {
    auto node = LiveEntity(handle);
    if (!node) {
      continue;
    }
    void* entity = *node;
    int16_t node_lane = 0;
    std::memcpy(&node_lane, static_cast<unsigned char*>(entity) + lane_field->offset,
                sizeof(node_lane));
    const auto node_team = team(entity);
    if (node_lane != lane || (node_team != hero_team && node_team != 4)) {
      continue;
    }
    const auto at = Origin(services_, entity);
    if (!at) {
      continue;
    }
    const float dx = (*at)[0] - (*from)[0];
    const float dy = (*at)[1] - (*from)[1];
    const float dz = (*at)[2] - (*from)[2];
    const float distance = dx * dx + dy * dy + dz * dz;
    if (!nearest || distance < nearest_distance) {
      nearest = at;
      nearest_distance = distance;
    }
  }
  if (!nearest) {
    return std::unexpected("this map has no zipline on that lane for the hero's team");
  }
  return *nearest;
}

std::expected<void, std::string> Game::MovePlayer(const movement::MovePlayerRequest& request) {
  // Resolve the motion setters and the player's live hero.
  auto motion = services_.Motion();
  if (!motion) {
    return std::unexpected(motion.error());
  }
  const auto slot = request.player();
  auto live = players_.LivePawn(slot);
  if (!live) {
    return std::unexpected(live.error());
  }

  // Convert the requested pose to the game's floats.
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  const auto velocity = Floats(request.velocity());

  // The aim takes the whole facing; tilting the body by pitch would lean the
  // whole hero.
  void* pawn = *live;
  if (auto aimed = players_.Observer().SetEyeAngles(pawn, facing); !aimed) {
    return aimed;
  }
  const std::array<float, 3> body{0, facing[1], 0};
  (*motion)->set_angles(pawn, body.data());
  (*motion)->set_velocity(pawn, velocity.data());
  (*motion)->set_origin(pawn, position.data());
  return {};
}

std::expected<void, std::string> Game::Steer(const movement::SteerRequest& request) {
  // Without steering, the slot's bot goes back to its own commands.
  const auto slot = request.player();
  if (!request.has_steering()) {
    steers_.erase(slot);
    return services_.Steps();
  }

  // Steer the live hero, and drop the steering if the movement hook fails.
  if (auto pawn = players_.LivePawn(slot); !pawn) {
    return std::unexpected(pawn.error());
  }
  steers_.insert_or_assign(slot, request.steering());
  auto steps = services_.Steps();
  if (!steps) {
    steers_.erase(slot);
  }
  return steps;
}

std::expected<void, std::string> Game::Drive(const movement::DriveRequest& request) {
  // A request without a command stops the drive.
  const auto slot = request.player();
  if (!request.has_command()) {
    drives_.erase(slot);
    return services_.Steps();
  }

  // Only a bot can be driven, through the shared hook.
  if (!gameinterop::ConnectionTracker::StateForSlot(slot).is_bot) {
    return std::unexpected("no bot plays in that slot");
  }
  if (auto installed = services_.Commands(); !installed) {
    return std::unexpected(installed.error());
  }

  // Find the slot's drive.
  const auto& command = request.command();
  const bool fresh = !drives_.contains(slot);
  auto& drive = drives_[slot];

  // A press waits for the next command even when the mod drives again first.
  const auto pressed = drive.pressed | command.pressed();
  drive = {.angles = Floats(command.facing()),
           .forward = std::clamp(command.forward(), -1.0f, 1.0f),
           .left = std::clamp(command.left(), -1.0f, 1.0f),
           .held = command.held(),
           .pressed = pressed};

  // A new drive starts the movement step; a failed start drops it.
  if (!fresh) {
    return {};
  }
  auto steps = services_.Steps();
  if (!steps) {
    drives_.erase(slot);
  }
  return steps;
}

}  // namespace modlock::wasm
