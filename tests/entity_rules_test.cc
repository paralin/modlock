#include "wasm/entity_rules.h"

#include <gtest/gtest.h>

namespace modlock::wasm {
namespace {

TEST(EntityRules, ModsMayNotReachTheConsoleThroughEntities) {
  // The command points, the commentary node, scripts and prefabs are refused
  // in any case; the game's other entities stay open.
  for (const auto* name :
       {"point_servercommand", "Point_ClientCommand", "point_broadcastclientcommand",
        "point_commentary_node", "logic_script", "point_prefab"}) {
    EXPECT_NE(RefuseEntityClass(name), "") << name;
  }
  EXPECT_EQ(RefuseEntityClass("point_servercommand"),
            "point_servercommand runs console lines or loads entities past a mod's checks, which "
            "a mod may not use");
  for (const auto* name : {"logic_relay", "npc_trooper", "point_worldtext", "prop_dynamic", ""}) {
    EXPECT_EQ(RefuseEntityClass(name), "") << name;
  }

  // Key values that name console lines or scripts are refused.
  for (const auto* key :
       {"vscripts", "VScripts", "precommands", "postcommands", "spawn_script_func"}) {
    EXPECT_NE(RefuseKeyValue(key), "") << key;
  }
  EXPECT_EQ(RefuseKeyValue("precommands"),
            "key value precommands names a console line or a script, which a mod may not set");
  for (const auto* key : {"targetname", "model", "message", "script"}) {
    EXPECT_EQ(RefuseKeyValue(key), "") << key;
  }

  // The schema fields those key values fill are refused too.
  for (const auto* field : {"m_iszPrivateVScripts", "m_iszPreCommands", "m_iszPostCommands"}) {
    EXPECT_NE(RefuseField(field), "") << field;
  }
  EXPECT_EQ(RefuseField("m_iszPrivateVScripts"),
            "field m_iszPrivateVScripts names a console line or a script, which a mod may not "
            "write");
  for (const auto* field : {"m_iszDialogXMLName", "m_iszNextScript", "m_flSimulationTime"}) {
    EXPECT_EQ(RefuseField(field), "") << field;
  }

  // AddOutput and the script inputs are refused on every entity.
  for (const auto* input : {"AddOutput", "addoutput", "RunScriptCode", "CallScriptFunction"}) {
    EXPECT_NE(RefuseInput(input), "") << input;
  }
  EXPECT_EQ(RefuseInput("AddOutput"),
            "input addoutput rewires an entity or runs a script, which a mod may not send");
  for (const auto* input : {"Kill", "SetAbsScale", "Trigger", "Command"}) {
    EXPECT_EQ(RefuseInput(input), "") << input;
  }
}

}  // namespace
}  // namespace modlock::wasm
