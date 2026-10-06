#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "modlock/export.h"

namespace modlock::gameinterop {

// ConsoleVariableInfo describes one registered console variable. Values are
// formatted as the console prints them; min and max are empty when unbounded.
struct ConsoleVariableInfo {
  std::string name;
  std::string help;
  uint64_t flags = 0;
  // type is the EConVarType: 0 Boolean through 15 world-space vector.
  int16_t type = 0;
  std::string default_value;
  std::string min;
  std::string max;
};

// ConsoleCommandInfo describes one registered console command.
struct ConsoleCommandInfo {
  std::string name;
  std::string help;
  uint64_t flags = 0;
};

// ConsoleVariables borrows the mapped VEngineCvar007 instance. All operations
// run on the engine thread; discard this view before tier0.dll unloads.
// The engine console parser remains responsible for setting values.
class MODLOCK_API ConsoleVariables {
 public:
  static std::expected<ConsoleVariables, std::string> Resolve();
  static std::expected<ConsoleVariables, std::string> Bind(void* instance);

  // Expose removes only development, hidden, and defensive flags from the
  // named variable. Other variables and flags remain unchanged.
  [[nodiscard]] std::expected<void, std::string> Expose(const char* name) const;
  // AllowServerChanges exposes one named variable and removes its cheat flag.
  // Use only for settings controlled by the host plugin; this does not enable
  // sv_cheats or change any value. Replication and command-source flags remain.
  [[nodiscard]] std::expected<void, std::string> AllowServerChanges(const char* name) const;
  // ReadBool copies the current slot-zero value, refusing non-Boolean variables.
  [[nodiscard]] std::expected<bool, std::string> ReadBool(const char* name) const;
  // ReadFloat copies the current slot-zero value, refusing non-float variables.
  [[nodiscard]] std::expected<float, std::string> ReadFloat(const char* name) const;
  // SetBool, SetFloat and SetInt32 write the slot-zero value of a Boolean, 32-bit float or
  // 32-bit integer variable, refusing other types. No change callback runs.
  [[nodiscard]] std::expected<void, std::string> SetBool(const char* name, bool value) const;
  [[nodiscard]] std::expected<void, std::string> SetFloat(const char* name, float value) const;
  [[nodiscard]] std::expected<void, std::string> SetInt32(const char* name, int32_t value) const;
  // Variables and Commands list the registry in registration order, including
  // development-only entries that the console's own listing hides.
  [[nodiscard]] std::expected<std::vector<ConsoleVariableInfo>, std::string> Variables() const;
  [[nodiscard]] std::expected<std::vector<ConsoleCommandInfo>, std::string> Commands() const;

 private:
  [[nodiscard]] std::expected<std::byte*, std::string> Find(const char* name) const;
  // Value returns the slot-zero value of a variable of the given type.
  [[nodiscard]] std::expected<std::byte*, std::string> Value(const char* name, int16_t type,
                                                             const char* kind) const;
  [[nodiscard]] std::expected<void, std::string> ClearFlags(const char* name, uint64_t flags) const;
  void* instance_ = nullptr;
};

}  // namespace modlock::gameinterop
