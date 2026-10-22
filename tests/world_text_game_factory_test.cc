// Contract tests for WorldTextGameFactory: symbol resolution against a
// synthetic module image, the native creation order (create, keyvalues,
// teleport, queue, execute), the proven constraint set in the keyvalues
// arguments, and log-and-continue degradation on unresolved surfaces.
#include "modlock/render/world_text_game_factory.h"

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "gameinterop_module_image_test.h"
#include "gtest/gtest.h"
#include "modlock/gameinterop/keyvalues.h"

namespace {

void SetInteropTrace(const char* value) {
#if defined(_WIN32)
  _putenv_s("MODLOCK_INTEROP_TRACE", value == nullptr ? "" : value);
#else
  if (value == nullptr) {
    unsetenv("MODLOCK_INTEROP_TRACE");
  } else {
    setenv("MODLOCK_INTEROP_TRACE", value, 1);
  }
#endif
}

class ScopedInteropTrace final {
 public:
  ScopedInteropTrace() {
    if (const char* value = std::getenv("MODLOCK_INTEROP_TRACE"); value != nullptr) {
      previous_ = value;
    }
    SetInteropTrace("1");
  }

  ~ScopedInteropTrace() { SetInteropTrace(previous_.has_value() ? previous_->c_str() : nullptr); }

 private:
  std::optional<std::string> previous_;
};

size_t CountOccurrences(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t offset = 0; (offset = text.find(needle, offset)) != std::string::npos; ++offset) {
    ++count;
  }
  return count;
}

using modlock::render::WorldTextGameCalls;
using modlock::render::WorldTextGameFactory;
using modlock::render::WorldTextStyle;
namespace gti = modlock::gameinterop::testing;

struct Recorder {
  std::vector<std::string> steps;
  std::vector<std::pair<std::string, std::string>> key_values;

  void Note(std::string step) { steps.push_back(std::move(step)); }
};

Recorder g_recorder;

alignas(16) std::array<void*, 200> g_entity_vtable;
alignas(16) std::array<void*, 4> g_entity_object;
void* g_ekv_sentinel = reinterpret_cast<void*>(0x2100);
modlock::Vec3 g_teleport_position;
modlock::EulerAngles g_teleport_angles;
void* g_spawned_identity = nullptr;
void* g_spawned_ekv = nullptr;
int g_removed_count = 0;

void* RecordCreateEntityByName(void* /*ignored_this*/, const char* class_name,
                               int /*force_edict_index*/) {
  g_recorder.Note(std::string("create:") + class_name);
  return &g_entity_object;
}

void RecordTeleport(void* /*entity*/, const float* position, const float* angles,
                    const float* velocity) {
  // Record the pose the entity is placed at.
  g_recorder.Note("teleport");
  g_teleport_position.set_x(position[0]);
  g_teleport_position.set_y(position[1]);
  g_teleport_position.set_z(position[2]);
  g_teleport_angles.set_pitch(angles[0]);
  g_teleport_angles.set_yaw(angles[1]);
  g_teleport_angles.set_roll(angles[2]);
  EXPECT_EQ(velocity[0], 0.0f);
}

void RecordQueueSpawnEntity(void* entity_system, void* identity, void* key_values) {
  g_recorder.Note("queue-spawn-entity");
  EXPECT_EQ(entity_system, reinterpret_cast<void*>(0x2200));
  g_spawned_identity = identity;
  g_spawned_ekv = key_values;
}

void RecordExecuteQueuedCreation(void* entity_system) {
  g_recorder.Note("execute-queued-creation");
  EXPECT_EQ(entity_system, reinterpret_cast<void*>(0x2200));
}

// g_refuse_set_message makes the fake entity refuse SetMessage, as an entity
// without the input would.
bool g_refuse_set_message = false;

bool RecordAcceptInput(void*, const char* input_name, void*, void*, void* value, int output_id,
                       void*) {
  const auto* text = *reinterpret_cast<const char* const*>(value);
  g_recorder.Note(std::string("accept-input:") + input_name + ":" + text);
  EXPECT_EQ(output_id, 0);
  return !(g_refuse_set_message && std::string_view(input_name) == "SetMessage");
}

bool RecordWriteMessage(void*, const char* message) {
  g_recorder.Note(std::string("write-message:") + message);
  return true;
}

void* RecordCreateKeyValues() {
  g_recorder.Note("create-key-values");
  return g_ekv_sentinel;
}

void RecordKvSetString(void*, const modlock::gameinterop::MemberName* name, const char* value) {
  g_recorder.key_values.emplace_back(name->string, value);
}

void RecordKvSetBool(void*, const modlock::gameinterop::MemberName* name, unsigned char value) {
  g_recorder.key_values.emplace_back(name->string, value != 0 ? "1" : "0");
}

void RecordKvSetFloat(void*, const modlock::gameinterop::MemberName* name, float value) {
  g_recorder.key_values.emplace_back(name->string, std::to_string(value));
}

void RecordKvSetInt(void*, const modlock::gameinterop::MemberName* name, int value) {
  g_recorder.key_values.emplace_back(name->string, std::to_string(value));
}

void RecordKvSetColor(void*, const modlock::gameinterop::MemberName* name, std::uint32_t packed) {
  g_recorder.key_values.emplace_back(name->string, std::to_string(packed & 0xFF) + "," +
                                                       std::to_string((packed >> 8) & 0xFF) + "," +
                                                       std::to_string((packed >> 16) & 0xFF) + "," +
                                                       std::to_string((packed >> 24) & 0xFF));
}

void RecordUtilRemove(void*) {
  g_removed_count++;
}

WorldTextGameCalls RecordingCalls() {
  // Point each engine call at its recorder.
  WorldTextGameCalls calls;
  calls.create_entity_by_name = &RecordCreateEntityByName;
  calls.entity_system = reinterpret_cast<void*>(0x2200);
  calls.queue_spawn_entity = &RecordQueueSpawnEntity;
  calls.execute_queued_creation = &RecordExecuteQueuedCreation;
  calls.accept_input = &RecordAcceptInput;
  calls.write_message = &RecordWriteMessage;

  // Record each keyvalue setter.
  calls.key_values.create_key_values = &RecordCreateKeyValues;
  calls.key_values.set_string = &RecordKvSetString;
  calls.key_values.set_bool = &RecordKvSetBool;
  calls.key_values.set_float = &RecordKvSetFloat;
  calls.key_values.set_int = &RecordKvSetInt;
  calls.key_values.set_color = &RecordKvSetColor;
  calls.util_remove = &RecordUtilRemove;

  // The fake entity: vtable at 0 with Teleport in slot 163; its m_pEntity slot
  // (0x10) carries a sentinel identity.
  g_entity_vtable.fill(nullptr);
  g_entity_vtable[163] = reinterpret_cast<void*>(&RecordTeleport);
  g_entity_object[0] = g_entity_vtable.data();
  g_entity_object[2] = reinterpret_cast<void*>(0x2300);
  return calls;
}

gti::FakeModuleImage BuildImage() {
  gti::FakeModuleImage image;
  for (const auto id : {"entity-system.create-entity-by-name", "entity-system.queue-spawn-entity",
                        "entity-system.execute-queued-creation", "entity-instance.accept-input",
                        "entity.remove", "entity-keyvalues.allocate", "entity-keyvalues.construct",
                        "entity-keyvalues.set-key-value"}) {
    image.AddSignature(id);
  }
  image.Seal(reinterpret_cast<std::uintptr_t>(&RecordTeleport));
  return image;
}

class WorldTextGameFactoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Clear what the last test recorded.
    g_recorder.steps.clear();
    g_recorder.key_values.clear();
    g_removed_count = 0;
    g_spawned_identity = nullptr;
    g_spawned_ekv = nullptr;
  }

  WorldTextGameFactory factory_{RecordingCalls()};
};

TEST_F(WorldTextGameFactoryTest, TryCreateResolvesEveryScannedProbeFromTheImage) {
  auto image = BuildImage();
  auto factory = WorldTextGameFactory::TryCreate(image);
  ASSERT_TRUE(factory.has_value()) << factory.error();
}

TEST_F(WorldTextGameFactoryTest, EngineResetInvalidationDoesNotCallUtilRemove) {
  // Create a text.
  auto created =
      factory_.Create("stale", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_TRUE(created.has_value()) << created.error();

  // An engine reset leaves nothing to remove.
  (*created)->InvalidateAfterEngineReset();
  created->reset();
  EXPECT_EQ(g_removed_count, 0);
}

TEST_F(WorldTextGameFactoryTest, CreateAppliesKeyValuesAndPoseBeforeQueuedSpawn) {
  // Place the text off the origin, turned.
  modlock::Vec3 origin;
  origin.set_x(10);
  origin.set_y(20);
  origin.set_z(30);
  modlock::EulerAngles angles;
  angles.set_yaw(-35.5);
  angles.set_roll(90);

  // Style it larger and colored.
  WorldTextStyle style;
  style.font_size = 120.0f;
  style.color_abgr = 0xFFA0FF40u;

  // Create it.
  auto created = factory_.Create("gate label", origin, angles, style);
  ASSERT_TRUE(created.has_value()) << created.error();

  // Seven steps ran.
  const auto& steps = g_recorder.steps;
  ASSERT_EQ(steps.size(), 7);

  // Each step runs in order.
  EXPECT_EQ(steps[0], "create:point_worldtext");
  EXPECT_EQ(steps[1], "create-key-values");
  EXPECT_EQ(steps[2], "teleport");
  EXPECT_EQ(steps[3], "queue-spawn-entity");
  EXPECT_EQ(steps[4], "execute-queued-creation");
  EXPECT_EQ(steps[5], "write-message:gate label");
  EXPECT_EQ(steps[6], "accept-input:Enable:");

  // Teleport precedes spawn and carries roll-90 plus the manual yaw.
  EXPECT_EQ(g_teleport_position.x(), 10);
  EXPECT_EQ(g_teleport_position.z(), 30);
  EXPECT_EQ(g_teleport_angles.yaw(), -35.5);
  EXPECT_EQ(g_teleport_angles.roll(), 90);

  // The queued creation receives the entity identity and the built keyvalues.
  EXPECT_EQ(g_spawned_identity, reinterpret_cast<void*>(0x2300));
  EXPECT_EQ(g_spawned_ekv, g_ekv_sentinel);

  // The keyvalues carry the text and its style.
  using Pair = std::pair<std::string, std::string>;
  const std::vector<Pair> expected{{"message_text", "gate label"},
                                   {"enabled", "1"},
                                   {"fullbright", "1"},
                                   {"font_name", "Reaver"},
                                   {"font_size", "120.000000"},
                                   {"world_units_per_pixel", "0.028571"},
                                   {"color", "64,255,160,255"},
                                   {"justify_horizontal", "0"},
                                   {"justify_vertical", "0"},
                                   {"reorient_mode", "1"}};
  EXPECT_EQ(g_recorder.key_values, expected);
}

TEST_F(WorldTextGameFactoryTest, TraceDeduplicatesCreateFailuresAndLogsOneSuccess) {
  // Break the keyvalue surface.
  ScopedInteropTrace trace;
  auto calls = RecordingCalls();
  calls.key_values = {};
  WorldTextGameFactory broken(calls);

  // Two failures log once.
  testing::internal::CaptureStderr();
  EXPECT_FALSE(broken.Create("text", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{}));
  EXPECT_FALSE(broken.Create("text", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{}));
  const std::string failures = testing::internal::GetCapturedStderr();
  EXPECT_EQ(CountOccurrences(failures, "world-text create failed:"), 1u);

  // Two successes log once.
  testing::internal::CaptureStderr();
  auto first = factory_.Create("first", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  auto second =
      factory_.Create("second", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const std::string successes = testing::internal::GetCapturedStderr();
  EXPECT_EQ(CountOccurrences(successes, "world-text create succeeded: point_worldtext"), 1u);
}

TEST_F(WorldTextGameFactoryTest, SetMessageChangesTheTextInPlace) {
  // Create a text.
  auto created =
      factory_.Create("before", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_TRUE(created.has_value());

  // Changing the message sends one input.
  g_recorder.steps.clear();
  created->get()->SetMessage("after");
  ASSERT_EQ(g_recorder.steps.size(), 1);
  EXPECT_EQ(g_recorder.steps[0], "accept-input:SetMessage:after");
  EXPECT_EQ(g_removed_count, 0);
}

TEST_F(WorldTextGameFactoryTest, SetMessageRebuildsWhenTheInputIsRefused) {
  // Create a text above the origin.
  modlock::Vec3 origin;
  origin.set_z(5);
  auto created = factory_.Create("before", origin, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_TRUE(created.has_value());

  // The game refuses the new message.
  g_recorder.steps.clear();
  g_refuse_set_message = true;
  created->get()->SetMessage("after");
  g_refuse_set_message = false;

  // The refused input is followed by a full rebuild.
  ASSERT_EQ(g_recorder.steps.size(), 8);
  EXPECT_EQ(g_recorder.steps[0], "accept-input:SetMessage:after");
  g_recorder.steps.erase(g_recorder.steps.begin());
  ASSERT_EQ(g_recorder.steps.size(), 7);
  EXPECT_EQ(g_recorder.steps[0], "create:point_worldtext");
  EXPECT_EQ(g_removed_count, 1);  // The stale entity left before the new one queued.

  // Removing it removes only the new entity.
  created->get()->Remove();
  EXPECT_EQ(g_removed_count, 2);
}

TEST_F(WorldTextGameFactoryTest, UnresolvedEntitySystemFailsWithANamedError) {
  // Leave the entity system unresolved; creation fails and names it.
  auto calls = RecordingCalls();
  calls.entity_system = nullptr;
  WorldTextGameFactory broken(calls);
  auto created = broken.Create("text", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_FALSE(created.has_value());
  EXPECT_NE(created.error().find("creation unresolved"), std::string::npos) << created.error();
}

TEST_F(WorldTextGameFactoryTest, UnrecordedKeyValuesSurfaceFailsWithoutSpawning) {
  // Break the keyvalue surface; creation fails and names it.
  auto calls = RecordingCalls();
  calls.key_values = {};
  WorldTextGameFactory broken(calls);
  auto created = broken.Create("text", modlock::Vec3{}, modlock::EulerAngles{}, WorldTextStyle{});
  ASSERT_FALSE(created.has_value());
  EXPECT_NE(created.error().find("construction is unresolved"), std::string::npos)
      << created.error();

  // The surface check runs first: nothing reaches the world at all.
  EXPECT_TRUE(g_recorder.steps.empty());
  EXPECT_EQ(g_removed_count, 0);
}

}  // namespace
