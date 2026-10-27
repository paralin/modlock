//go:build deps_only

#include <google/protobuf/text_format.h>

#include <memory>
#include <string_view>

#include "modlock/gameinterop/ability_definitions.h"
#include "modlock/gameinterop/keyvalues.h"
#include "src/gamedata/hero_builds.pb.h"
#include "wasm/game.h"

namespace modlock::wasm {

// kBuiltInHeroBuilds is data/hero_builds.txtpb as of the build.
extern const std::string_view kBuiltInHeroBuilds;

namespace {

// Builds parses the embedded hero builds once. A file that fails to parse
// leaves no builds, so every hero answers with none.
const gamedata::HeroBuildFile& Builds() {
  static const gamedata::HeroBuildFile file = [] {
    gamedata::HeroBuildFile parsed;
    if (!google::protobuf::TextFormat::ParseFromString(std::string(kBuiltInHeroBuilds), &parsed)) {
      parsed.Clear();
    }
    return parsed;
  }();
  return file;
}

}  // namespace

std::expected<abilities::HeroBuildResponse, std::string> Game::HeroBuild(
    const abilities::HeroBuildRequest& request) {
  // Find the hero's build; a hero without one has no categories.
  abilities::HeroBuildResponse response;
  const gamedata::HeroBuild* build = nullptr;
  for (const auto& hero : Builds().heroes()) {
    if (hero.hero_class() == request.hero()) {
      build = &hero;
      break;
    }
  }
  if (build == nullptr) {
    return response;
  }

  // Keep the items the game defines and has not disabled, and the categories
  // that keep any.
  auto definitions = services_.Abilities();
  if (!definitions) {
    return std::unexpected(definitions.error());
  }
  for (const auto& category : build->categories()) {
    abilities::BuildCategory kept;
    kept.set_name(category.name());
    kept.set_description(category.description());
    for (const auto& item : category.items()) {
      const auto definition =
          (*definitions)->Find(gameinterop::MakeMemberName(item.class_name()).hash);
      if (!definition || definition->disabled) {
        continue;
      }
      auto* out = kept.add_items();
      out->set_item(item.class_name());
      out->set_annotation(item.annotation());
    }
    if (kept.items_size() > 0) {
      *response.add_categories() = std::move(kept);
    }
  }
  return response;
}

}  // namespace modlock::wasm
