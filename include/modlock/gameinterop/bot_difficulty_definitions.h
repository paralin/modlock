#pragma once

#include <expected>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// BotDifficultyCalls are the server's own lookup of the parsed
// scripts/bots/bot_difficulty.vdata records, which the bot constructor makes.
struct MODLOCK_API BotDifficultyCalls {
  // records returns the process's map of difficulty records.
  void* (*records)() = nullptr;
  // find returns the CBotDifficultyDefinition for an ECitadelBotDifficulty
  // value, or null when the file has no record for it.
  void* (*find)(void* records, int difficulty) = nullptr;
};

// BotDifficultyDefinitions finds bot difficulty records on the engine thread.
// Each bot keeps a pointer to its record, so a change to a record reaches
// every bot of that difficulty, including bots already playing.
class MODLOCK_API BotDifficultyDefinitions {
 public:
  static std::expected<BotDifficultyDefinitions, std::string> Resolve(const ModuleImage& server);
  BotDifficultyDefinitions() = default;
  explicit BotDifficultyDefinitions(BotDifficultyCalls calls);

  // Find returns the record for a difficulty, or null when the file has none
  // for it. It fails while the file is not loaded, which it detects from the
  // easy record, since the game falls back to that one for every bot.
  [[nodiscard]] std::expected<void*, std::string> Find(int difficulty) const;

 private:
  BotDifficultyCalls calls_;
};

}  // namespace modlock::gameinterop
