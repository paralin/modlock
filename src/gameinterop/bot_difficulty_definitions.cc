#include "modlock/gameinterop/bot_difficulty_definitions.h"

namespace modlock::gameinterop {
namespace {

// kEasy is k_ECitadelBotDifficulty_Easy, the record the game gives a bot whose
// own difficulty has none.
constexpr int kEasy = 1;

}  // namespace

std::expected<BotDifficultyDefinitions, std::string> BotDifficultyDefinitions::Resolve(
    const ModuleImage& server) {
  BotDifficultyCalls calls;
  const SignatureCall signatures[] = {
      {"bot-difficulty.records", &calls.records},
      {"bot-difficulty.find", &calls.find},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  return BotDifficultyDefinitions(calls);
}

BotDifficultyDefinitions::BotDifficultyDefinitions(BotDifficultyCalls calls) : calls_(calls) {}

std::expected<void*, std::string> BotDifficultyDefinitions::Find(int difficulty) const {
  // A default-constructed owner finds nothing.
  if (!calls_.records || !calls_.find) {
    return std::unexpected("bot difficulty calls are unresolved");
  }

  // Look the difficulty up, and tell a missing record from unloaded data.
  void* records = calls_.records();
  if (void* record = calls_.find(records, difficulty)) {
    return record;
  }
  if (!calls_.find(records, kEasy)) {
    return std::unexpected("bot difficulties are not loaded yet");
  }
  return nullptr;
}

}  // namespace modlock::gameinterop
