#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/export.h"
#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

// VirtualSlot is one virtual function modlock calls or hooks through its
// class's dispatch table, recorded in the game data file. A game update that
// adds or removes a virtual ahead of it moves the function, and a call through
// a stale index reaches the wrong function without any error, so callers take
// the index from ResolveVirtualSlot rather than the recorded one.
struct VirtualSlot {
  // id is the stable name callers resolve, such as "engine-server.globals".
  std::string_view id;
  // module is the file that defines the class, such as "server.dll".
  std::string_view module;
  // class_name is the implementing class's RTTI name, such as "CBaseEntity".
  std::string_view class_name;
  // index is the function's position in the class's primary table on the
  // game build the pattern was taken from.
  size_t index = 0;
  // pattern matches the function's entry bytes in the ParseSignature syntax,
  // and only this function's among the table's entries.
  std::string_view pattern;
  // shape names the function.
  std::string_view shape;
};

// GameVirtualSlots returns every recorded virtual slot, sorted by id.
// LoadGameSignatures replaces them with the slots of the file it loads.
[[nodiscard]] MODLOCK_API std::span<const VirtualSlot> GameVirtualSlots();

// FindVirtualSlot returns the recorded virtual slot with id, or nullptr.
[[nodiscard]] MODLOCK_API const VirtualSlot* FindVirtualSlot(std::string_view id);

// FindVtable returns the image offset of class_name's primary dispatch table,
// found through its MSVC RTTI: the type descriptor named ".?AV<class_name>@@",
// the complete object locator that places it at offset zero, and the table
// that follows the pointer to that locator.
[[nodiscard]] MODLOCK_API std::expected<size_t, std::string> FindVtable(
    const ModuleImage& image, std::string_view class_name);

// ObjectClassName returns the RTTI class name of the live polymorphic object
// at object, such as CCitadel_Modifier_Knockdown, read through its dispatch
// table's object locator with ReadNative, so a stale or foreign pointer
// answers empty instead of faulting. It answers empty off Windows.
[[nodiscard]] MODLOCK_API std::string ObjectClassName(const void* object);

// VtableEntries returns the image offset of each function in the table at
// image offset table, ending at the first entry outside executable code.
[[nodiscard]] MODLOCK_API std::vector<size_t> VtableEntries(const ModuleImage& image, size_t table);

// MatchingSlots returns every index of the table at image offset table whose
// function's entry bytes match slot's pattern. A sound slot yields exactly one
// index, its recorded one until a game update moves it.
[[nodiscard]] MODLOCK_API std::expected<std::vector<size_t>, std::string> MatchingSlots(
    const ModuleImage& image, size_t table, const VirtualSlot& slot);

// ResolveVirtualSlot returns the index of id's function in its class's table
// in the running game: the one entry whose function matches the slot's
// pattern. A slot whose function moved resolves to its new index and logs it
// once; zero or several matches, or an unknown id, is an error, so the feature
// that needs the slot stays off rather than calling the wrong function.
// Results are kept for the life of the process. Without mapped game modules,
// off Windows, it returns the recorded index.
[[nodiscard]] MODLOCK_API std::expected<size_t, std::string> ResolveVirtualSlot(
    std::string_view id);

}  // namespace modlock::gameinterop
