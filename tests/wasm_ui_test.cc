// Contract tests for a mod's interface trees: changes apply in order, a change
// that breaks a limit leaves the tree as it was, and only a shown button takes
// presses. A departed player's slot takes only a reset.
#include <string>

#include "gtest/gtest.h"
#include "wasm/ui.h"

namespace {

using modlock::ui::Change;
using modlock::ui::Kind;
using modlock::wasm::UiTrees;

// Set returns a change that sets one node.
Change Set(std::string id, Kind kind, bool reset = false) {
  Change change;
  change.set_reset(reset);
  auto* node = change.add_set();
  node->set_id(std::move(id));
  node->set_kind(kind);
  return change;
}

TEST(UiTrees, TracksButtons) {
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("ready", modlock::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(3, Set("score", modlock::ui::KIND_LABEL)));
  EXPECT_TRUE(trees.IsButton(3, "ready"));
  EXPECT_FALSE(trees.IsButton(3, "score"));
  EXPECT_FALSE(trees.IsButton(4, "ready"));

  // Removing and resetting both take the button away.
  Change removed;
  removed.add_removed("ready");
  ASSERT_TRUE(trees.Apply(3, removed));
  EXPECT_FALSE(trees.IsButton(3, "ready"));
  ASSERT_TRUE(trees.Apply(3, Set("ready", modlock::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::ui::KIND_PANEL, true)));
  EXPECT_FALSE(trees.IsButton(3, "ready"));
}

TEST(UiTrees, RefusesWhatCannotBeDrawn) {
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(0, Set("ready", modlock::ui::KIND_BUTTON)));

  auto web = Set("logo", modlock::ui::KIND_IMAGE);
  web.mutable_set(0)->set_image("https://example.com/logo.png");
  EXPECT_FALSE(trees.Apply(0, web));
  web.mutable_set(0)->set_image("file://{images}/heroes/wraith_card.psd");
  EXPECT_TRUE(trees.Apply(0, web));
  EXPECT_FALSE(trees.Apply(0, Set(std::string(65, 'x'), modlock::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.Apply(64, Set("ready", modlock::ui::KIND_BUTTON)));

  // A change that grows the tree past its bound applies none of its nodes.
  Change large;
  large.set_reset(true);
  for (int i = 0; i <= 512; ++i) large.add_set()->set_id(std::to_string(i));
  EXPECT_FALSE(trees.Apply(0, large));
  EXPECT_TRUE(trees.IsButton(0, "ready"));
}

TEST(UiTrees, ForgetsPlayers) {
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(1, Set("a", modlock::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(2, Set("b", modlock::ui::KIND_BUTTON)));
  EXPECT_TRUE(trees.Drop(1));
  EXPECT_FALSE(trees.Drop(1));

  // The departed slot takes only a reset, which ends the departure.
  EXPECT_FALSE(trees.Apply(1, Set("a", modlock::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(1, Set("a", modlock::ui::KIND_BUTTON, true)));
  ASSERT_TRUE(trees.Apply(1, Set("c", modlock::ui::KIND_LABEL)));
  EXPECT_TRUE(trees.Drop(1));
  EXPECT_EQ(trees.Clear(), std::vector<int32_t>{2});
  EXPECT_TRUE(trees.Clear().empty());
}

}  // namespace
