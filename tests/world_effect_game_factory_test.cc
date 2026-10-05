#include "modlock/render/world_effect_game_factory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using modlock::render::WorldEffectGameCalls;
using modlock::render::WorldEffectGameFactory;

std::vector<std::string> steps;
int removes = 0;
bool accept_result = true;
alignas(16) std::array<void*, 200> vtable;
alignas(16) std::array<void*, 4> entity;

void* Create(void*, const char* name, int) {
  steps.emplace_back(std::string("create:") + name);
  return entity.data();
}
void Teleport(void*, const float* p, const float*, const float*) {
  steps.emplace_back("teleport:" + std::to_string(static_cast<int>(p[0])));
}
void Queue(void*, void*, void*) { steps.emplace_back("queue"); }
void Execute(void*) { steps.emplace_back("execute"); }
bool Accept(void*, const char* input, void*, void*, void* value, int, void*) {
  const auto* variant = static_cast<const char*>(value);
  (void)variant;
  steps.emplace_back(std::string("input:") + input);
  return accept_result;
}
void* MakeKv() {
  steps.emplace_back("keyvalues");
  return reinterpret_cast<void*>(0x2200);
}
void* SetKeyValue(void*, const modlock::gameinterop::MemberName* name, unsigned char) {
  return const_cast<modlock::gameinterop::MemberName*>(name);
}
void SetString(void*, const modlock::gameinterop::MemberName* name, const char* value) {
  steps.emplace_back(std::string("kv:") + name->string + "=" + value);
}
void SetBool(void*, const modlock::gameinterop::MemberName* name, unsigned char value) {
  steps.emplace_back(std::string("kv:") + name->string + "=" + (value ? "1" : "0"));
}
void Remove(void*) {
  ++removes;
  steps.emplace_back("remove");
}

WorldEffectGameCalls Calls() {
  vtable.fill(nullptr);
  vtable[163] = reinterpret_cast<void*>(&Teleport);
  entity.fill(nullptr);
  entity[0] = vtable.data();
  entity[2] = reinterpret_cast<void*>(0x2300);
  WorldEffectGameCalls calls;
  calls.create_entity_by_name = &Create;
  calls.entity_system = reinterpret_cast<void*>(0x2100);
  calls.queue_spawn_entity = &Queue;
  calls.execute_queued_creation = &Execute;
  calls.accept_input = &Accept;
  calls.util_remove = &Remove;
  calls.key_values.create_key_values = &MakeKv;
  calls.key_values.set_key_value = &SetKeyValue;
  calls.key_values.set_string = &SetString;
  calls.key_values.set_bool = &SetBool;
  calls.key_values.set_int = [](void*, const modlock::gameinterop::MemberName* name, int value) {
    steps.emplace_back(std::string("kv:") + name->string + "=" + std::to_string(value));
  };
  calls.key_values.set_float = [](void*, const modlock::gameinterop::MemberName* name,
                                  float value) {
    steps.emplace_back(std::string("kv:") + name->string + "=" + std::to_string(value));
  };
  calls.key_values.set_color = [](void*, const modlock::gameinterop::MemberName* name,
                                  uint32_t value) {
    steps.emplace_back(std::string("kv:") + name->string + "=" + std::to_string(value));
  };
  calls.key_values.set_vector = [](void*, const modlock::gameinterop::MemberName* name,
                                   const float* value) {
    steps.emplace_back(std::string("kv:") + name->string + "=" + std::to_string(value[0]));
  };
  return calls;
}

class EffectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    steps.clear();
    removes = 0;
    accept_result = true;
    factory = std::make_unique<WorldEffectGameFactory>(Calls());
  }
  std::unique_ptr<WorldEffectGameFactory> factory;
};

TEST_F(EffectTest, CreatesMovesAndRemovesOneNativeEffect) {
  modlock::Vec3 at;
  at.set_x(4);
  at.set_y(5);
  at.set_z(6);
  auto effect = factory->Create(modlock::render::ParticleSettings{
      .resource = "particles/environment/powerup_spawner_ambient.vpcf", .origin = at});
  ASSERT_TRUE(effect) << effect.error();
  ASSERT_EQ(steps, (std::vector<std::string>{
                       "create:info_particle_system", "keyvalues",
                       "kv:effect_name=particles/environment/powerup_spawner_ambient.vpcf",
                       "kv:start_active=1", "teleport:4", "queue", "execute", "input:Start"}));
  at.set_x(8);
  effect->get()->Move(at, std::nullopt);
  EXPECT_EQ(steps.back(), "teleport:8");
  effect->get()->Remove();
  effect->reset();
  EXPECT_EQ(removes, 1);
}

// The live resolver exposes allocate/construct/member calls, without typed setters.
TEST_F(EffectTest, UsesTheNativeFreshMemberSurface) {
  static std::array<std::uint64_t, 7> storage;
  static std::array<std::uint64_t, 2> member;
  auto calls = Calls();
  calls.key_values = {};
  calls.key_values.allocate = [](size_t) -> void* { return storage.data(); };
  calls.key_values.construct_key_values = [](void* memory, void*, unsigned char) -> void* {
    return memory;
  };
  calls.key_values.set_key_value = [](void*, const modlock::gameinterop::MemberName*,
                                      unsigned char) -> void* {
    member = {4, 0};
    return member.data();
  };
  WorldEffectGameFactory native_factory(calls);
  auto effect = native_factory.Create(modlock::render::ParticleSettings{
      .resource = "particles/environment/powerup_spawner_ambient.vpcf"});
  ASSERT_TRUE(effect) << effect.error();
  EXPECT_EQ(steps.back(), "input:Start");
  effect->reset();
  EXPECT_EQ(removes, 1);
}

TEST_F(EffectTest, RejectsUnshippedEffectWithoutSpawning) {
  auto effect = factory->Create(modlock::render::ParticleSettings{.resource = "custom.vpcf"});
  ASSERT_FALSE(effect);
  EXPECT_NE(effect.error().find("not one of the shipped"), std::string::npos);
  EXPECT_TRUE(steps.empty());
  EXPECT_EQ(removes, 0);
}

TEST_F(EffectTest, FailedStartRemovesCreatedEntity) {
  accept_result = false;
  auto effect = factory->Create(modlock::render::ParticleSettings{
      .resource = "particles/generic/generic_ping_ground_arrow.vpcf"});
  ASSERT_FALSE(effect);
  EXPECT_NE(effect.error().find("rejected Start"), std::string::npos);
  EXPECT_EQ(removes, 1);
}

TEST_F(EffectTest, InvalidationNeverCallsEngineRemove) {
  auto effect = factory->Create(modlock::render::ParticleSettings{
      .resource = "particles/environment/powerup_spawner_ambient_beam.vpcf"});
  ASSERT_TRUE(effect);
  effect->get()->InvalidateAfterEngineReset();
  effect->reset();
  EXPECT_EQ(removes, 0);
}

TEST_F(EffectTest, ConfiguredParticleCanStartPausedAndChangePlaybackWithoutRespawning) {
  modlock::render::ParticleSettings settings;
  settings.resource = "particles/generic/generic_ping_ground_arrow.vpcf";
  settings.start_active = false;
  settings.angles = {10, 20, 30};
  auto particle = factory->CreateParticle(settings);
  ASSERT_TRUE(particle) << particle.error();
  EXPECT_EQ(steps.back(), "execute");
  ASSERT_TRUE(particle->get()->Start());
  EXPECT_EQ(steps.back(), "input:Start");
  ASSERT_TRUE(particle->get()->Stop());
  EXPECT_EQ(steps.back(), "input:Stop");
  ASSERT_TRUE(particle->get()->Detach());
  EXPECT_EQ(steps.back(), "input:ClearParent");
  particle->get()->Remove();
  const auto count = steps.size();
  EXPECT_FALSE(particle->get()->Start());
  EXPECT_FALSE(particle->get()->Stop());
  EXPECT_FALSE(particle->get()->Transform(settings.origin, settings.angles));
  EXPECT_EQ(steps.size(), count);
  particle->reset();
  EXPECT_EQ(removes, 1);
}

TEST_F(EffectTest, ConfiguredParticleAcceptsOtherGameResourcesButRejectsPathTraversal) {
  modlock::render::ParticleSettings settings;
  settings.resource = "particles/test/another_effect.vpcf";
  auto particle = factory->CreateParticle(settings);
  ASSERT_TRUE(particle) << particle.error();
  particle->reset();
  steps.clear();
  settings.resource = "particles/../../other.vpcf";
  EXPECT_FALSE(factory->CreateParticle(settings));
  EXPECT_TRUE(steps.empty());
}

TEST_F(EffectTest, VisualModelDisablesCollisionAndOwnsItsLifetime) {
  const modlock::render::WorldModelSettings settings{
      .resource = "models/npc/boss_tier_01_brazier_guardian/boss_tier_01_brazier_guardian.vmdl",
      .scale = 0.22f,
      .color_rgba = 0xff20ff40u,
      .glow = true};
  auto model = factory->CreateModel(settings);
  ASSERT_TRUE(model) << model.error();
  for (const auto* expected : {"create:prop_dynamic", "kv:solid=0", "kv:spawnflags=1792",
                               "kv:scales=0.220000", "kv:glowstate=3", "kv:glowrange=0"}) {
    EXPECT_NE(std::find(steps.begin(), steps.end(), expected), steps.end()) << expected;
  }
  EXPECT_EQ(steps.back(), "execute");
  modlock::Vec3 origin;
  origin.set_x(42);
  model->get()->Move(origin, std::nullopt);
  EXPECT_EQ(steps.back(), "teleport:42");
  model->reset();
  EXPECT_EQ(removes, 1);
  model = factory->CreateModel(settings);
  ASSERT_TRUE(model);
  model->get()->InvalidateAfterEngineReset();
  model->reset();
  EXPECT_EQ(removes, 1);
}

TEST_F(EffectTest, SolidModelCollidesThroughItsPhysicsShape) {
  auto model = factory->CreateModel(modlock::render::WorldModelSettings{
      .resource = "models/props_gameplay/crate_wood_small.vmdl", .solid = true});
  ASSERT_TRUE(model) << model.error();
  for (const auto* expected : {"kv:solid=6", "kv:spawnflags=1536"}) {
    EXPECT_NE(std::find(steps.begin(), steps.end(), expected), steps.end()) << expected;
  }
}

TEST_F(EffectTest, FogVolumesRemoveOnceAndInvalidateWithTheirWorld) {
  auto controller = factory->CreateFogController({}, 8192);
  ASSERT_TRUE(controller) << controller.error();
  auto volume = factory->CreateFogVolume({}, {0, 90, 0}, {1600, -8192, -4096}, {8192, 8192, 4096},
                                         4, {12, 85, 55});
  ASSERT_TRUE(volume) << volume.error();
  for (const auto* expected :
       {"create:env_volumetric_fog_controller", "create:env_volumetric_fog_volume", "kv:IsMaster=1",
        "kv:box_mins=1600.000000", "kv:FogStrength=4.000000", "kv:OverrideTintColor=1",
        "kv:FogSunLightStrength=1.000000"}) {
    EXPECT_NE(std::find(steps.begin(), steps.end(), expected), steps.end()) << expected;
  }
  volume->get()->Remove();
  volume->reset();
  EXPECT_EQ(removes, 1);
  controller->get()->InvalidateAfterEngineReset();
  controller->reset();
  EXPECT_EQ(removes, 1);
}

}  // namespace
