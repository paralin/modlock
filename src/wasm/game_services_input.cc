//go:build deps_only

#include <algorithm>
#include <array>
#include <iostream>
#include <utility>
#include <vector>

#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// The movement buttons the game holds beside the movement axes.
constexpr uint64_t kInForward = 1u << 3;     // IN_FORWARD
constexpr uint64_t kInBack = 1u << 4;        // IN_BACK
constexpr uint64_t kInMoveLeft = 1u << 9;    // IN_MOVELEFT
constexpr uint64_t kInMoveRight = 1u << 10;  // IN_MOVERIGHT
constexpr uint64_t kMoveKeys = kInForward | kInBack | kInMoveLeft | kInMoveRight;

// MoveKeys returns the movement buttons that match the forward and left axes.
uint64_t MoveKeys(float forward, float left) {
  // Hold the key for each axis direction.
  uint64_t keys = 0;
  if (forward > 0) {
    keys |= kInForward;
  }
  if (forward < 0) {
    keys |= kInBack;
  }
  if (left > 0) {
    keys |= kInMoveLeft;
  }
  if (left < 0) {
    keys |= kInMoveRight;
  }
  return keys;
}

}  // namespace

void GameServices::Input() {
  // Install the hook once the world is ready to take input.
  input_wanted_ = true;
  if (input_ || !world_ready_) {
    return;
  }

  // Resolve the server module and schema the hook needs.
  auto image = Image();
  auto schema = Schema();
  if (!image || !schema) {
    return;
  }

  // Bots take input too, so a mod can press their buttons.
  auto input = gameinterop::AbilityInputHook::Install(
      **image, *schema,
      [this](gameinterop::AbilityInputHook::Input& input) { return Filter(input); },
      [](std::string error) { std::cerr << "mod input: " << error << '\n'; }, true);
  if (!input) {
    std::cerr << "mod input: " << input.error() << '\n';
    return;
  }
  input_ = std::move(*input);
}

uint64_t GameServices::Filter(gameinterop::AbilityInputHook::Input& input) {
  // Merge every game's blocks for this player.
  uint64_t blocked = 0;
  for (auto* game : games_) {
    blocked |= game->input_.Blocked(input.slot);
  }

  // A press is a button held now that changed for this input; a release is
  // one that changed and is up. Blocking clears a button from the stored
  // state, so the engine reads every later command's button as changed; a
  // blocked button's edges come from the player's last real held mask.
  const uint64_t held = input.buttons[0];
  auto& last = held_[static_cast<size_t>(input.slot)];
  const uint64_t changed = (input.buttons[1] & ~blocked) | ((held ^ last) & blocked);
  last = held;
  const uint64_t pressed = held & changed;
  const uint64_t released = ~held & changed;

  // Queue each game's watched changes and add its pending press, then apply
  // the remaps to the input the presses made.
  for (auto* game : games_) {
    game->input_.Record(input.slot, pressed, released);
    game->input_.ApplyPress(input);
  }
  for (auto* game : games_) {
    game->input_.ApplyRemap(input);
  }
  return blocked;
}

void GameServices::Projectiles() {
  // Install the hook once the world is ready for impacts.
  impacts_wanted_ = true;
  if (impacts_ || !world_ready_) {
    return;
  }

  // Resolve the server module the hook patches.
  auto image = Image();
  if (!image) {
    return;
  }

  // Hook impacts, keeping the hook only when it installs.
  auto hook = gameinterop::ProjectileImpactHook::Install(
      **image, [this](void* entity, const auto& contact, const auto& native) {
        Impact(entity, contact, native);
      });
  if (!hook) {
    std::cerr << "mod projectiles: " << hook.error() << '\n';
    return;
  }
  impacts_ = std::move(*hook);
}

std::expected<void, std::string> GameServices::Transmit() {
  // Install the transmit hook once.
  if (transmit_) {
    return {};
  }
  auto hook =
      gameinterop::TransmitHook::Install([this](gameinterop::TransmitSet& set) { Withhold(set); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  transmit_ = std::move(*hook);
  return {};
}

std::expected<void, std::string> GameServices::Commands() {
  // Install once, against the server image.
  if (usercmds_) {
    return {};
  }
  if (usercmds_error_) {
    return std::unexpected(*usercmds_error_);
  }

  // Pawn reads ask every frame, so a hook that cannot install is not retried.
  auto image = Image();
  auto hook = image ? gameinterop::CommandHook::Install(
                          **image, [this](int32_t slot) { return Driven(slot); })
                    : std::unexpected(image.error());
  if (!hook) {
    usercmds_error_ = hook.error();
    return std::unexpected(hook.error());
  }
  usercmds_ = std::move(*hook);
  return {};
}

std::optional<std::array<float, 3>> GameServices::CameraPosition(int32_t slot) {
  if (!Commands()) {
    return std::nullopt;
  }
  return usercmds_->CameraPosition(slot);
}

std::optional<gameinterop::CommandHook::Command> GameServices::Driven(int32_t slot) {
  for (auto* game : games_) {
    // The first game driving the slot answers; its press counts once.
    auto found = game->drives_.find(slot);
    if (found == game->drives_.end()) {
      continue;
    }
    const auto command = found->second;
    found->second.pressed = 0;
    return command;
  }
  for (auto* game : games_) {
    auto steered = game->steers_.find(slot);
    if (steered != game->steers_.end() &&
        gameinterop::ConnectionTracker::StateForSlot(slot).is_bot) {
      const auto& steering = steered->second;
      return gameinterop::CommandHook::Command{.angles = Floats(steering.facing()),
                                               .forward = steering.command().forward(),
                                               .left = steering.command().left(),
                                               .held = steering.command().held()};
    }
  }
  return std::nullopt;
}

void GameServices::Withhold(gameinterop::TransmitSet& set) {
  // A text object's entity changes with its words, so each update reads the
  // current handles.
  for (auto* game : games_) {
    game->visibility_.Withhold(set, [game](uint32_t id) { return game->ObjectHandle(id); });
  }
}

void GameServices::Impact(void* entity, const gameinterop::TraceResult& contact,
                          const gameinterop::ProjectileImpactHook::NativeImpact& native) {
  // Collect the games that watch this projectile.
  std::vector<std::pair<Game*, combat::ImpactEvent>> watching;
  bool keep = false;
  for (auto* game : games_) {
    auto event = game->Watching(entity, contact);
    if (!event) {
      continue;
    }
    keep |= game->projectiles_.keep_momentum();
    watching.emplace_back(game, std::move(*event));
  }
  if (watching.empty()) {
    native();
    return;
  }

  // The game's explosion replaces the motion of heroes near it, even with no
  // push of its own.
  std::vector<Game::Hero> heroes;
  if (keep) {
    heroes = watching.front().first->Heroes();
  }
  native();
  for (const auto& hero : heroes) {
    if (hero.velocity) {
      gameinterop::SetEntityVelocity(hero.pawn, *hero.velocity);
    }
  }

  // Tell each watching game, if it is still loaded.
  for (auto& [game, event] : watching) {
    if (games_.contains(game)) {
      game->Impacted(event);
    }
  }
}

std::expected<void, std::string> GameServices::StepWith(gameinterop::MovementHook::Handler step) {
  // Allow one mod's movement handler at a time.
  if (step && step_) {
    return std::unexpected("another mod's movement runs");
  }
  step_ = std::move(step);

  // Install or remove the movement hook to match.
  auto steps = Steps();
  if (!steps) {
    step_ = nullptr;
  }
  return steps;
}

std::expected<void, std::string> GameServices::Steps() {
  // Keep the hook only while a mod or a steered bot moves heroes.
  const bool needed = step_ || std::ranges::any_of(games_, [](const Game* game) {
                        return game->Moving() || !game->steers_.empty() || !game->drives_.empty();
                      });
  if (!needed) {
    steps_.reset();
    commands_.clear();
    return {};
  }

  // Install the hook the first time a mover needs it.
  if (steps_) {
    return {};
  }
  auto hook = gameinterop::MovementHook::Install(
      gameinterop::MovementHook::Module::kServer,
      [this](gameinterop::MovementCall& call) { return Step(call); });
  if (!hook) {
    return std::unexpected(hook.error());
  }
  steps_ = std::move(*hook);
  return {};
}

bool GameServices::Step(gameinterop::MovementCall& call) {
  // Note the ground the last step stood on.
  using gameinterop::MovementHook;
  if (call.ground_entity != MovementHook::kNoGround) {
    ground_ = call.ground_entity;
  }

  // A driven bot's axes and view reach movement here: the game rebuilds them,
  // and the movement keys, from the bot's own state after ProcessUsercmds,
  // which keeps only the buttons the drive wrote there. The step stays native.
  const auto driven = std::ranges::find_if(
      games_, [&](const Game* game) { return game->drives_.contains(call.slot); });
  if (driven != games_.end()) {
    // Replay the drive's axes, movement keys and view.
    const auto& drive = (*driven)->drives_.at(call.slot);
    call.forward = drive.forward;
    call.left = drive.left;
    call.buttons[0] = (call.buttons[0] & ~kMoveKeys) | MoveKeys(drive.forward, drive.left);
    Face(**driven, call, drive.angles);
  }

  // Record the pawn's command for the mod to read.
  if (call.pawn) {
    // The tick's first command fixes its starting ground; later commands add
    // their presses and leave the last buttons and axes.
    auto& [command, stepped] = commands_[call.pawn];
    if (!std::exchange(stepped, true)) {
      command.set_grounded(call.ground_entity != MovementHook::kNoGround);
    }

    // Keep the latest held buttons and axes, and add this command's presses.
    command.set_held(call.buttons[0]);
    command.set_changed(command.changed() | call.buttons[1]);
    command.set_scroll(command.scroll() | call.buttons[2]);
    command.set_forward(call.forward);
    command.set_left(call.left);
    command.set_up(call.up);
  }
  if (driven != games_.end()) {
    return false;
  }

  // Steer a bot that replays a recorded command.
  for (auto* game : games_) {
    // Steer the player this game replays.
    if (auto steered = game->steers_.find(call.slot); steered != game->steers_.end()) {
      // The recorded command drives the game's own movement, abilities and
      // animation for this step.
      auto& steering = steered->second;
      auto& command = *steering.mutable_command();
      call.buttons = {command.held(), command.changed(), command.scroll()};
      call.forward = command.forward();
      call.left = command.left();
      call.up = command.up();

      // The recorded view steers the step and becomes the hero's aim.
      Face(*game, call, Floats(steering.facing()));

      // A press counts once, though the command stays for the ticks after.
      command.set_changed(0);
      command.set_scroll(0);

      // A pose counts once too: on the ticks before the next Steer, as when
      // the mod skips a slow frame, the game's own step carries the held
      // command on from it rather than pinning the hero where it stood.
      if (!steering.has_position()) {
        return false;
      }

      // The step then ends exactly on the recorded pose, on the ground the
      // heroes stand on when its own step left it in the air.
      call.origin = Floats(steering.position());
      call.velocity = Floats(steering.velocity());
      const auto ground = call.ground_entity;
      if (!steering.grounded()) {
        call.ground_entity = MovementHook::kNoGround;
      } else if (ground == MovementHook::kNoGround) {
        call.ground_entity = ground_;
      }
      if (call.ground_entity != ground) {
        static_cast<void>(gameinterop::NotifyEntityStateChanged(call.pawn));
      }
      steering.clear_position();
      steering.clear_velocity();
      return true;
    }
  }

  return step_ && step_(call);
}

void GameServices::Face(Game& game, gameinterop::MovementCall& call,
                        const std::array<float, 3>& angles) {
  // The yaw turns the body, as Game::MovePlayer does. The bot's own command
  // set the view and the body before this step, and the eye angles alone
  // leave the body at the bot's yaw, so the hero would run sideways.
  call.angles = angles;
  static_cast<void>(game.observer_.SetEyeAngles(call.pawn, call.angles));
  if (auto motion = Motion()) {
    const std::array<float, 3> body{0, call.angles[1], 0};
    (*motion)->set_angles(call.pawn, body.data());
  }
}

std::optional<movement::MovementCommand> GameServices::Command(const void* pawn) {
  // Find the pawn's command for this tick.
  const auto found = commands_.find(pawn);
  if (found == commands_.end()) {
    return std::nullopt;
  }

  // Hand it over, keeping the held buttons and clearing the presses.
  auto& [command, stepped] = found->second;
  auto taken = command;
  command.set_changed(0);
  command.set_scroll(0);
  stepped = false;
  return taken;
}

}  // namespace modlock::wasm
