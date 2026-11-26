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

}  // namespace
