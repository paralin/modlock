//go:build deps_only

#include <algorithm>
#include <array>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "src/gameinterop/engine/hud.pb.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

namespace {

// ScreenState returns the game's state for a screen effect.
std::expected<gameinterop::engine::ScreenEffectState, std::string> ScreenState(
    effects::ScreenEffect effect) {
  switch (effect) {
    case effects::SCREEN_EFFECT_KILLED:
      return gameinterop::engine::SCREEN_EFFECT_STATE_KILLED;
    case effects::SCREEN_EFFECT_BLACK:
      return gameinterop::engine::SCREEN_EFFECT_STATE_BLACK;
    case effects::SCREEN_EFFECT_BLINDED:
      return gameinterop::engine::SCREEN_EFFECT_STATE_BLINDED;
    case effects::SCREEN_EFFECT_DARKNESS:
      return gameinterop::engine::SCREEN_EFFECT_STATE_DRIFTER_DARKNESS_CASTER;
    case effects::SCREEN_EFFECT_MATCH_INTRO:
      return gameinterop::engine::SCREEN_EFFECT_STATE_MATCH_INTRO;
    default:
      return std::unexpected("the screen effect is unknown");
  }
}

}  // namespace

std::expected<combat::TraceResponse, std::string> Game::Trace(const combat::TraceOptions& request) {
  // Resolve the world's collision.
  auto trace = engine_->Trace();
  if (!trace) {
    return std::unexpected(trace.error());
  }
  if (!(*trace)->IsReady()) {
    return std::unexpected("the world's collision is not ready");
  }

  // Set the layers to hit and to exclude.
  gameinterop::TraceOptions options;
  if (request.layers() != 0) {
    options.interacts_with = request.layers();
  }
  options.interacts_exclude = request.exclude();

  // The game's trace skips at most two entities.
  if (request.ignore_size() > static_cast<int>(options.ignored_entities.size())) {
    return std::unexpected("a trace ignores at most two entities");
  }
  std::ranges::copy(request.ignore(), options.ignored_entities.begin());

  // Trace the line.
  auto result = (*trace)->Query(Floats(request.start()), Floats(request.end()),
                                gameinterop::TraceLine{}, options);
  if (!result) {
    return std::unexpected(result.error());
  }

  // Report the hit, if any.
  combat::TraceResponse response;
  if (!result->DidHit()) {
    return response;
  }
  auto* hit = response.mutable_hit();
  SetVector(hit->mutable_position(), result->position);
  SetVector(hit->mutable_normal(), result->normal);
  hit->set_start_solid(result->start_in_solid);
  hit->set_entity(HitEntity(result->entity_handle));
  return response;
}

std::expected<objects::ObjectResponse, std::string> Game::CreateParticle(
    const objects::ParticleOptions& request) {
  // Resolve the effects service.
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Describe the particle with its optional tint, data, anchors and parent.
  render::ParticleSettings settings{
      .resource = WithAddon(request.resource()),
      .origin = request.position(),
      .angles = Floats(request.facing()),
      .start_active = !request.inactive(),
  };
  if (request.color() != 0) {
    // The particle takes its color as 0xAABBGGRR.
    settings.tint = render::ParticleTint{.rgba = std::byteswap(request.color()),
                                         .control_point = request.tint_point()};
  }
  if (request.has_point()) {
    settings.data = render::ParticleData{.control_point = request.point().index(),
                                         .value = Floats(request.point().position())};
  }
  for (const auto& anchor : request.anchors()) {
    settings.control_points.emplace_back(anchor.index(), anchor.entity());
  }
  if (request.parent() != 0) {
    settings.parent = request.parent();
  }

  // Create the particle and keep it.
  auto particle = (*effects)->CreateParticle(settings);
  if (!particle) {
    return std::unexpected(particle.error());
  }
  return Keep(std::unique_ptr<render::WorldEffect>(std::move(*particle)));
}

std::expected<objects::ObjectResponse, std::string> Game::CreateFog(
    const objects::FogOptions& request) {
  // kDrawDistance reaches the far edges of the largest map.
  constexpr float kDrawDistance = 16384;
  auto effects = services_.Effects();
  if (!effects) {
    return std::unexpected(effects.error());
  }

  // Create the map's fog controller once.
  if (!fog_) {
    auto controller = (*effects)->CreateFogController(request.position(), kDrawDistance);
    if (!controller) {
      return std::unexpected(controller.error());
    }
    fog_ = std::move(*controller);
  }

  // Create the fog volume with the color's tint.
  const uint32_t color = request.color();
  const std::array<uint8_t, 3> tint{static_cast<uint8_t>(color >> 24),
                                    static_cast<uint8_t>(color >> 16),
                                    static_cast<uint8_t>(color >> 8)};
  auto volume = (*effects)->CreateFogVolume(request.position(), Floats(request.facing()),
                                            Floats(request.mins()), Floats(request.maxs()),
                                            request.strength(), tint);
  if (!volume) {
    return std::unexpected(volume.error());
  }
  return Keep(std::move(*volume));
}

std::expected<void, std::string> Game::Heal(const players::HealRequest& request) {
  // Read the live hero.
  auto sample = Live(request.player());
  if (!sample) {
    return std::unexpected(sample.error());
  }
  if (sample->health <= 0) {
    return std::unexpected("the player's hero is dead");
  }

  // Raise its health, at most to its maximum.
  // Sum in 64 bits, so an amount near the int32 limit heals to full.
  const int64_t maximum = sample->effective_max_health.value_or(sample->max_health);
  const auto health =
      static_cast<int32_t>(std::min(maximum, int64_t{sample->health} + int64_t{request.amount()}));
  if (health <= sample->health) {
    return {};
  }
  if (!gameinterop::RestorePawnHealth(sample->pawn, health, sample->max_health)) {
    return std::unexpected("cannot heal the player's hero");
  }
  return {};
}

std::expected<void, std::string> Game::ScreenEffect(const effects::ScreenEffectRequest& request) {
  // Resolve the messages, the screen's owner and the effect.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  auto owner = ScreenOwner(request.player());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto state = ScreenState(request.effect());
  if (!state) {
    return std::unexpected(state.error());
  }

  // Show the effect with its timing.
  const auto& timing = request.timing();
  return (*messages)->ScreenEffect(request.player(), *owner, *state,
                                   {.delay = timing.delay(),
                                    .fade_in = timing.fade_in(),
                                    .hold = timing.hold(),
                                    .fade_out = timing.fade_out()});
}

std::expected<void, std::string> Game::ClearScreenEffect(
    const effects::ClearScreenEffectRequest& request) {
  // Resolve the messages, the screen's owner and the effect.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  auto owner = ScreenOwner(request.player());
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto state = ScreenState(request.effect());
  if (!state) {
    return std::unexpected(state.error());
  }
  return (*messages)->ClearScreenEffect(request.player(), *owner, *state);
}

std::expected<int32_t, std::string> Game::ScreenOwner(int32_t slot) {
  // The client drops an effect unless its owner is a live entity, so the
  // effect belongs to the player's hero.
  auto sample = Live(slot);
  if (!sample) {
    return std::unexpected(sample.error());
  }
  return static_cast<int32_t>(gameinterop::EntityIndexOf(sample->pawn_handle));
}

uint64_t Game::HumanSlots() {
  uint64_t slots = 0;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (state.occupied && !state.is_bot) {
      slots |= uint64_t{1} << slot;
    }
  }
  return slots;
}

std::expected<effects::SoundResponse, std::string> Game::PlaySound(
    const std::string& sound, int32_t source, const effects::SoundOptions& options) {
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }

  // Send to the listed players, or to every player, skipping any slot that
  // holds no player to hear it.
  const uint64_t humans = HumanSlots();
  uint64_t recipients = options.listeners().empty() ? humans : 0;
  for (const auto slot : options.listeners()) {
    if (gameinterop::IsPlayerSlot(slot)) {
      recipients |= humans & uint64_t{1} << slot;
    }
  }

  auto id = (*messages)->StartSound(
      recipients, sound, source,
      {options.has_volume() ? std::optional(options.volume()) : std::nullopt,
       options.has_pitch() ? std::optional(options.pitch()) : std::nullopt});
  if (!id) {
    return std::unexpected(id.error());
  }
  effects::SoundResponse response;
  response.set_id(*id);
  return response;
}

std::expected<effects::SoundResponse, std::string> Game::Sound(
    const effects::SoundRequest& request) {
  // Play the sound on the live player's hero, which it follows.
  auto hero = ScreenOwner(request.player());
  if (!hero) {
    return std::unexpected(hero.error());
  }
  return PlaySound(request.sound(), *hero, request.options());
}

std::expected<effects::SoundResponse, std::string> Game::EmitSound(
    const effects::EmitSoundRequest& request) {
  // Play the sound on the live entity, which it follows.
  if (auto entity = Entity(request.entity()); !entity) {
    return std::unexpected(entity.error());
  }
  const auto index = static_cast<int32_t>(gameinterop::EntityIndexOf(request.entity()));
  return PlaySound(request.sound(), index, request.options());
}

std::expected<void, std::string> Game::AdjustSound(const effects::AdjustSoundRequest& request) {
  // Every player gets the change; one not playing the sound ignores it.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->SetSoundFields(
      HumanSlots(), request.id(),
      {request.has_volume() ? std::optional(request.volume()) : std::nullopt,
       request.has_pitch() ? std::optional(request.pitch()) : std::nullopt});
}

std::expected<void, std::string> Game::StopSound(const effects::StopSoundRequest& request) {
  // Every player gets the stop; one not playing the sound ignores it.
  auto messages = services_.Messages();
  if (!messages) {
    return std::unexpected(messages.error());
  }
  return (*messages)->StopSound(HumanSlots(), request.id());
}

}  // namespace modlock::wasm
