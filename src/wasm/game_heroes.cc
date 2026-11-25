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
#include "src/gameinterop/engine/camera.pb.h"
#include "wasm/entity_fields.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {
std::expected<void, std::string> Game::Spectate(const players::PlayerRequest& request) {
  // Resolve the selection service and the player in that slot.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  const auto state = gameinterop::ConnectionTracker::StateForSlot(request.player());
  if (!state.occupied) {
    return std::unexpected("no player is in that slot");
  }

  watched_.erase(request.player());
  return players_.Observer().SelectPlayer(request.player(), state.xuid, state.generation, 1,
                                          nullptr, **selection);
}

// The observer modes of ObserverMode_t that a camera here uses.
constexpr uint8_t kObserveInEye = 2;
constexpr uint8_t kObserveChase = 3;
constexpr uint8_t kObserveRoaming = 4;

std::expected<void*, std::string> Game::ObserveFrom(int32_t slot, uint8_t mode, uint32_t target) {
  // Find the camera pawn of the player's slot.
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  auto pawn = players_.Observer().CameraPawnForSlot(slot, **selection);
  if (!pawn) {
    return std::unexpected(pawn.error());
  }

  // Read the pawn's observer services.
  auto services = services_.Field("CBasePlayerPawn", "m_pObserverServices");
  if (!services) {
    return std::unexpected(services.error());
  }
  unsigned char* observer = nullptr;
  if (!gameinterop::ReadNative(static_cast<unsigned char*>(*pawn) + services->offset, &observer,
                               sizeof(observer)) ||
      observer == nullptr) {
    return std::unexpected("the player's camera has no observer services");
  }

  // Resolve the observer target and mode fields.
  auto target_field = services_.Field("CPlayer_ObserverServices", "m_hObserverTarget");
  if (!target_field) {
    return std::unexpected(target_field.error());
  }
  auto mode_field = services_.Field("CPlayer_ObserverServices", "m_iObserverMode");
  if (!mode_field) {
    return std::unexpected(mode_field.error());
  }

  // Set the mode and target, and replicate them. A watch repeats every frame,
  // so a camera already aimed this way sends nothing.
  uint32_t aimed = 0;
  uint8_t observing = 0;
  if (gameinterop::ReadNative(observer + target_field->offset, &aimed, sizeof(aimed)) &&
      gameinterop::ReadNative(observer + mode_field->offset, &observing, sizeof(observing)) &&
      aimed == target && observing == mode) {
    return *pawn;
  }
  if (!gameinterop::WriteNative(observer + target_field->offset, &target, sizeof(target)) ||
      !gameinterop::WriteNative(observer + mode_field->offset, &mode, sizeof(mode))) {
    return std::unexpected("cannot aim the spectator's camera");
  }
  if (!gameinterop::NotifyEntityStateChanged(*pawn)) {
    return std::unexpected("cannot replicate the spectator's camera");
  }
  return *pawn;
}

std::expected<void, std::string> Game::Watch(const players::WatchRequest& request) {
  // Follow the target in its own view or from behind it.
  if (auto target = LiveEntity(request.target()); !target) {
    return std::unexpected(target.error());
  }
  const auto mode = request.chase() ? kObserveChase : kObserveInEye;
  if (auto camera = ObserveFrom(request.player(), mode, request.target()); !camera) {
    return std::unexpected(camera.error());
  }
  watched_[request.player()] = request.target();
  return {};
}

std::expected<void, std::string> Game::FreeCamera(const players::FreeCameraRequest& request) {
  // Resolve the teleport that also turns the client's camera.
  auto teleport = services_.Teleport();
  if (!teleport) {
    return std::unexpected(teleport.error());
  }

  // Roam with no target, then place the camera at the requested pose.
  watched_.erase(request.player());
  auto camera = ObserveFrom(request.player(), kObserveRoaming, gameinterop::kInvalidEHandleIndex);
  if (!camera) {
    return std::unexpected(camera.error());
  }
  const auto position = Floats(request.position());
  const auto facing = Floats(request.facing());
  (*teleport)(nullptr, *camera, position.data(), facing.data());
  return {};
}

std::expected<void, std::string> Game::Unwatch(const players::PlayerRequest& request) {
  // Return the player's camera to their hero.
  watched_.erase(request.player());
  auto selection = services_.Selection();
  if (!selection) {
    return std::unexpected(selection.error());
  }
  auto camera = players_.Observer().ReturnToHero(request.player(), **selection);
  if (!camera) {
    return std::unexpected(camera.error());
  }
  if (!*camera) {
    return {};
  }

  // Remove the camera entity the player watched through.
  auto world = world_.Entities();
  if (!world) {
    return std::unexpected(world.error());
  }
  static_cast<void>((*world)->RemoveEntity(*camera));
  return {};
}

namespace {

namespace engine = gameinterop::engine;

// kCameraParams maps each CameraParam, in order, to the game's param.
constexpr std::array kCameraParams = {
    engine::CAMERA_PARAM_DISTANCE,        engine::CAMERA_PARAM_FOV,
    engine::CAMERA_PARAM_TARGET_POSITION, engine::CAMERA_PARAM_VERT_OFFSET,
    engine::CAMERA_PARAM_HORIZ_OFFSET,
};

// NetworkedHandle packs an entity handle as the game sends one in a message:
// the index in the low 14 bits and the low 10 serial bits above it.
constexpr uint32_t NetworkedHandle(uint32_t handle) {
  const uint32_t serial = (handle >> gameinterop::kSerialShift) & 0x3FF;
  return (gameinterop::EntityIndexOf(handle) & 0x3FFF) | (serial << 14);
}

// ContextId is the game's string token for a camera context's name.
uint32_t ContextId(std::string_view context) {
  return gameinterop::MakeMemberName(context).hash;
}

// SetValue writes a camera value as an operation's number or its vector.
template <typename SetNumber, typename MutableVector>
void SetValue(const players::CameraValue& value, SetNumber set_number,
              MutableVector mutable_vector) {
  // A value without a position is a number.
  if (!value.has_position()) {
    set_number(value.number());
    return;
  }

  // Otherwise write the position as the operation's vector.
  engine::MsgVector* vector = mutable_vector();
  vector->set_x(static_cast<float>(value.position().x()));
  vector->set_y(static_cast<float>(value.position().y()));
  vector->set_z(static_cast<float>(value.position().z()));
}

// SetMotion writes a camera motion as the message's operation and its fields.
// Fields a mod omits stay unset, so the game's defaults apply.
std::expected<void, std::string> SetMotion(const players::CameraMotion& motion,
                                           engine::CameraController& message) {
  switch (motion.motion_case()) {
    case players::CameraMotion::kHold:
      message.set_operation(engine::CAMERA_OPERATION_MAINTAIN);
      message.mutable_maintain()->set_duration(motion.hold().duration());
      return {};
    case players::CameraMotion::kApproach: {
      const auto& in = motion.approach();
      auto* out = message.mutable_approach();
      message.set_operation(engine::CAMERA_OPERATION_APPROACH);
      SetValue(
          in.value(), [&](float number) { out->set_approach_float(number); },
          [&] { return out->mutable_approach_vector(); });
      if (in.has_speed()) {
        out->set_speed(in.speed());
      }
      if (in.has_default_speed()) {
        out->set_default_speed(in.default_speed());
      }
      if (in.has_acceleration()) {
        out->set_acceleration(in.acceleration());
      }
      out->set_min_duration(in.min_duration());
      out->set_chase_default(in.chase_default());
      return {};
    }
    case players::CameraMotion::kSpring: {
      const auto& in = motion.spring();
      auto* out = message.mutable_spring();
      message.set_operation(engine::CAMERA_OPERATION_SPRING);
      SetValue(
          in.value(), [&](float number) { out->set_target_float(number); },
          [&] { return out->mutable_target_vector(); });
      if (in.has_strength()) {
        out->set_spring_strength(in.strength());
      }
      out->set_min_speed(in.min_speed());
      out->set_max_duration(in.max_duration());
      return {};
    }
    case players::CameraMotion::kLerp: {
      const auto& in = motion.lerp();
      auto* out = message.mutable_lerp();
      message.set_operation(engine::CAMERA_OPERATION_LERP);
      SetValue(
          in.start(), [&](float number) { out->set_start_float(number); },
          [&] { return out->mutable_start_vector(); });
      SetValue(
          in.end(), [&](float number) { out->set_end_float(number); },
          [&] { return out->mutable_end_vector(); });
      out->set_bias(in.bias());
      out->set_gain(in.gain());
      if (in.has_duration()) {
        out->set_duration(in.duration());
      }
      return {};
    }
    case players::CameraMotion::kLag: {
      const auto& in = motion.lag();
      auto* out = message.mutable_lag();
      message.set_operation(engine::CAMERA_OPERATION_LAG);
      out->set_min_duration(in.min_duration());
      out->set_lag_time(in.lag_time());
      out->set_max_speed(in.max_speed());
      out->set_spring_strength(in.spring_strength());
      if (in.has_keep_target_on_screen()) {
        out->set_increase_spring_strength_to_keep_target_on_screen(in.keep_target_on_screen());
      }
      return {};
    }
    case players::CameraMotion::MOTION_NOT_SET:
      break;
  }
  return std::unexpected("the camera operation has no motion");
}

}  // namespace

std::expected<void, std::string> Game::MoveCamera(const players::CameraOptions& request) {
  // Resolve the messages and the game's param.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  const auto param = static_cast<size_t>(request.param());
  if (param >= kCameraParams.size()) {
    return std::unexpected("the camera param is unknown");
  }

  // Describe the operation.
  engine::CameraController message;
  message.set_action(engine::CAMERA_ACTION_ADD_OP);
  message.set_param(kCameraParams[param]);
  if (auto motion = SetMotion(request.motion(), message); !motion) {
    return motion;
  }

  // Apply its context, timing and sharing options.
  message.set_context_symbol_id(ContextId(request.context()));
  message.set_delay(request.delay());
  message.set_relative_values(request.relative());
  if (request.has_priority()) {
    message.set_priority(request.priority());
  }
  if (request.shared()) {
    message.set_param_mode(engine::CAMERA_PARAM_MODE_ALLOW_IN_MULTIPLE_CONTEXTS);
  }

  // Aim it at the named hero, or the player's own; the client drops an
  // operation that names no hero.
  uint32_t target = request.target();
  if (request.has_target()) {
    if (auto entity = LiveEntity(target); !entity) {
      return std::unexpected(entity.error());
    }
  } else {
    auto sample = players_.Live(request.player());
    if (!sample) {
      return std::unexpected(sample.error());
    }
    target = sample->pawn_handle;
  }
  message.set_target_pawn(NetworkedHandle(target));
  return (*messages)->CameraController(request.player(), message);
}

std::expected<void, std::string> Game::ClearCamera(const players::ClearCameraRequest& request) {
  // Resolve the message sender.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }

  // Clear the named context, or everything.
  engine::CameraController message;
  if (request.has_context()) {
    message.set_action(engine::CAMERA_ACTION_CLEAR_OPS_FOR_CONTEXT);
    message.set_param(engine::CAMERA_PARAM_CLEAR_ALL_OPS_FOR_CONTEXT);
    message.set_context_symbol_id(ContextId(request.context()));
  } else {
    message.set_action(engine::CAMERA_ACTION_CLEAR_ALL_OPS);
    message.set_param(engine::CAMERA_PARAM_CLEAR_ALL_OPS);
  }

  // Name the player's hero; the client drops a message that names none.
  auto sample = players_.Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  message.set_target_pawn(NetworkedHandle(sample->pawn_handle));
  return (*messages)->CameraController(request.player(), message);
}

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
