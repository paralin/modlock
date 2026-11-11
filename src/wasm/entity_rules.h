#pragma once

#include <string>
#include <string_view>

namespace modlock::wasm {

// RefuseEntityClass returns why a mod may not create, or send inputs to, an
// entity of designer_name, or an empty string when it may. The refused
// classes run console lines, on the server or on players' clients, or load
// entities and scripts that no check of the mod's calls sees.
std::string RefuseEntityClass(std::string_view designer_name);

// RefuseKeyValue returns why a mod may not give an entity the key value key,
// or an empty string when it may. The refused keys name console lines or
// scripts the entity runs.
std::string RefuseKeyValue(std::string_view key);

// RefuseField returns why a mod may not write the schema field field, or an
// empty string when it may. The refused fields hold what the refused key
// values set.
std::string RefuseField(std::string_view field);

// RefuseInput returns why a mod may not send input to any entity, or an
// empty string when it may. AddOutput rewires an entity's outputs and sets
// its key values, and the script inputs run script code.
std::string RefuseInput(std::string_view input);

}  // namespace modlock::wasm
