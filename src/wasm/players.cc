//go:build deps_only

#include "wasm/players.h"

#include "modlock/gameinterop/connection_tracker.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"
#include "wasm/modifier_masks.h"

namespace modlock::wasm {

namespace {

// kHoldingIdol is the state the game gives the hero carrying the urn.
constexpr const char* kHoldingIdol = "MODIFIER_STATE_HOLDING_IDOL";

}  // namespace

Players::Players(GameServices& services)
    : services_(services), observer_(gameinterop::PawnObserver::LiveSeams(true), true) {}

void Players::WorldReady() {
  // A lookup that fails here is retried by the call that needs it.
  static_cast<void>(services_.ModifierState(kHoldingIdol));
  observer_.Prepare();
}

void Players::WorldEnding() {
  observer_.Invalidate();
}

players::PlayersResponse Players::List() const {
  players::PlayersResponse players;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // List each slot with a seat, connected or away.
    const auto state = gameinterop::ConnectionTracker::StateForSlot(slot);
    if (!state.occupied && !state.away) {
      continue;
    }

    // With its identity, readiness and presence.
    auto* player = players.add_players();
    player->set_player(slot);
    player->set_steam_id(state.is_bot ? 0 : state.xuid);
    player->set_name(state.name);
    player->set_bot(state.is_bot);
    player->set_ready(state.is_bot || state.fully_connected);
    player->set_generation(state.generation);
    player->set_away(state.away);
  }
  return players;
}

players::PawnResponse Players::Pawn(const players::PlayerRequest& request) {
  // Sample the player's hero; a slot without one, or a spectator, answers no
  // pawn.
  players::PawnResponse response;
  const auto sample = Live(request.player());
  if (sample) {
    FillPawn(request.player(), *sample, response.mutable_pawn());
  }
  return response;
}

players::PawnsResponse Players::Pawns() {
  // One sample per occupied slot with a live hero, in slot order: the same
  // pawn Pawn answers for each, in one reply.
  players::PawnsResponse response;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    if (!gameinterop::ConnectionTracker::StateForSlot(slot).occupied) {
      continue;
    }
    const auto sample = Live(slot);
    if (!sample) {
      continue;
    }
    auto* entry = response.add_pawns();
    entry->set_player(slot);
    FillPawn(slot, *sample, entry->mutable_pawn());
  }
  return response;
}

void Players::FillPawn(int32_t slot, const gameinterop::PawnObserver::Sample& sample,
                       players::Pawn* pawn) {
  // Copy the pawn's identity and health.
  pawn->set_entity(sample.pawn_handle);
  pawn->set_hero(sample.hero_id);
  pawn->set_team(sample.team);
  pawn->set_health(sample.health);
  pawn->set_max_health(sample.max_health);
  if (sample.effective_max_health) {
    pawn->set_effective_max_health(*sample.effective_max_health);
  }

  // Copy the pawn's position and view.
  pawn->mutable_position()->set_x(sample.x);
  pawn->mutable_position()->set_y(sample.y);
  pawn->mutable_position()->set_z(sample.z);
  SetAngles(pawn->mutable_eye_angles(), sample.eye_angles);
  SetAngles(pawn->mutable_camera_angles(), sample.camera_angles);

  // Copy the optional readings the sample carries.
  if (sample.stamina) {
    pawn->set_stamina(sample.stamina->current);
    pawn->set_max_stamina(sample.stamina->max);
  }
  pawn->set_generation(sample.session_generation);
  if (sample.currencies) {
    pawn->set_souls((*sample.currencies)[0]);
    pawn->set_ability_points((*sample.currencies)[1]);
    pawn->set_ability_unlocks((*sample.currencies)[2]);
  }

  // Copy the pawn's eye and velocity when the sample has them.
  if (sample.eye_position) {
    SetVector(pawn->mutable_eye_position(), *sample.eye_position);
  }
  if (const auto camera = services_.CameraPosition(slot)) {
    SetVector(pawn->mutable_camera_position(), *camera);
  }
  if (sample.movement && sample.movement->abs_velocity) {
    SetVector(pawn->mutable_velocity(), *sample.movement->abs_velocity);
  }

  // Copy the hero's scoreboard line.
  if (sample.score) {
    // Copy the scoreboard.
    auto* score = pawn->mutable_score();
    score->set_kills(sample.score->kills);
    score->set_deaths(sample.score->deaths);
    score->set_assists(sample.score->assists);
    score->set_last_hits(sample.score->last_hits);

    // Then denies, damage, healing and net worth.
    score->set_denies(sample.score->denies);
    score->set_hero_damage(sample.score->hero_damage);
    score->set_hero_healing(sample.score->hero_healing);
    score->set_net_worth(sample.score->net_worth);
  }

  // The game marks the hero carrying the urn with its holding-idol state.
  if (const auto carrying = ReadModifierState(services_, sample.pawn_handle, kHoldingIdol)) {
    pawn->set_carrying_urn(*carrying);
  }
}

std::expected<gameinterop::PawnObserver::Sample, std::string> Players::Live(int32_t slot) {
  // A spectator's observed pawn is its camera, not a hero.
  auto sample = observer_.Observe(slot);
  if (!sample || sample->team == 1) {
    return std::unexpected("the player has no live hero");
  }
  return *sample;
}

std::expected<void*, std::string> Players::LivePawn(int32_t slot) const {
  void* pawn = observer_.PawnForSlot(slot);
  if (pawn == nullptr) {
    return std::unexpected("the player has no live hero");
  }
  return pawn;
}

std::vector<Players::Hero> Players::Heroes() {
  std::vector<Hero> heroes;
  for (int32_t slot = 0; slot < gameinterop::kMaxPlayerSlots; ++slot) {
    // Collect each live hero.
    const auto sample = Live(slot);
    if (!sample || sample->health <= 0) {
      continue;
    }
    Hero hero{.slot = slot,
              .handle = sample->pawn_handle,
              .pawn = sample->pawn,
              .feet = {float(sample->x), float(sample->y), float(sample->z)}};
    if (sample->movement) {
      hero.velocity = sample->movement->abs_velocity;
    }
    heroes.push_back(hero);
  }
  return heroes;
}

}  // namespace modlock::wasm
