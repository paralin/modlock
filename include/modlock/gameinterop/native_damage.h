#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// NativeDamage borrows the mapped server module and constructs native damage
// descriptors through its constructor and destructor. Calls run on the engine
// thread; the caller retains connection authority and verifies the resulting death.
class MODLOCK_API NativeDamage {
 public:
  static std::expected<NativeDamage, std::string> Resolve(const ModuleImage& server,
                                                          void* schema_system);
  // Kill requests a native death without awarding gold or kill credit. The pawn
  // is borrowed only for this call and may be destroyed by the native operation.
  std::expected<void, std::string> Kill(void* pawn) const;
  // Hazard applies fixed environmental health damage without mitigation,
  // rewards or item procs. The pawn may die during this synchronous call.
  std::expected<void, std::string> Hazard(void* pawn, float amount) const;
  // Hit applies ordinary attributed damage through the engine. All entities are
  // borrowed live pointers, and ability may be null for a hit without one; the
  // victim may die during this synchronous call.
  // hit_group retains an attributed trace group; -1 leaves the native default.
  std::expected<void, std::string> Hit(void* victim, void* inflictor, void* attacker, void* ability,
                                       float amount, int32_t hit_group = -1) const;

 private:
  std::expected<void, std::string> Apply(void* victim, void* inflictor, void* attacker,
                                         void* ability, float amount, uint64_t flags,
                                         int32_t hit_group = -1) const;
  void* (*construct_)(void*, void*, void*, void*, float, int32_t, int32_t) = nullptr;
  void (*destroy_)(void*) = nullptr;
  void (*damage_)(void*, void*, void*) = nullptr;
  size_t size_ = 0;
  size_t flags_offset_ = 0;
  size_t health_offset_ = 0;
  size_t ability_offset_ = 0;
  size_t hit_group_offset_ = 0;
  uint64_t hazard_flags_ = 0;
};

}  // namespace modlock::gameinterop
