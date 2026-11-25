#pragma once

#include <expected>
#include <string>

#include "src/wasm/entities/entities.pb.h"
#include "src/wasm/fields/fields.pb.h"
#include "wasm/host_service.gen.h"
#include "wasm/module.h"
#include "wasm/world.h"

namespace modlock::wasm {

class GameServices;

// Fields answers a mod's reads and writes of live entities' schema fields and
// its questions about an entity's class, name and place. A write to a client
// UI entity's layout rebuilds every client's panels at the next frame.
class Fields final : public Module, public FieldsCalls {
 public:
  // services and world outlive the fields.
  Fields(GameServices& services, const World& world) : services_(services), world_(world) {}

  Fields(const Fields&) = delete;
  Fields& operator=(const Fields&) = delete;

  std::expected<fields::FieldResponse, std::string> ReadField(
      const fields::ReadFieldRequest& request) override;
  std::expected<void, std::string> WriteField(const fields::WriteFieldRequest& request) override;
  std::expected<fields::FieldListResponse, std::string> ReadFieldList(
      const fields::ReadFieldRequest& request) override;
  std::expected<void, std::string> WriteFieldList(
      const fields::WriteFieldListRequest& request) override;
  std::expected<entities::EntityClassResponse, std::string> EntityClass(
      const entities::EntityRequest& request) override;
  std::expected<entities::EntityNameResponse, std::string> EntityName(
      const entities::EntityRequest& request) override;
  std::expected<entities::EntityPositionResponse, std::string> EntityPosition(
      const entities::EntityRequest& request) override;

  // Frame sends every client a full update when a layout changed since the
  // last frame, so each builds its client UI panels from their current
  // layouts.
  void Frame() override;

 private:
  GameServices& services_;
  // world_ gives the mod's content addon for a text field's {addon}.
  const World& world_;
  // rebuild_panels_ asks every client for a full update at the next frame,
  // after the mod wrote a client UI entity's layout.
  bool rebuild_panels_ = false;
};

}  // namespace modlock::wasm
