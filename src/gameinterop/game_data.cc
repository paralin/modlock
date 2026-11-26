#include "modlock/gameinterop/game_data.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/virtual_slots.h"

namespace modlock::gameinterop {

#if defined(_WIN32)
namespace {

// ReportOnce logs each gap the first time any call finds it, so each one
// costs one line for the life of the process.
void ReportOnce(std::string_view kind, const std::vector<GameDataGap>& gaps) {
  static std::mutex mutex;
  static std::set<std::string, std::less<>> reported;
  const std::lock_guard lock(mutex);
  for (const auto& gap : gaps) {
    if (reported.insert(std::string(kind) + ' ' + gap.id).second) {
      std::fprintf(stderr, "[modlock] game data unresolved: %s\n", gap.error.c_str());
    }
  }
}

}  // namespace
#endif

GameDataGaps ResolveGameData() {
  GameDataGaps gaps;
#if defined(_WIN32)
  // Resolve each loaded module's signatures.
  constexpr std::array kModules{
      std::pair{GameModule::kServer, L"server.dll"},
      std::pair{GameModule::kEngine, L"engine2.dll"},
  };
  for (const auto& [module, file] : kModules) {
    if (const auto image = MappedModuleImage::ForModule(file)) {
      auto missing = ResolveModuleSignatures(*image, module);
      gaps.signatures.insert(gaps.signatures.end(), std::make_move_iterator(missing.begin()),
                             std::make_move_iterator(missing.end()));
    }
  }

  // Keep the signatures in id order across modules; a signature that two
  // modules carry is named once.
  std::ranges::sort(gaps.signatures, {}, &GameDataGap::id);
  const auto repeated = std::ranges::unique(gaps.signatures, {}, &GameDataGap::id);
  gaps.signatures.erase(repeated.begin(), repeated.end());

  // Resolve each virtual slot whose module is loaded. The engine loads some
  // of those modules as it starts, after the host maps its own.
  std::map<std::string_view, bool, std::less<>> loaded;
  for (const auto& slot : GameVirtualSlots()) {
    auto [found, added] = loaded.try_emplace(slot.module);
    if (added) {
      const std::wstring module(slot.module.begin(), slot.module.end());
      found->second = MappedModuleImage::ForModule(module).has_value();
    }
    if (!found->second) {
      continue;
    }
    if (auto index = ResolveVirtualSlot(slot.id); !index) {
      gaps.slots.push_back({.id = std::string(slot.id), .error = std::move(index.error())});
    }
  }
  std::ranges::sort(gaps.slots, {}, &GameDataGap::id);

  ReportOnce("signature", gaps.signatures);
  ReportOnce("slot", gaps.slots);
#endif
  return gaps;
}

}  // namespace modlock::gameinterop
