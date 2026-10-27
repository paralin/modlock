#include "wasm/console_lines.h"

#include <gtest/gtest.h>

namespace modlock::wasm {
namespace {

TEST(ConsoleLines, ModsChangeTheGameButNotWhatSurroundsIt) {
  // A line splits at every newline and semicolon, quoted or not, and a name
  // ends at the tokenizer's break characters.
  const auto commands = SplitConsoleLine(" sv_cheats 1 ;\"say\" a;b\nexec:x ");
  ASSERT_EQ(commands.size(), 4u);
  EXPECT_EQ(commands[0].name, "sv_cheats");
  EXPECT_EQ(commands[0].arguments, "1");
  EXPECT_EQ(commands[1].name, "say");
  EXPECT_EQ(commands[1].arguments, "a");
  EXPECT_EQ(commands[2].name, "b");
  EXPECT_EQ(commands[3].name, "exec");
  EXPECT_EQ(commands[3].arguments, ":x");

  // The game's own rules, map, players and recording stay open to a mod.
  for (const auto* line :
       {"sv_cheats 1", "citadel_bot_brain_disable_movement 1", "changelevel dl_midtown",
        "kick \"Bot 1\"", "quit", "tv_enable 1", "tv_delay 0", "tv_record cursed-20261027-1530",
        "tv_record match.dem", "host_timescale 0.4", "ai_disabled 1"}) {
    EXPECT_EQ(RefuseConsoleLine(line), "") << line;
  }

  // Files, logging, network, the process and stored commands are refused by
  // name, in any case, quoted, or chained after an allowed command.
  EXPECT_EQ(RefuseConsoleLine("exec server.cfg"),
            "exec is an engine file control, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("sv_cheats 1; EXEC_async x"),
            "exec_async is an engine file control, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("\"con_logfile\" out.txt"),
            "con_logfile is an engine logging control, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("say \"hi;tv_broadcast_url x\""),
            "tv_broadcast_url is an engine network control, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("echo\ncrash"),
            "crash is an engine process control, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("alias go exec x"),
            "alias is a command that stores other commands, which a mod may not run");
  EXPECT_EQ(RefuseConsoleLine("log:on"),
            "log is an engine logging control, which a mod may not run");

  // A recording keeps to the recording directory.
  for (const auto* line : {"tv_record ../escape", "tv_record a/b", "tv_record", "tv_record a b"}) {
    EXPECT_EQ(RefuseConsoleLine(line),
              "tv_record takes a bare file name of letters, digits, - and _")
        << line;
  }
}

}  // namespace
}  // namespace modlock::wasm
