// Contract tests for the pawn observer's hero selection, seating, watching
// and respawn, over the same recorded entity images.
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "modlock/gameinterop/pawn_observer.h"
#include "pawn_observer_fixture.h"

namespace pawn_observer_test {

namespace {

TEST(PawnObserver, PlayerSelectionStopsAfterTeamCallbackLosesConnection) {
  // Seat a player whose team change bumps the connection generation.
  static Fixture* active = nullptr;
  static int observer_spawns = 0;
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 7;
  active = &fixture;
  observer_spawns = 0;

  // Install the team change in the controller's table.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ++active->generation; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Count observer spawns.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    return nullptr;
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
  };

  // Selection stops when the team change loses the connection.
  PawnObserver observer{HeroSeamsFor(fixture)};
  const auto refused = observer.SelectPlayer(0, fixture.xuid, 7, 1, nullptr, calls);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("connection changed"), std::string::npos);
  EXPECT_EQ(observer_spawns, 0);
  active = nullptr;
}

TEST(PawnObserver, SpectatorSelectionNeedsNoHeroAndDoesNotCreateCombatPawn) {
  // Seat a player already on the spectator team.
  static int observer_spawns = 0;
  static Fixture* active = nullptr;
  observer_spawns = 0;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 1;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] =
      reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE() << "already on spectator team"; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Spawn the observer without publishing a pawn, and fail on any pawn bind.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    ++observer_spawns;
    // The engine can return an actor before publishing m_hPawn.
    return active->controller.data();
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
    ADD_FAILURE() << "already on spectator team";
  };

  // Selection waits for the pawn and spawns once.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);

  // Once the same connection publishes a pawn, selection becomes ready and
  // later observer checks must reuse it rather than creating another actor.
  auto pawn = MakeController(0);
  fixture.image.SetIdentity(2, pawn.data(), 9, 0);
  const uint32_t handle = HandleOf(2, 9);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &handle, sizeof(handle));
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);

  // Another player's identity is refused.
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid + 1, 3, 1, nullptr, calls));
  EXPECT_EQ(observer_spawns, 1);
}

TEST(PawnObserver, SpectatorSelectionReleasesTheHeroBeforeTheTeamChanges) {
  // Seat a player on team 2.
  static Fixture* active = nullptr;
  static std::vector<std::string> steps;
  Fixture fixture;
  active = &fixture;
  steps.clear();
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Bind them to a hero.
  Instance hero{};
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // The controller records each native call in order.
  std::array<void*, 106> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) { steps.push_back("notify"); });
  table[105] = reinterpret_cast<void*>(+[](void*, int team) {
    EXPECT_EQ(team, 1);
    steps.push_back("team");
  });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.set_pawn = +[](void* controller, void* pawn, bool retain_old_pawn_team, bool, bool, bool) {
    // Unbind the pawn and keep its team.
    EXPECT_EQ(controller, active->controller.data());
    EXPECT_EQ(pawn, nullptr);
    EXPECT_TRUE(retain_old_pawn_team);
    const uint32_t none = 0xffffffff;
    std::memcpy(static_cast<char*>(controller) + kPawnHandleField, &none, sizeof(none));
    steps.push_back("unbind");
  };
  calls.spawn_observer = +[](void*) -> void* {
    steps.push_back("observer");
    return nullptr;
  };

  // The hero handle is cleared and published, and the pawn unbound, before
  // the team changes and the observer spawns.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_FALSE(observer.SelectPlayer(0, fixture.xuid, 3, 1, nullptr, calls));
  uint32_t released = 0;
  std::memcpy(&released, fixture.controller.data() + kHeroHandleField, sizeof(released));
  EXPECT_EQ(released, 0xffffffffu);
  EXPECT_EQ(steps, (std::vector<std::string>{"notify", "unbind", "team", "observer"}));
  active = nullptr;
}

PawnObserver::Seams SeatingSeamsFor(Fixture& fixture, bool play_state = true) {
  auto seams = HeroSeamsFor(fixture);
  seams.layout = [play_state]() -> std::expected<EntityLayout, std::string> {
    auto layout = FixtureLayout();
    layout.hero_pawn_handle = kHeroHandleField;
    if (play_state) {
      layout.play_state = kPlayStateField;
    }
    return layout;
  };
  return seams;
}

TEST(PawnObserver, SeatingJoinsATeamlessPlayerAndLeavesThemChoosingAHero) {
  // Seat a teamless player.
  static Fixture* active = nullptr;
  static std::vector<std::string> steps;
  Fixture fixture;
  active = &fixture;
  steps.clear();
  fixture.occupied = true;
  fixture.generation = 3;

  // Record each notification's play state and each team change.
  std::array<void*, 106> table{};
  table[28] = reinterpret_cast<void*>(+[](void*, const void*) {
    int32_t state = 0;
    std::memcpy(&state, active->controller.data() + kPlayStateField, sizeof(state));
    steps.push_back("notify:" + std::to_string(state));
  });
  table[105] = reinterpret_cast<void*>(+[](void* controller, int team) {
    static_cast<unsigned char*>(controller)[kTeamField] = static_cast<unsigned char>(team);
    steps.push_back("team:" + std::to_string(team));
  });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // The player has no hero, and creating one records its team.
  const uint32_t none = 0xffffffff;
  std::memcpy(fixture.controller.data() + kHeroHandleField, &none, sizeof(none));
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.create_pawn = +[](void*, int team) -> void* {
    steps.push_back("pawn:" + std::to_string(team));
    return nullptr;
  };

  // Seating joins the team.
  PawnObserver observer{SeatingSeamsFor(fixture)};
  EXPECT_EQ(observer.ControllerTeam(0), 0);
  const auto seated = observer.SeatForChoice(0, fixture.xuid, 3, 3, calls);
  ASSERT_TRUE(seated) << seated.error();
  EXPECT_TRUE(*seated);

  // The game's own join: the team, the hero pawn, then the play state, which
  // is EPlayerChoosingHero when it replicates.
  EXPECT_EQ(steps, (std::vector<std::string>{"team:3", "pawn:3", "notify:2"}));
  EXPECT_EQ(observer.ControllerTeam(0), 3);

  // A player who has a team is left alone.
  steps.clear();
  const auto again = observer.SeatForChoice(0, fixture.xuid, 3, 2, calls);
  ASSERT_TRUE(again) << again.error();
  EXPECT_FALSE(*again);
  EXPECT_TRUE(steps.empty());
  active = nullptr;
}

TEST(PawnObserver, SeatingRefusesWithoutThePlayStateAndLeavesTheTeamScreen) {
  // Seat a teamless player.
  static Fixture* active = nullptr;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE() << "team changed"; });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.create_pawn = +[](void*, int) -> void* {
    return nullptr;
  };

  // Without the play state seating is refused and the team stays.
  PawnObserver observer{SeatingSeamsFor(fixture, false)};
  const auto refused = observer.SeatForChoice(0, fixture.xuid, 3, 2, calls);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().find("play state"), std::string::npos);
  EXPECT_EQ(observer.ControllerTeam(0), 0);
  EXPECT_FALSE(observer.SeatForChoice(0, fixture.xuid, 3, 1, calls));
  active = nullptr;
}

TEST(PawnObserver, HeroSelectionDoesNotTreatObserverPawnAsHeroPawn) {
  // Track the hero pawn and the native calls.
  static Fixture* active = nullptr;
  static void* hero_pawn = nullptr;
  static int created = 0;
  static int selected = 0;

  // Seat a player on team 2.
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Fail on any team change.
  std::array<void*, 106> table{};
  table[105] = reinterpret_cast<void*>(+[](void*, int) { ADD_FAILURE(); });
  auto* table_ptr = table.data();
  std::memcpy(fixture.controller.data(), &table_ptr, sizeof(table_ptr));
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);

  // Bind the player to a spectator pawn; the hero pawn is separate.
  auto spectator = MakeController(0);
  auto hero = MakeController(0);
  hero_pawn = hero.data();
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  const auto spectator_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &spectator_handle,
              sizeof(spectator_handle));

  // Creating the hero publishes its handle, and selection checks it.
  modlock::gameinterop::PlayerSelectionCalls calls;
  created = selected = 0;
  calls.create_pawn = +[](void*, int) -> void* {
    ++created;
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kHeroHandleField, &handle, sizeof(handle));
    return hero_pawn;
  };
  calls.select_hero = +[](void* pawn, void*) {
    EXPECT_EQ(pawn, hero_pawn);
    ++selected;
  };

  // Selection creates and selects the hero once.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_TRUE(observer.SelectPlayer(0, fixture.xuid, 3, 2, &fixture, calls));
  EXPECT_EQ(created, 1);
  EXPECT_EQ(selected, 1);
}

TEST(PawnObserver, WatchingKeepsTheHeroAndReturnsToIt) {
  // Track the camera, the rebound pawn and the spawns.
  static Fixture* active = nullptr;
  static void* camera_pawn = nullptr;
  static void* rebound = nullptr;
  static int spawns = 0;

  // Seat a player on team 2.
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 2;

  // Give them a living hero and a camera.
  Instance hero{}, camera{};
  camera_pawn = camera.data();
  const int32_t health = 800;
  std::memcpy(hero.data() + kHealthField, &health, sizeof(health));

  // Register them and bind the hero.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, camera.data(), 8, 0);
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // The game binds the camera it spawns, and the hero it is given.
  modlock::gameinterop::PlayerSelectionCalls calls;
  spawns = 0;
  rebound = nullptr;
  calls.spawn_observer = +[](void*) -> void* {
    ++spawns;
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kPawnHandleField, &handle, sizeof(handle));
    return camera_pawn;
  };
  calls.set_pawn = +[](void* controller, void* pawn, bool retain_old_pawn_team, bool, bool, bool) {
    EXPECT_FALSE(retain_old_pawn_team);
    rebound = pawn;
    const auto handle = HandleOf(2, 7);
    std::memcpy(static_cast<char*>(controller) + kPawnHandleField, &handle, sizeof(handle));
  };

  // The hero stays the player's hero while a single camera watches.
  PawnObserver observer{HeroSeamsFor(fixture)};
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), camera.data());
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), camera.data());
  EXPECT_EQ(spawns, 1);
  uint32_t kept = 0;
  std::memcpy(&kept, fixture.controller.data() + kHeroHandleField, sizeof(kept));
  EXPECT_EQ(kept, hero_handle);
  EXPECT_EQ(observer.WatchingSlots(), std::vector<int32_t>{0});

  // Returning binds the hero and hands back the camera to remove, once.
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(0), HandleOf(3, 8));
  EXPECT_EQ(rebound, hero.data());
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(1), 0u);
  EXPECT_TRUE(observer.WatchingSlots().empty());

  // A camera still bound after the player reconnects, or after the runtime
  // forgot it, returns the player to their living hero all the same.
  const auto camera_handle = HandleOf(3, 8);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &camera_handle, sizeof(camera_handle));
  fixture.generation = 4;
  rebound = nullptr;
  PawnObserver restarted{HeroSeamsFor(fixture)};
  EXPECT_EQ(restarted.ReturnToHero(0, calls).value_or(0), camera_handle);
  EXPECT_EQ(rebound, hero.data());
  active = nullptr;
}

TEST(PawnObserver, ReturnLeavesTheGamesCameraAfterTheHeroDies) {
  // Track the camera and seat a player on team 3.
  static Fixture* active = nullptr;
  static void* camera_pawn = nullptr;
  Fixture fixture;
  active = &fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller[kTeamField] = 3;

  // Give them a hero, a camera and a death camera.
  Instance hero{}, camera{}, death{};
  camera_pawn = camera.data();
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, hero.data(), 7, 0);
  fixture.image.SetIdentity(3, camera.data(), 8, 0);
  fixture.image.SetIdentity(4, death.data(), 9, 0);

  // Bind the hero.
  const auto hero_handle = HandleOf(2, 7);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  std::memcpy(fixture.controller.data() + kPawnHandleField, &hero_handle, sizeof(hero_handle));

  // Spawning binds the camera; the dead hero cannot be bound.
  modlock::gameinterop::PlayerSelectionCalls calls;
  calls.spawn_observer = +[](void*) -> void* {
    const auto handle = HandleOf(3, 8);
    std::memcpy(active->controller.data() + kPawnHandleField, &handle, sizeof(handle));
    return camera_pawn;
  };
  calls.set_pawn = +[](void*, void*, bool, bool, bool, bool) {
    ADD_FAILURE() << "hero is dead";
  };

  // Watch through the camera.
  PawnObserver observer{HeroSeamsFor(fixture)};
  ASSERT_TRUE(observer.CameraPawnForSlot(0, calls));

  // The hero dies and the game binds its own camera, which a later watch
  // reuses; the respawn returns the player to the hero.
  const auto death_handle = HandleOf(4, 9);
  std::memcpy(fixture.controller.data() + kPawnHandleField, &death_handle, sizeof(death_handle));
  EXPECT_EQ(observer.ReturnToHero(0, calls).value_or(0), HandleOf(3, 8));
  EXPECT_EQ(observer.CameraPawnForSlot(0, calls).value_or(nullptr), death.data());
  EXPECT_TRUE(observer.WatchingSlots().empty());
  active = nullptr;
}

TEST(PawnObserver, RespawnUsesDeadHeroHandleAndRejectsStaleConnections) {
  // Track the respawned pawn and the respawns.
  static void* expected_pawn = nullptr;
  static int respawned = 0;

  // Seat a player on team 3 with a spectator and a hero.
  Fixture fixture;
  fixture.occupied = true;
  fixture.generation = 3;
  fixture.controller = MakeController(HandleOf(2, 7));
  Instance spectator{}, hero{};
  expected_pawn = hero.data();
  respawned = 0;

  // Register them and name the hero.
  fixture.image.SetIdentity(1, fixture.controller.data(), 1, 0);
  fixture.image.SetIdentity(2, spectator.data(), 7, 0);
  fixture.image.SetIdentity(3, hero.data(), 8, 0);
  auto hero_handle = HandleOf(3, 8);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  PawnObserver observer{HeroSeamsFor(fixture)};

  // Respawning restores the hero's health.
  auto respawn = +[](void* pawn, bool force) {
    // Check the respawn and give the pawn health.
    EXPECT_EQ(pawn, expected_pawn);
    EXPECT_TRUE(force);
    ++respawned;
    const int32_t health = 100;
    std::memcpy(static_cast<char*>(pawn) + kHealthField, &health, sizeof(health));
  };

  // The wrong team respawns nothing.
  EXPECT_FALSE(observer.RespawnPlayer(0, 2, respawn));
  EXPECT_EQ(respawned, 0);

  // The dead hero respawns once.
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
  ASSERT_TRUE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);

  // A stale hero handle is refused.
  hero_handle = HandleOf(3, 9);
  std::memcpy(fixture.controller.data() + kHeroHandleField, &hero_handle, sizeof(hero_handle));
  EXPECT_FALSE(observer.RespawnPlayer(0, 3, respawn));
  EXPECT_EQ(respawned, 1);
}

}  // namespace

}  // namespace pawn_observer_test
