// Contract tests for a mod's interface trees: changes apply in order, a change
// that breaks a limit leaves the tree as it was, layouts and style sheets stay
// inside the mod's addon, only a shown button takes presses, and only a value a
// shown input can hold reaches the mod. A departed player's slot takes only a
// reset.
#include <string>

#include "gtest/gtest.h"
#include "wasm/ui.h"

namespace {

using modlock::wasm::UiTrees;
using modlock::wasm::ui::Change;
using modlock::wasm::ui::Kind;

// Set returns a change that sets one node.
Change Set(std::string id, Kind kind, bool reset = false) {
  // Build a change that sets one node.
  Change change;
  change.set_reset(reset);
  auto* node = change.add_set();
  node->set_id(std::move(id));
  node->set_kind(kind);
  return change;
}

TEST(UiTrees, TracksButtons) {
  // A button is tracked per slot.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("ready", modlock::wasm::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(3, Set("score", modlock::wasm::ui::KIND_LABEL)));
  EXPECT_TRUE(trees.IsButton(3, "ready"));
  EXPECT_FALSE(trees.IsButton(3, "score"));
  EXPECT_FALSE(trees.IsButton(4, "ready"));

  // Removing and resetting both take the button away.
  Change removed;
  removed.add_removed("ready");
  ASSERT_TRUE(trees.Apply(3, removed));
  EXPECT_FALSE(trees.IsButton(3, "ready"));
  ASSERT_TRUE(trees.Apply(3, Set("ready", modlock::wasm::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL, true)));
  EXPECT_FALSE(trees.IsButton(3, "ready"));
}

TEST(UiTrees, TakesAbilityBarPressesOnlyWhileTheRootAsks) {
  // A root without the ask takes no bar press, even with a button of that id.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  ASSERT_TRUE(trees.Apply(3, Set("ability:0", modlock::wasm::ui::KIND_BUTTON)));
  EXPECT_FALSE(trees.IsButton(3, "ability:0"));

  // A root that asks takes a press on each of the four slots and nothing else.
  auto asks = Set("", modlock::wasm::ui::KIND_PANEL);
  asks.mutable_set(0)->set_ability_bar_presses(true);
  ASSERT_TRUE(trees.Apply(3, asks));
  for (const char* node : {"ability:0", "ability:1", "ability:2", "ability:3"}) {
    EXPECT_TRUE(trees.IsButton(3, node)) << node;
  }
  for (const char* node : {"ability:4", "ability:", "ability:00", "ability:a"}) {
    EXPECT_FALSE(trees.IsButton(3, node)) << node;
  }
  EXPECT_FALSE(trees.IsButton(4, "ability:0"));

  // Only the root's ask counts, and a root that stops asking takes none.
  auto child = Set("panel", modlock::wasm::ui::KIND_PANEL);
  child.mutable_set(0)->set_ability_bar_presses(true);
  ASSERT_TRUE(trees.Apply(4, Set("", modlock::wasm::ui::KIND_PANEL)));
  ASSERT_TRUE(trees.Apply(4, child));
  EXPECT_FALSE(trees.IsButton(4, "ability:0"));
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.IsButton(3, "ability:0"));
}

TEST(UiTrees, ChecksTheAbilityDisplay) {
  // A row of four abilities with pictures from the game's content is drawn.
  UiTrees trees;
  auto shown = Set("", modlock::wasm::ui::KIND_PANEL);
  auto* row = shown.mutable_set(0)->add_ability_display();
  row->set_player(5);
  row->set_name("Player One");
  row->set_hero("hero_atlas");
  for (int slot = 0; slot < 4; ++slot) {
    auto* ability = row->add_abilities();
    ability->set_ability(7);
    ability->set_image("file://{images}/hud/abilities/bull_drain.psd");
    ability->set_upgrades(0b11);
  }
  EXPECT_TRUE(trees.Apply(3, shown));

  // A fifth ability, a picture from the web, a slot past the players or a hero
  // that is not a word is refused.
  auto fifth = shown;
  fifth.mutable_set(0)->mutable_ability_display(0)->add_abilities();
  EXPECT_FALSE(trees.Apply(3, fifth));
  auto web = shown;
  web.mutable_set(0)->mutable_ability_display(0)->mutable_abilities(1)->set_image(
      "https://example.com/a.png");
  EXPECT_FALSE(trees.Apply(3, web));
  auto slot = shown;
  slot.mutable_set(0)->mutable_ability_display(0)->set_player(64);
  EXPECT_FALSE(trees.Apply(3, slot));
  auto hero = shown;
  hero.mutable_set(0)->mutable_ability_display(0)->set_hero("hero atlas");
  EXPECT_FALSE(trees.Apply(3, hero));
  auto name = shown;
  name.mutable_set(0)->mutable_ability_display(0)->set_name(std::string(129, 'a'));
  EXPECT_FALSE(trees.Apply(3, name));

  // A slot that shows the game's own takes no picture; a bot's row no name.
  auto plain = shown;
  plain.mutable_set(0)->mutable_ability_display(0)->mutable_abilities(2)->clear_image();
  plain.mutable_set(0)->mutable_ability_display(0)->clear_name();
  EXPECT_TRUE(trees.Apply(3, plain));
}

TEST(UiTrees, RefusesWhatCannotBeDrawn) {
  // Track a button.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(0, Set("ready", modlock::wasm::ui::KIND_BUTTON)));

  // Images come only from the game, and ids and slots are bounded.
  auto web = Set("logo", modlock::wasm::ui::KIND_IMAGE);
  web.mutable_set(0)->set_image("https://example.com/logo.png");
  EXPECT_FALSE(trees.Apply(0, web));
  web.mutable_set(0)->set_image("file://{images}/heroes/wraith_card.psd");
  EXPECT_TRUE(trees.Apply(0, web));
  EXPECT_FALSE(trees.Apply(0, Set(std::string(65, 'x'), modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.Apply(64, Set("ready", modlock::wasm::ui::KIND_BUTTON)));

  // A press writes an id into a console command, so an id is a word that may
  // hold '-', '.' and ':' and nothing that starts another command.
  for (const char* id : {"a b", "a;quit", "a\nquit", "a\"b", "a/b"}) {
    EXPECT_FALSE(trees.Apply(0, Set(id, modlock::wasm::ui::KIND_BUTTON))) << id;
  }
  EXPECT_TRUE(trees.Apply(0, Set("route:scores.v2", modlock::wasm::ui::KIND_BUTTON)));

  // A layout names an .xml file inside the mod's addon and nothing outside it.
  auto layout = Set("inventory", modlock::wasm::ui::KIND_LAYOUT);
  for (const char* path : {"", "inventory.js", "../hyperline/session.xml", "a//b.xml", "a b.xml"}) {
    layout.mutable_set(0)->set_layout(path);
    EXPECT_FALSE(trees.Apply(0, layout)) << path;
  }
  layout.mutable_set(0)->set_layout("menus/inventory.xml");
  EXPECT_TRUE(trees.Apply(0, layout));
  layout.mutable_set(0)->set_data(std::string(8193, 'x'));
  EXPECT_FALSE(trees.Apply(0, layout));

  // A style sheet names a .css file inside the addon, and a class is a word.
  auto styled = Set("", modlock::wasm::ui::KIND_PANEL);
  for (const char* path : {"hud.xml", "../hyperline/hud.css", "a//b.css"}) {
    styled.mutable_set(0)->clear_sheets();
    styled.mutable_set(0)->add_sheets(path);
    EXPECT_FALSE(trees.Apply(0, styled)) << path;
  }
  styled.mutable_set(0)->clear_sheets();
  styled.mutable_set(0)->add_sheets("mymod/hud.css");
  styled.mutable_set(0)->add_classes("Board-gold");
  EXPECT_TRUE(trees.Apply(0, styled));
  styled.mutable_set(0)->add_classes("a b");
  EXPECT_FALSE(trees.Apply(0, styled));

  // A change that grows the tree past its bound applies none of its nodes.
  Change large;
  large.set_reset(true);
  for (int i = 0; i <= 512; ++i) {
    large.add_set()->set_id(std::to_string(i));
  }
  EXPECT_FALSE(trees.Apply(0, large));
  EXPECT_TRUE(trees.IsButton(0, "ready"));
}

TEST(UiTrees, ChecksInputs) {
  // Show an entry, a slider, a toggle and a dropdown.
  UiTrees trees;
  Change change;
  auto add = [&](const char* id) {
    auto* node = change.add_set();
    node->set_id(id);
    node->set_kind(modlock::wasm::ui::KIND_INPUT);
    return node;
  };
  add("name")->mutable_text_entry()->set_max_length(4);
  auto* slider = add("volume")->mutable_slider();
  slider->set_min(0);
  slider->set_max(10);
  slider->set_step(2.5F);
  slider->set_wheel(true);
  add("ready")->mutable_toggle();
  auto* team = add("team")->mutable_dropdown();
  team->add_options("Red");
  team->add_options("Blue");
  ASSERT_TRUE(trees.Apply(0, change));

  // Text arrives in base64url, losing control characters and what passes
  // the entry's length; malformed text is refused.
  EXPECT_EQ(trees.Input(0, "name", "SGkK8J-Ygg")->text(), "Hi\xf0\x9f\x98\x82");
  EXPECT_EQ(trees.Input(0, "name", "YWJjZGVm")->text(), "abcd");
  EXPECT_FALSE(trees.Input(0, "name", "wA"));
  EXPECT_FALSE(trees.Input(0, "name", "a b"));

  // A slider's number is clamped and snapped, and a dropdown's index and a
  // toggle's state must be ones the control holds.
  EXPECT_FLOAT_EQ(trees.Input(0, "volume", "6.1")->number(), 5);
  EXPECT_FLOAT_EQ(trees.Input(0, "volume", "99")->number(), 10);
  EXPECT_FALSE(trees.Input(0, "volume", "nan"));
  EXPECT_TRUE(trees.Input(0, "ready", "1")->on());
  EXPECT_FALSE(trees.Input(0, "ready", "yes"));
  EXPECT_EQ(trees.Input(0, "team", "1")->choice(), 1U);
  EXPECT_FALSE(trees.Input(0, "team", "2"));

  // Wheel input has a bounded anchor and never becomes a seek or another control's edit.
  const auto wheel = trees.Input(0, "volume", "wheel:-1:0.25");
  ASSERT_TRUE(wheel);
  EXPECT_FLOAT_EQ(wheel->wheel().delta(), -1);
  EXPECT_FLOAT_EQ(wheel->wheel().fraction(), 0.25F);
  EXPECT_FLOAT_EQ(trees.Input(0, "volume", "wheel:1:9")->wheel().fraction(), 1);
  EXPECT_FALSE(trees.Input(0, "volume", "wheel:2:0.5"));
  EXPECT_FALSE(trees.Input(0, "volume", "wheel:1:nan"));
  EXPECT_FALSE(trees.Input(0, "volume", "wheel:1:0.5:0"));
  EXPECT_FALSE(trees.Input(0, "name", "wheel:1:0.5"));
  EXPECT_FALSE(trees.Input(1, "volume", "wheel:1:0.5"));
  auto opted_out = Set("volume", modlock::wasm::ui::KIND_INPUT);
  opted_out.mutable_set(0)->mutable_slider();
  ASSERT_TRUE(trees.Apply(0, opted_out));
  EXPECT_FALSE(trees.Input(0, "volume", "wheel:1:0.5"));
  opted_out.mutable_set(0)->mutable_slider()->set_wheel(true);
  opted_out.mutable_set(0)->set_disabled(true);
  ASSERT_TRUE(trees.Apply(0, opted_out));
  EXPECT_FALSE(trees.Input(0, "volume", "wheel:1:0.5"));

  // Another slot, a node that is no input and a disabled input take nothing.
  EXPECT_FALSE(trees.Input(1, "ready", "1"));
  ASSERT_TRUE(trees.Apply(0, Set("ready", modlock::wasm::ui::KIND_BUTTON)));
  EXPECT_FALSE(trees.Input(0, "ready", "1"));
  auto disabled = Set("ready", modlock::wasm::ui::KIND_INPUT);
  disabled.mutable_set(0)->mutable_toggle();
  disabled.mutable_set(0)->set_disabled(true);
  ASSERT_TRUE(trees.Apply(0, disabled));
  EXPECT_FALSE(trees.Input(0, "ready", "1"));

  // An input needs a control, and a token is '#' then a word.
  EXPECT_FALSE(trees.Apply(0, Set("bare", modlock::wasm::ui::KIND_INPUT)));
  auto label = Set("hero", modlock::wasm::ui::KIND_LABEL);
  label.mutable_set(0)->set_token("#Citadel_HeroName_Haze");
  EXPECT_TRUE(trees.Apply(0, label));
  label.mutable_set(0)->set_token("#{s:x}");
  EXPECT_FALSE(trees.Apply(0, label));
}

TEST(UiTrees, ForgetsPlayers) {
  // Two players, and the first departs.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(1, Set("a", modlock::wasm::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(2, Set("b", modlock::wasm::ui::KIND_BUTTON)));
  EXPECT_TRUE(trees.Drop(1));
  EXPECT_FALSE(trees.Drop(1));

  // The departed slot takes only a reset, which ends the departure.
  EXPECT_FALSE(trees.Apply(1, Set("a", modlock::wasm::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(1, Set("a", modlock::wasm::ui::KIND_BUTTON, true)));
  ASSERT_TRUE(trees.Apply(1, Set("c", modlock::wasm::ui::KIND_LABEL)));
  EXPECT_TRUE(trees.Drop(1));
  EXPECT_EQ(trees.Clear(), std::vector<int32_t>{2});
  EXPECT_TRUE(trees.Clear().empty());
}

}  // namespace
