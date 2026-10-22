// Contract tests for the engine frame hook seam: dispatch-table entry
// patching, original preservation, restore, and the fail-closed fallback when
// the Windows-only install cannot resolve its module.
#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

#include "modlock/gameinterop/frame_hook.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace {

using modlock::gameinterop::EngineFrameHook;
using modlock::gameinterop::VtableSlotHook;

void TargetA() {}
void TargetB() {}
void Replacement() {}

void* Ptr(void (*fn)()) {
  return reinterpret_cast<void*>(fn);
}

// Fixture: one object whose vptr selects a table the test owns.
struct FakeObject {
  void* vptr;
};

void** DispatchTableOf(FakeObject& object) {
  return *reinterpret_cast<void***>(&object);
}

TEST(VtableSlotHook, RedirectsSlotAndPreservesOriginal) {
  // Hook slot 1 of a three slot table.
  void* table[3] = {Ptr(TargetA), Ptr(TargetB), Ptr(TargetA)};
  FakeObject object{table};
  auto hook = VtableSlotHook::Install(&object, 1, Ptr(Replacement));
  ASSERT_TRUE(hook.has_value());

  // The object still dispatches through its own table; slot 1 is the
  // replacement and the original target is reported for call-through.
  EXPECT_EQ(DispatchTableOf(object), table);
  EXPECT_EQ(table[1], Ptr(Replacement));
  EXPECT_EQ(hook->Original(), Ptr(TargetB));

  // Every other slot keeps its target.
  EXPECT_EQ(table[0], Ptr(TargetA));
  EXPECT_EQ(table[2], Ptr(TargetA));

  // Restoring puts the original back.
  hook->Restore();
  EXPECT_EQ(table[1], Ptr(TargetB));
}

// Regression for the deterministic engine2.dll+0x1d54da fault: the engine
// dispatches ISource2Server through slots modlock never names - the crash
// came from `call qword ptr [rax+0x300]`, slot 96. A hook that repointed the
// object at a fixed-size copy of the table sent that call past the end of the
// copy and into heap bytes. The table the object dispatches through must stay
// the module's own, whatever its length.
TEST(VtableSlotHook, LeavesUnnamedSlotsDispatchableThroughTheModuleTable) {
  // Hook the frame slot of a full sized table.
  constexpr size_t kSlots = 128;
  const size_t kHookedSlot = modlock::gameinterop::FindVirtualSlot("game-server.game-frame")->index;
  constexpr size_t kEngineSlot = 0x300 / sizeof(void*);

  // Fill the table, with a distinct engine function at 0x300.
  void* table[kSlots];
  for (void*& entry : table) {
    entry = Ptr(TargetA);
  }
  table[kEngineSlot] = Ptr(TargetB);
  FakeObject object{table};

  // Hook the frame slot.
  auto hook = VtableSlotHook::Install(&object, kHookedSlot, Ptr(Replacement));
  ASSERT_TRUE(hook.has_value());

  // The engine's slot still reaches the module's function.
  void** dispatch = DispatchTableOf(object);
  EXPECT_EQ(dispatch, table);
  EXPECT_EQ(dispatch[kHookedSlot], Ptr(Replacement));
  ASSERT_EQ(dispatch[kEngineSlot], Ptr(TargetB));

  // The engine's own call through that slot must reach the module's function.
  reinterpret_cast<void (*)()>(dispatch[kEngineSlot])();

  // Restoring puts the original back.
  hook->Restore();
  EXPECT_EQ(table[kHookedSlot], Ptr(TargetA));
}

TEST(VtableSlotHook, DestructionRestoresTheOriginalSlotTarget) {
  void* table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject object{table};
  {
    auto hook = VtableSlotHook::Install(&object, 0, Ptr(Replacement));
    ASSERT_TRUE(hook.has_value());
    EXPECT_EQ(table[0], Ptr(Replacement));
  }
  EXPECT_EQ(table[0], Ptr(TargetA));
}

TEST(VtableSlotHook, MoveTransfersTheRestoreDuty) {
  // A moved hook restores the slot.
  void* table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject object{table};
  auto hook = VtableSlotHook::Install(&object, 1, Ptr(Replacement));
  ASSERT_TRUE(hook.has_value());
  VtableSlotHook moved = std::move(*hook);
  moved.Restore();
  EXPECT_EQ(table[1], Ptr(TargetB));
}

TEST(VtableSlotHook, SelfMoveKeepsTheSlotPatched) {
  // A hook moved onto itself keeps the slot patched and restores once.
  void* table[] = {Ptr(TargetA)};
  FakeObject object{table};
  auto hook = VtableSlotHook::Install(&object, 0, Ptr(Replacement));
  ASSERT_TRUE(hook);
  auto& same_hook = *hook;
  *hook = std::move(same_hook);

  // The slot still holds the replacement until restored.
  EXPECT_EQ(table[0], Ptr(Replacement));
  EXPECT_EQ(hook->Original(), Ptr(TargetA));
  hook->Restore();
  EXPECT_EQ(table[0], Ptr(TargetA));
}

TEST(VtableSlotHook, RejectsAnObjectWithoutADispatchTable) {
  FakeObject object{nullptr};
  auto hook = VtableSlotHook::Install(&object, 5, Ptr(Replacement));
  ASSERT_FALSE(hook.has_value());
  EXPECT_NE(hook.error().find("null vtable pointer"), std::string::npos);
}

TEST(EngineFrameHook, InstallFailsClosedWithoutMappedServerModule) {
  // No game modules are mapped in this process (and non-Windows builds never
  // map them), so the only correct outcome is a named error: callers keep the
  // local pump as their frame source.
  auto hook = EngineFrameHook::Install([] {});
  ASSERT_FALSE(hook.has_value());
  EXPECT_FALSE(hook.error().empty());
}

}  // namespace
