// Contract tests for ThunkOwner, the active-ownership mechanics EngineFrameHook
// and StartupServerProbe share: a moved-from owner is inert, the final owner
// restores the dispatch-table entry before clearing thunk state, and move
// assignment tears down the old duty before taking over the new one. The
// fixtures are writable fake tables through the public VtableSlotHook::Install
// path; no production backdoor.
#include <cstddef>
#include <utility>

#include "gtest/gtest.h"
#include "modlock/gameinterop/thunk_owner.h"

namespace {

using modlock::gameinterop::ThunkOwner;
using modlock::gameinterop::VtableSlotHook;

void TargetA() {}
void TargetB() {}
void Replacement() {}

void* Ptr(void (*fn)()) {
  return reinterpret_cast<void*>(fn);
}

struct FakeObject {
  void* vptr;
};

// Recorder stands in for one hook's singleton thunk state: it counts clears
// and, while clearing, proves the table entry is already back to the original.
struct Recorder {
  int clears = 0;
  bool restore_before_clear = true;
  void** slot = nullptr;
  void* original = nullptr;

  void Clear() {
    ++clears;
    if (slot != nullptr && *slot != original) {
      restore_before_clear = false;
    }
  }
};

// ThunkOwner::ClearThunk is a plain function pointer, so the active recorder
// rides one file-scope pointer; tests run single-threaded.
Recorder* g_active_recorder = nullptr;

void ClearActiveRecorder() {
  if (g_active_recorder != nullptr) {
    g_active_recorder->Clear();
  }
}

TEST(ThunkOwner, MovedFromOwnerDestructionLeavesTheHookLive) {
  // Hook a slot and record clears.
  void* table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject object{table};
  auto hook = VtableSlotHook::Install(&object, 1, Ptr(Replacement));
  ASSERT_TRUE(hook.has_value());
  Recorder recorder;
  g_active_recorder = &recorder;

  // An active wrapper moves its duty away, then the moved-from wrapper is
  // destroyed while the new owner lives.
  auto* active = new ThunkOwner(std::move(*hook), &ClearActiveRecorder);
  ThunkOwner taken_over(std::move(*active));
  delete active;  // the moved-from owner must be inert

  // The owner that took over still holds the duty: the table stays patched,
  // the thunk state was never cleared, and the entry still dispatches to the
  // replacement.
  EXPECT_EQ(table[1], Ptr(Replacement));
  EXPECT_EQ(recorder.clears, 0);
  reinterpret_cast<void (*)()>(table[1])();
  g_active_recorder = nullptr;
}

TEST(ThunkOwner, FinalOwnerRestoresBeforeClearingThunkState) {
  // Hook a slot and record the order of restore and clear.
  void* table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject object{table};
  auto hook = VtableSlotHook::Install(&object, 1, Ptr(Replacement));
  ASSERT_TRUE(hook.has_value());
  Recorder recorder;
  recorder.slot = &table[1];
  recorder.original = Ptr(TargetB);
  g_active_recorder = &recorder;

  // The last owner restores the slot, then clears the thunk state.
  {
    ThunkOwner active(std::move(*hook), &ClearActiveRecorder);
    EXPECT_EQ(table[1], Ptr(Replacement));
  }
  g_active_recorder = nullptr;
  EXPECT_TRUE(recorder.restore_before_clear);
  EXPECT_EQ(recorder.clears, 1);
  EXPECT_EQ(table[1], Ptr(TargetB));
}

TEST(ThunkOwner, MoveAssignmentTearsDownOldDutyBeforeTakeover) {
  // Two objects, each with its own table.
  void* old_table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject old_object{old_table};
  void* new_table[2] = {Ptr(TargetA), Ptr(TargetB)};
  FakeObject new_object{new_table};

  // Hook the old object and record its restore.
  Recorder old_recorder;
  auto old_hook = VtableSlotHook::Install(&old_object, 1, Ptr(Replacement));
  ASSERT_TRUE(old_hook.has_value());
  old_recorder.slot = &old_table[1];
  old_recorder.original = Ptr(TargetB);

  // Hook the new object.
  Recorder next_recorder;
  auto next_hook = VtableSlotHook::Install(&new_object, 1, Ptr(Replacement));
  ASSERT_TRUE(next_hook.has_value());

  // Give each hook an owner.
  ThunkOwner* active = new ThunkOwner(std::move(*old_hook), &ClearActiveRecorder);
  ThunkOwner successor(std::move(*next_hook), &ClearActiveRecorder);

  // Move the successor over the active owner.
  g_active_recorder = &old_recorder;
  *active = std::move(successor);
  g_active_recorder = nullptr;

  // The takeover fully tore down the old duty first: its table is back to the
  // original and its state cleared exactly once. Exactly one live duty
  // remains - the new table stays patched with no extra clears.
  EXPECT_EQ(old_recorder.clears, 1);
  EXPECT_EQ(old_table[1], Ptr(TargetB));
  EXPECT_EQ(new_table[1], Ptr(Replacement));
  EXPECT_EQ(next_recorder.clears, 0);

  // The final destruction releases only the successor's duty.
  g_active_recorder = &next_recorder;
  delete active;
  g_active_recorder = nullptr;
  EXPECT_EQ(next_recorder.clears, 1);
  EXPECT_EQ(new_table[1], Ptr(TargetB));
  EXPECT_EQ(old_recorder.clears, 1);
}

}  // namespace
