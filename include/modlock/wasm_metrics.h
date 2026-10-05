#pragma once

#include <cstdint>
#include <string_view>

#include "modlock/export.h"
#include "proto/modlock/wasm.pb.h"

namespace modlock {

// WasmMetrics receives the metric totals players reach in mods. A WasmHost
// hands on one player's totals for a mod once, when the player leaves or the
// mod stops. Its method runs on the engine thread, so it must not wait.
class MODLOCK_API WasmMetrics {
 public:
  virtual ~WasmMetrics() = default;

  // Keep receives the player's totals for the mod's session.
  virtual void Keep(std::string_view mod, uint64_t steam_id, const wasm::MetricTotals& totals) = 0;
};

}  // namespace modlock
