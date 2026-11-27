// Contract tests for a mod's interface trees: changes apply in order, a change
// that breaks a limit leaves the tree as it was, layouts and style sheets stay
// inside the mod's addon, only a shown button takes presses, and only a value a
// shown input can hold reaches the mod, and a layout's press reaches it only
// from the player whose tree shows the layout, small and at a player's pace. A
// departed player's slot takes only a reset.
#include <chrono>
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

TEST(UiTrees, TakesGameActionsOnlyWhileTheRootFollowsThem) {
  // A root that follows nothing takes no game action.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.GameAction(3, "1 1"));

  // A root that follows the scoreboard takes each phase of it.
  auto follows = Set("", modlock::wasm::ui::KIND_PANEL);
  follows.mutable_set(0)->add_game_actions(modlock::wasm::ui::GAME_ACTION_SCOREBOARD);
  ASSERT_TRUE(trees.Apply(3, follows));
  auto pressed = trees.GameAction(3, "1 1");
  ASSERT_TRUE(pressed) << pressed.error();
  EXPECT_EQ(pressed->player(), 3);
  EXPECT_EQ(pressed->action(), modlock::wasm::ui::GAME_ACTION_SCOREBOARD);
  EXPECT_EQ(pressed->phase(), modlock::wasm::ui::ACTION_PHASE_PRESSED);
  EXPECT_EQ(trees.GameAction(3, "1 3")->phase(), modlock::wasm::ui::ACTION_PHASE_CANCELLED);

  // An unknown action or phase, another player and a malformed line are refused.
  for (const char* line : {"2 1", "1 0", "1 4", "1", "1 1 1", "a 1", "1 -1", ""}) {
    EXPECT_FALSE(trees.GameAction(3, line)) << line;
  }
  EXPECT_FALSE(trees.GameAction(4, "1 1"));

  // A child's list does not count, and a root that stops following takes none.
  auto child = Set("panel", modlock::wasm::ui::KIND_PANEL);
  child.mutable_set(0)->add_game_actions(modlock::wasm::ui::GAME_ACTION_SCOREBOARD);
  ASSERT_TRUE(trees.Apply(4, Set("", modlock::wasm::ui::KIND_PANEL)));
  ASSERT_TRUE(trees.Apply(4, child));
  EXPECT_FALSE(trees.GameAction(4, "1 1"));
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.GameAction(3, "1 2"));
}

TEST(UiTrees, TakesOverlaysOnlyWhileTheRootFollowsThem) {
  // A root that does not follow overlays takes none.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.Overlay(3, "1"));

  // A root that follows them takes each overlay, none included.
  auto follows = Set("", modlock::wasm::ui::KIND_PANEL);
  follows.mutable_set(0)->set_follows_overlay(true);
  ASSERT_TRUE(trees.Apply(3, follows));
  auto chat = trees.Overlay(3, "1");
  ASSERT_TRUE(chat) << chat.error();
  EXPECT_EQ(chat->player(), 3);
  EXPECT_EQ(chat->overlay(), modlock::wasm::ui::OVERLAY_CHAT);
  EXPECT_EQ(trees.Overlay(3, "0")->overlay(), modlock::wasm::ui::OVERLAY_NONE);
  EXPECT_EQ(trees.Overlay(3, "6")->overlay(), modlock::wasm::ui::OVERLAY_ESCAPE_MENU);

  // An unknown overlay, another player and a malformed line are refused.
  for (const char* line : {"7", "-1", "1 1", "a", ""}) {
    EXPECT_FALSE(trees.Overlay(3, line)) << line;
  }
  EXPECT_FALSE(trees.Overlay(4, "1"));

  // A child's flag does not count, and a root that stops following takes none.
  auto child = Set("panel", modlock::wasm::ui::KIND_PANEL);
  child.mutable_set(0)->set_follows_overlay(true);
  ASSERT_TRUE(trees.Apply(4, Set("", modlock::wasm::ui::KIND_PANEL)));
  ASSERT_TRUE(trees.Apply(4, child));
  EXPECT_FALSE(trees.Overlay(4, "1"));
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.Overlay(3, "1"));
}

// Measured returns the bounds event reports for name, or empty bounds.
modlock::wasm::ui::Bounds Measured(const modlock::wasm::ui::UiPlacementEvent& event,
                                   std::string_view name) {
  for (const auto& each : event.measured()) {
    if (each.name() == name) {
      return each.bounds();
    }
  }
  return {};
}

TEST(UiTrees, MergesEachPlacementReportIntoTheWholePlacement) {
  // A root that does not follow placement takes no report.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, Set("", modlock::wasm::ui::KIND_PANEL)));
  EXPECT_FALSE(trees.Placement(3, "2520 1080 300 0 1920 1080"));

  // A following root with two measured nodes takes the screen, the HUD and
  // both boxes.
  auto follows = Set("", modlock::wasm::ui::KIND_PANEL);
  follows.mutable_set(0)->set_follows_placement(true);
  auto* preview = follows.add_set();
  preview->set_id("preview");
  preview->set_measure("preview");
  auto* bar = follows.add_set();
  bar->set_id("bar");
  bar->set_measure("bar");
  ASSERT_TRUE(trees.Apply(3, follows));
  auto first = trees.Placement(3, "2520 1080 300 0 1920 1080 preview 10 20 300 200 bar 0 -5 40 8");
  ASSERT_TRUE(first) << first.error();
  EXPECT_EQ(first->player(), 3);
  EXPECT_EQ(first->screen().width(), 2520);
  EXPECT_EQ(first->hud().x(), 300);
  EXPECT_EQ(Measured(*first, "bar").y(), -5);

  // A report of one change still holds the other box, and a name the tree
  // does not measure is left out.
  auto second = trees.Placement(3, "2520 1080 300 0 1920 1080 preview 12 20 300 200 other 1 1 1 1");
  ASSERT_TRUE(second) << second.error();
  EXPECT_EQ(second->measured().size(), 2U);
  EXPECT_EQ(Measured(*second, "preview").x(), 12);
  EXPECT_EQ(Measured(*second, "bar").width(), 40);

  // Once the tree drops a measured node, its box leaves the placement.
  modlock::wasm::ui::Change removed;
  removed.add_removed("bar");
  ASSERT_TRUE(trees.Apply(3, removed));
  auto third = trees.Placement(3, "2560 1080 0 0 2560 1080");
  ASSERT_TRUE(third) << third.error();
  EXPECT_EQ(third->measured().size(), 1U);
  EXPECT_EQ(third->measured(0).name(), "preview");

  // A malformed report, a negative size and another player are refused.
  for (const char* line : {"", "2520 1080", "2520 1080 0 0 1920", "2520 1080 0 0 -1 1080",
                           "2520 1080 0 0 1920 1080 preview 1 1 1", "a 1080 0 0 1920 1080",
                           "2520  1080 0 0 1920 1080"}) {
    EXPECT_FALSE(trees.Placement(3, line)) << line;
  }
  EXPECT_FALSE(trees.Placement(4, "2520 1080 300 0 1920 1080"));

  // A measure name is a word of at most 64 bytes.
  auto bad = Set("odd", modlock::wasm::ui::KIND_PANEL);
  bad.mutable_set(0)->set_measure("has space");
  EXPECT_FALSE(trees.Apply(3, bad));
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

  // A range names the handle each hold takes, and a release carries every
  // handle's value, clamped and snapped, in order.
  auto delay = Set("delay", modlock::wasm::ui::KIND_INPUT);
  auto* range = delay.mutable_set(0)->mutable_range();
  range->set_max(1);
  range->set_step(0.25F);
  for (const auto* name : {"min", "favored", "max"}) {
    range->add_handles()->set_name(name);
  }
  ASSERT_TRUE(trees.Apply(0, delay));
  const auto grabbed = trees.Input(0, "delay", "range:1:favored");
  ASSERT_TRUE(grabbed);
  EXPECT_EQ(grabbed->range().phase(), modlock::wasm::ui::ACTION_PHASE_PRESSED);
  EXPECT_EQ(grabbed->range().handle(), "favored");
  EXPECT_TRUE(grabbed->range().values().empty());
  const auto released = trees.Input(0, "delay", "range:2:favored:-1,0.3,9");
  ASSERT_TRUE(released);
  EXPECT_EQ(released->range().phase(), modlock::wasm::ui::ACTION_PHASE_RELEASED);
  ASSERT_EQ(released->range().values_size(), 3);
  EXPECT_FLOAT_EQ(released->range().values(0), 0);
  EXPECT_FLOAT_EQ(released->range().values(1), 0.25F);
  EXPECT_FLOAT_EQ(released->range().values(2), 1);
  EXPECT_EQ(trees.Input(0, "delay", "range:3:max")->range().phase(),
            modlock::wasm::ui::ACTION_PHASE_CANCELLED);

  // A release out of order, short of a value, past the handles or with a
  // number that is not finite is refused, as are a stale handle, a press
  // with values and a slider's or a wheel's value.
  EXPECT_FALSE(trees.Input(0, "delay", "range:2:min:0.5,0.25,1"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:2:min:0,0.5"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:2:min:0,0.5,1,1"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:2:min:0,nan,1"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:2:min"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:1:mid"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:1:min:0,0.5,1"));
  EXPECT_FALSE(trees.Input(0, "delay", "range:4:min"));
  EXPECT_FALSE(trees.Input(0, "delay", "0.5"));
  EXPECT_FALSE(trees.Input(0, "delay", "wheel:1:0.5"));
  EXPECT_FALSE(trees.Input(0, "volume", "range:1:min"));

  // A range draws one to eight handles with distinct names.
  range->add_handles()->set_name("max");
  EXPECT_FALSE(trees.Apply(0, delay));
  range->mutable_handles(3)->set_name("a b");
  EXPECT_FALSE(trees.Apply(0, delay));
  range->clear_handles();
  EXPECT_FALSE(trees.Apply(0, delay));

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

// ShowLayout returns a change that shows the layout node id.
Change ShowLayout(std::string id) {
  auto change = Set(std::move(id), modlock::wasm::ui::KIND_LAYOUT);
  change.mutable_set(0)->set_layout("board.xml");
  return change;
}

// kStart is when a test's presses begin.
const auto kStart = std::chrono::steady_clock::time_point(std::chrono::hours(1));

TEST(UiTrees, TakesALayoutsPressFromThePlayerWhoseTreeShowsIt) {
  // Slot 3's tree shows the layout; slot 4's does not.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, ShowLayout("card")));
  ASSERT_TRUE(trees.Apply(3, Set("ready", modlock::wasm::ui::KIND_BUTTON)));
  ASSERT_TRUE(trees.Apply(4, Set("card", modlock::wasm::ui::KIND_BUTTON)));

  // The press carries the slot the engine named, whatever its line says, and
  // its payload's text: "eyJ2b3RlIjoxfQ" is {"vote":1}.
  auto press = trees.Action(3, "card vote eyJ2b3RlIjoxfQ", kStart);
  ASSERT_TRUE(press) << press.error();
  EXPECT_EQ(press->player(), 3);
  EXPECT_EQ(press->node(), "card");
  EXPECT_EQ(press->action(), "vote");
  EXPECT_EQ(press->payload(), R"({"vote":1})");
  auto bare = trees.Action(3, "card close", kStart);
  ASSERT_TRUE(bare) << bare.error();
  EXPECT_EQ(bare->player(), 3);
  EXPECT_EQ(bare->payload(), "");

  // Another player, a button, a missing layout or a bad name is refused.
  EXPECT_FALSE(trees.Action(4, "card vote eyJ2b3RlIjoxfQ", kStart));
  EXPECT_FALSE(trees.Action(3, "ready vote", kStart));
  EXPECT_FALSE(trees.Action(3, "shop vote", kStart));
  for (const char* line : {"card", "card ", "card a;quit", "card a/b", "card vote !!"}) {
    EXPECT_FALSE(trees.Action(3, line, kStart)) << line;
  }
}

TEST(UiTrees, TakesALayoutsPressesOnlyFromItsBindingsThatPress) {
  // The layout binds a pressing panel and a label.
  UiTrees trees;
  auto change = ShowLayout("card");
  auto* buy = change.mutable_set(0)->add_bindings();
  buy->set_panel("buy");
  buy->set_press(true);
  change.mutable_set(0)->add_bindings()->set_panel("price");
  ASSERT_TRUE(trees.Apply(3, change));

  // Only the pressing binding's press passes, with no payload.
  auto press = trees.Action(3, "card buy", kStart);
  ASSERT_TRUE(press) << press.error();
  EXPECT_EQ(press->action(), "buy");
  EXPECT_FALSE(trees.Action(3, "card price", kStart));
  EXPECT_FALSE(trees.Action(3, "card vote eyJ2b3RlIjoxfQ", kStart));

  // Once the mod stops binding the press, the panel's press is refused.
  change.mutable_set(0)->mutable_bindings(0)->set_press(false);
  ASSERT_TRUE(trees.Apply(3, change));
  EXPECT_FALSE(trees.Action(3, "card buy", kStart));
}

TEST(UiTrees, RefusesBindingsALayoutCannotDraw) {
  // A binding outside a layout, without a panel's name, or a second one of a
  // panel is refused, and the tree stays as it was.
  UiTrees trees;
  auto label = Set("card", modlock::wasm::ui::KIND_LABEL);
  label.mutable_set(0)->add_bindings()->set_panel("buy");
  EXPECT_FALSE(trees.Apply(3, label));
  for (const char* name : {"", "a b", "a;quit"}) {
    auto change = ShowLayout("card");
    change.mutable_set(0)->add_bindings()->set_panel(name);
    EXPECT_FALSE(trees.Apply(3, change)) << name;
  }
  auto twice = ShowLayout("card");
  twice.mutable_set(0)->add_bindings()->set_panel("buy");
  twice.mutable_set(0)->add_bindings()->set_panel("buy");
  EXPECT_FALSE(trees.Apply(3, twice));
  EXPECT_FALSE(trees.Action(3, "card buy", kStart));
}

TEST(UiTrees, RefusesALayoutsOversizedPayload) {
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, ShowLayout("card")));

  // 180 bytes of payload pass; 181 do not, nor does text that is not UTF-8.
  const auto encode = [](size_t bytes) {
    // "QUFB" is "AAA"; a tail of one or two bytes is "QQ" or "QUE".
    std::string encoded;
    for (size_t at = 0; at + 3 <= bytes; at += 3) {
      encoded += "QUFB";
    }
    encoded += bytes % 3 == 1 ? "QQ" : bytes % 3 == 2 ? "QUE" : "";
    return encoded;
  };
  auto most = trees.Action(3, "card keep " + encode(UiTrees::kActionPayload), kStart);
  ASSERT_TRUE(most) << most.error();
  EXPECT_EQ(most->payload(), std::string(UiTrees::kActionPayload, 'A'));
  EXPECT_FALSE(trees.Action(3, "card keep " + encode(UiTrees::kActionPayload + 1), kStart));
  EXPECT_FALSE(trees.Action(3, "card keep " + encode(3000), kStart));
  EXPECT_FALSE(trees.Action(3, "card keep _w", kStart));
}

TEST(UiTrees, KeepsEachPlayersLayoutPressesToAPace) {
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, ShowLayout("card")));
  ASSERT_TRUE(trees.Apply(4, ShowLayout("card")));

  // A burst passes and the next press at once is refused, for that player only.
  for (int i = 0; i < static_cast<int>(UiTrees::kActionBurst); ++i) {
    EXPECT_TRUE(trees.Action(3, "card watch", kStart)) << i;
  }
  EXPECT_FALSE(trees.Action(3, "card watch", kStart));
  EXPECT_TRUE(trees.Action(4, "card watch", kStart));

  // A quarter second later one more press passes.
  const auto later = kStart + std::chrono::milliseconds(250);
  EXPECT_TRUE(trees.Action(3, "card watch", later));
  EXPECT_FALSE(trees.Action(3, "card watch", later));
}

TEST(UiTrees, TakesNoLayoutPressOnceTheModStops) {
  // A stopping mod clears every tree, so its layouts' presses go nowhere.
  UiTrees trees;
  ASSERT_TRUE(trees.Apply(3, ShowLayout("card")));
  ASSERT_TRUE(trees.Action(3, "card close", kStart));
  EXPECT_EQ(trees.Clear(), std::vector<int32_t>{3});
  EXPECT_FALSE(trees.Action(3, "card close", kStart));

  // A player who leaves takes their layout's presses with them.
  ASSERT_TRUE(trees.Apply(5, ShowLayout("card")));
  EXPECT_TRUE(trees.Drop(5));
  EXPECT_FALSE(trees.Action(5, "card close", kStart));
}

// DocumentTree places an enabled form below the root and an ordinary short entry beside it.
Change DocumentTree() {
  auto change = Set("", modlock::wasm::ui::KIND_PANEL, true);
  change.mutable_set(0)->add_children("form");
  change.mutable_set(0)->add_children("name");
  auto* form = change.add_set();
  form->set_id("form");
  form->set_kind(modlock::wasm::ui::KIND_DOCUMENT_INPUT);
  form->mutable_document_input()->set_control(999);
  auto* name = change.add_set();
  name->set_id("name");
  name->set_kind(modlock::wasm::ui::KIND_INPUT);
  name->mutable_text_entry();
  return change;
}

// EncodeChunk encodes arbitrary bytes as the renderer's unpadded base64url.
std::string EncodeChunk(std::string_view text) {
  constexpr std::string_view digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string encoded;
  unsigned int bits = 0;
  int count = 0;
  for (const unsigned char byte : text) {
    bits = (bits << 8) | byte;
    count += 8;
    while (count >= 6) {
      count -= 6;
      encoded += digits[(bits >> count) & 63];
    }
  }
  if (count) {
    encoded += digits[(bits << (6 - count)) & 63];
  }
  return encoded;
}

TEST(UiTrees, DeliversOneWholeDocumentToItsCurrentPlayerAndMod) {
  UiTrees trees;
  UiTrees other;
  modlock::wasm::UiDocuments documents;
  std::optional<Change> stamped;
  ASSERT_TRUE(trees.Apply(3, DocumentTree(), &stamped));
  ASSERT_TRUE(trees.Apply(4, DocumentTree()));
  ASSERT_TRUE(other.Apply(3, DocumentTree()));
  ASSERT_TRUE(stamped);
  const auto control = trees.DocumentControl(3);
  ASSERT_NE(control, 0);
  EXPECT_NE(control, 999);
  EXPECT_EQ(stamped->set(1).document_input().control(), control);
  const auto prefix = std::to_string(control) + ":1:";
  auto submit = [&](std::string_view frame) {
    return trees.Document(3, "form", prefix + std::string(frame), documents, kStart);
  };
  std::string text;
  while (text.size() + 8 <= modlock::wasm::UiDocuments::kLimit) {
    text += "雪🙂\n";
  }
  ASSERT_EQ(text.size(), modlock::wasm::UiDocuments::kLimit);
  EXPECT_FALSE(submit("b:" + std::to_string(text.size())));
  EXPECT_FALSE(trees.Document(4, "form", prefix + "e", documents, kStart));
  EXPECT_FALSE(other.Document(3, "form", prefix + "e", documents, kStart));
  const auto other_prefix = std::to_string(other.DocumentControl(3)) + ":1:";
  EXPECT_FALSE(other.Document(3, "form", other_prefix + "b:0", documents, kStart));
  EXPECT_FALSE(other.Document(3, "form", other_prefix + "e", documents, kStart));
  const auto entry = trees.Input(3, "name", "QW5hw69z");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->text(), "Anaïs");
  EXPECT_FALSE(trees.Input(3, "form", "VGV4dA"));
  for (size_t at = 0; at < text.size(); at += 180) {
    EXPECT_FALSE(submit("p:" + std::to_string(at) + ":" +
                        EncodeChunk(std::string_view(text).substr(at, 180))));
  }
  const auto event = submit("e");
  ASSERT_TRUE(event);
  EXPECT_EQ(event->player(), 3);
  EXPECT_EQ(event->node(), "form");
  EXPECT_EQ(event->delivered().text(), text);
  EXPECT_FALSE(submit("e"));
  EXPECT_FALSE(submit("b:0"));
  EXPECT_FALSE(submit("e"));
}

TEST(UiTrees, ReportsEachFailedSubmissionOnceAndDropsReplacedDocuments) {
  UiTrees trees;
  modlock::wasm::UiDocuments documents;
  ASSERT_TRUE(trees.Apply(3, DocumentTree()));
  const auto control = trees.DocumentControl(3);
  auto input = [&](std::string_view frame, auto now) {
    return trees.Document(3, "form", std::to_string(control) + ":" + std::string(frame), documents,
                          now);
  };
  auto failure = [&](std::string_view frame) -> std::optional<modlock::wasm::ui::DocumentFailure> {
    const auto event = input(frame, kStart);
    if (!event || !event->has_failed()) {
      return std::nullopt;
    }
    EXPECT_EQ(event->player(), 3);
    EXPECT_EQ(event->node(), "form");
    return event->failed().reason();
  };
  EXPECT_EQ(failure("1:b:262145"), modlock::wasm::ui::DOCUMENT_FAILURE_TOO_LARGE);
  EXPECT_FALSE(input("1:e", kStart));
  EXPECT_FALSE(input("2:b:2", kStart));
  EXPECT_EQ(failure("2:p:1:YWk"), modlock::wasm::ui::DOCUMENT_FAILURE_MALFORMED);
  EXPECT_FALSE(input("2:e", kStart));
  EXPECT_FALSE(input("3:b:2", kStart));
  EXPECT_EQ(failure("3:e"), modlock::wasm::ui::DOCUMENT_FAILURE_MALFORMED);
  EXPECT_FALSE(input("4:b:1", kStart));
  EXPECT_FALSE(input("4:p:0:_w", kStart));
  EXPECT_EQ(failure("4:e"), modlock::wasm::ui::DOCUMENT_FAILURE_MALFORMED);
  EXPECT_FALSE(input("5:b:1", kStart));
  EXPECT_FALSE(input("5:p:0:YQ", kStart));
  EXPECT_EQ(failure("5:p:1:e"), modlock::wasm::ui::DOCUMENT_FAILURE_MALFORMED);
  EXPECT_FALSE(input("5:e", kStart));
  EXPECT_FALSE(input("6:b:0", kStart));
  EXPECT_EQ(failure("6:x"), modlock::wasm::ui::DOCUMENT_FAILURE_CANCELLED);
  EXPECT_FALSE(input("6:e", kStart));

  // A frame past the deadline ends the transfer as expired, once.
  EXPECT_FALSE(input("7:b:0", kStart));
  const auto later = kStart + std::chrono::seconds(21);
  const auto expired = input("7:e", later);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->failed().reason(), modlock::wasm::ui::DOCUMENT_FAILURE_EXPIRED);
  EXPECT_FALSE(input("7:e", later));
  EXPECT_TRUE(trees.ExpireDocuments(documents, later).empty());

  // A replaced form drops the old submission without an event.
  EXPECT_FALSE(input("8:b:0", later));
  ASSERT_TRUE(trees.Apply(3, DocumentTree()));
  EXPECT_NE(trees.DocumentControl(3), control);
  EXPECT_FALSE(input("8:e", later));
  documents.Cancel(3, control);
  const auto retry = std::to_string(trees.DocumentControl(3)) + ":9:";
  EXPECT_FALSE(trees.Document(3, "form", retry + "b:0", documents, later));
  EXPECT_TRUE(trees.Document(3, "form", retry + "e", documents, later));
  ASSERT_TRUE(trees.Drop(3));
  ASSERT_TRUE(trees.Apply(3, DocumentTree()));
  EXPECT_FALSE(trees.Document(3, "form", retry + "b:0", documents, later));
}

TEST(UiTrees, ScalesTheDocumentDeadlineAndTellsOnlyItsOwnMod) {
  UiTrees trees;
  UiTrees other;
  modlock::wasm::UiDocuments documents;
  ASSERT_TRUE(trees.Apply(3, DocumentTree()));
  ASSERT_TRUE(other.Apply(4, DocumentTree()));
  const auto mine = std::to_string(trees.DocumentControl(3)) + ":1:";
  const auto theirs = std::to_string(other.DocumentControl(4)) + ":1:";

  // A whole-size document gets 20 seconds and its sending time, about 18.2.
  EXPECT_FALSE(trees.Document(3, "form", mine + "b:262144", documents, kStart));
  EXPECT_FALSE(other.Document(4, "form", theirs + "b:0", documents, kStart));
  EXPECT_TRUE(trees.ExpireDocuments(documents, kStart + std::chrono::seconds(37)).empty());
  const auto past = kStart + std::chrono::seconds(39);
  const auto events = trees.ExpireDocuments(documents, past);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events[0].player(), 3);
  EXPECT_EQ(events[0].node(), "form");
  EXPECT_EQ(events[0].failed().reason(), modlock::wasm::ui::DOCUMENT_FAILURE_EXPIRED);
  EXPECT_TRUE(trees.ExpireDocuments(documents, past).empty());

  // The other mod's expired transfer waits for that mod's own expiry.
  const auto theirs_expired = other.ExpireDocuments(documents, past);
  ASSERT_EQ(theirs_expired.size(), 1);
  EXPECT_EQ(theirs_expired[0].player(), 4);
}

TEST(UiTrees, BoundsAndValidatesDocumentNodesWithoutWideningLabels) {
  UiTrees trees;
  auto change = DocumentTree();
  ASSERT_TRUE(trees.Apply(3, change));
  const auto control = trees.DocumentControl(3);
  change.mutable_set(0)->clear_children();
  EXPECT_FALSE(trees.Apply(3, change));
  EXPECT_EQ(trees.DocumentControl(3), control);
  auto label = Set("export", modlock::wasm::ui::KIND_LABEL);
  label.mutable_set(0)->set_text(std::string(2000, 'x'));
  EXPECT_FALSE(trees.Apply(3, label));
  auto output = Set("export", modlock::wasm::ui::KIND_DOCUMENT_OUTPUT);
  output.mutable_set(0)->set_document_text(std::string(modlock::wasm::UiDocuments::kLimit, 'x'));
  ASSERT_TRUE(trees.Apply(3, output));
  output.mutable_set(0)->mutable_document_text()->push_back('x');
  EXPECT_FALSE(trees.Apply(3, output));
}

// ShowList returns a reset change that shows a list node named "scores",
// whose rows show a name label and a portrait from the field "hero".
Change ShowList() {
  auto change = Set("scores", modlock::wasm::ui::KIND_LIST, true);
  auto* list = change.mutable_set(0)->mutable_list();
  list->set_row_height(32);
  auto* row = list->add_row();
  row->set_kind(modlock::wasm::ui::KIND_PANEL);
  row->add_children("name");
  row->add_children("portrait");
  auto* name = list->add_row();
  name->set_id("name");
  name->set_kind(modlock::wasm::ui::KIND_LABEL);
  name->set_text("{name}");
  auto* portrait = list->add_row();
  portrait->set_id("portrait");
  portrait->set_kind(modlock::wasm::ui::KIND_IMAGE);
  portrait->set_image("{hero}");
  return change;
}

// AddRows adds rows keyed "row<first>" onward to change's list "scores",
// each with a name field of size bytes.
void AddRows(Change& change, int first, int count, size_t size = 4) {
  auto* rows = change.add_lists();
  rows->set_node("scores");
  for (int i = first; i < first + count; ++i) {
    auto* row = rows->add_set();
    row->set_key("row" + std::to_string(i));
    row->set_rank(i);
    (*row->mutable_fields())["name"] = std::string(size, 'n');
  }
}

TEST(UiTrees, ChecksAListsTemplateAndRows) {
  // A list with a row template of panels, labels and images, one of them the
  // row's own panel, is drawn; its rows may name content images.
  UiTrees trees;
  auto shown = ShowList();
  AddRows(shown, 0, 2);
  (*shown.mutable_lists(0)->mutable_set(0)->mutable_fields())["hero"] =
      "file://{images}/heroes/haze.png";
  ASSERT_TRUE(trees.Apply(3, shown));

  // An image field from outside the game's content, a long key and too many
  // fields are refused, and so is a row change for a node that is no list.
  auto web = Change();
  AddRows(web, 5, 1);
  (*web.mutable_lists(0)->mutable_set(0)->mutable_fields())["hero"] = "https://example.com/x.png";
  EXPECT_FALSE(trees.Apply(3, web));
  auto key = Change();
  AddRows(key, 5, 1);
  key.mutable_lists(0)->mutable_set(0)->set_key(std::string(65, 'k'));
  EXPECT_FALSE(trees.Apply(3, key));
  auto fields = Change();
  AddRows(fields, 5, 1);
  for (int i = 0; i < 17; ++i) {
    (*fields.mutable_lists(0)->mutable_set(0)->mutable_fields())["f" + std::to_string(i)] = "x";
  }
  EXPECT_FALSE(trees.Apply(3, fields));
  auto other = Change();
  AddRows(other, 5, 1);
  other.mutable_lists(0)->set_node("missing");
  EXPECT_FALSE(trees.Apply(3, other));

  // A template without the row's own panel, with an input, with a child it
  // lacks or with rows carried in the node is refused, as is a height of 0.
  auto rootless = ShowList();
  rootless.mutable_set(0)->mutable_list()->mutable_row(0)->set_id("row");
  EXPECT_FALSE(trees.Apply(3, rootless));
  auto input = ShowList();
  input.mutable_set(0)->mutable_list()->mutable_row(1)->set_kind(modlock::wasm::ui::KIND_BUTTON);
  EXPECT_FALSE(trees.Apply(3, input));
  auto stray = ShowList();
  stray.mutable_set(0)->mutable_list()->mutable_row(0)->add_children("gone");
  EXPECT_FALSE(trees.Apply(3, stray));
  auto carried = ShowList();
  carried.mutable_set(0)->mutable_list()->add_rows()->set_key("row0");
  EXPECT_FALSE(trees.Apply(3, carried));
  auto flat = ShowList();
  flat.mutable_set(0)->mutable_list()->set_row_height(0);
  EXPECT_FALSE(trees.Apply(3, flat));
}

TEST(UiTrees, RefusesARowChangePastTheLimitsAndKeepsTheList) {
  // A list holds kMaxRows rows; one more is refused.
  UiTrees trees;
  auto full = ShowList();
  AddRows(full, 0, UiTrees::kMaxRows);
  ASSERT_TRUE(trees.Apply(3, full));
  auto more = Change();
  AddRows(more, UiTrees::kMaxRows, 1);
  EXPECT_FALSE(trees.Apply(3, more));

  // The refusal left the list as it was: dropping one row makes room for
  // exactly one, and a change to its node keeps its rows.
  auto swap = Change();
  AddRows(swap, UiTrees::kMaxRows, 1);
  swap.mutable_lists(0)->add_removed("row0");
  ASSERT_TRUE(trees.Apply(3, swap));
  auto renamed = ShowList();
  renamed.set_reset(false);
  ASSERT_TRUE(trees.Apply(3, renamed));
  EXPECT_FALSE(trees.Apply(3, more));

  // A reset drops the rows.
  ASSERT_TRUE(trees.Apply(3, ShowList()));
  ASSERT_TRUE(trees.Apply(3, more));

  // The lists' bytes are bounded together, and a refused change leaves the
  // bytes as they were, so a row of the same size still fits.
  UiTrees sized;
  const size_t row = 1000;
  const int fits = static_cast<int>(UiTrees::kMaxListBytes / (row + 64));
  auto big = ShowList();
  AddRows(big, 0, fits, row);
  ASSERT_TRUE(sized.Apply(3, big));
  auto past = Change();
  AddRows(past, fits, 64, row);
  EXPECT_FALSE(sized.Apply(3, past));
  auto replaced = Change();
  AddRows(replaced, 0, 1, row);
  EXPECT_TRUE(sized.Apply(3, replaced));
}

TEST(UiTrees, TakesAWindowOnlyForAListTheTreeShows) {
  // A window names a shown list, its first row and at most kMaxWindow rows.
  UiTrees trees;
  EXPECT_FALSE(trees.Window(3, "scores 0 50"));
  ASSERT_TRUE(trees.Apply(3, ShowList()));
  auto window = trees.Window(3, "scores 120 64");
  ASSERT_TRUE(window) << window.error();
  EXPECT_EQ(window->node(), "scores");
  EXPECT_EQ(window->first(), 120u);
  EXPECT_EQ(window->count(), 64u);
  EXPECT_FALSE(trees.Window(3, "scores 0 257"));
  EXPECT_FALSE(trees.Window(3, "scores -1 5"));
  EXPECT_FALSE(trees.Window(3, "scores 5"));
  EXPECT_FALSE(trees.Window(4, "scores 0 5"));
  ASSERT_TRUE(trees.Apply(3, Set("scores", modlock::wasm::ui::KIND_LABEL, true)));
  EXPECT_FALSE(trees.Window(3, "scores 0 5"));
}

TEST(UiTrees, TakesTheFocusAndChosenRowOfAList) {
  // A window carries the focused row's key, of 1 to 64 bytes, in base64url.
  UiTrees trees;
  auto shown = ShowList();
  AddRows(shown, 0, 2);
  ASSERT_TRUE(trees.Apply(3, shown));
  auto window = trees.Window(3, "scores 0 50 cm93MQ");
  ASSERT_TRUE(window) << window.error();
  EXPECT_EQ(window->focus(), "row1");
  EXPECT_EQ(trees.Window(3, "scores 0 50")->focus(), "");
  EXPECT_FALSE(trees.Window(3, "scores 0 50 !"));
  EXPECT_FALSE(trees.Window(
      3,
      "scores 0 50 "
      "a2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2tra2s"));

  // A chosen row must be one the list holds.
  auto chosen = trees.Input(3, "scores", "row:cm93MQ");
  ASSERT_TRUE(chosen) << chosen.error();
  EXPECT_EQ(chosen->row(), "row1");
  EXPECT_FALSE(trees.Input(3, "scores", "row:cm93OQ"));
  EXPECT_FALSE(trees.Input(3, "scores", "1"));

  // The focus is the player's, so a mod may not set it, and a selected key
  // is bounded as a row's is.
  auto focus = ShowList();
  focus.mutable_set(0)->mutable_list()->set_focus("row1");
  EXPECT_FALSE(trees.Apply(3, focus));
  auto at = ShowList();
  at.mutable_set(0)->mutable_list()->set_focus_at(1);
  EXPECT_FALSE(trees.Apply(3, at));
  auto selected = ShowList();
  selected.mutable_set(0)->mutable_list()->set_selected("row1");
  EXPECT_TRUE(trees.Apply(3, selected));
  selected.mutable_set(0)->mutable_list()->set_selected(std::string(65, 'k'));
  EXPECT_FALSE(trees.Apply(3, selected));
}

}  // namespace
