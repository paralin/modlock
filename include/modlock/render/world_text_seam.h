#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "modlock/export.h"
#include "proto/modlock/types.pb.h"

namespace modlock::render {

// WorldTextStyle carries the presentation values a factory applies to one
// world-text entity.
struct WorldTextStyle {
  // font_size maps to the point_worldtext font_size keyvalue.
  float font_size = 100.0f;
  // color_abgr is the entity color as 0xAABBGGRR.
  std::uint32_t color_abgr = 0xFFFFFFFFu;
  // face_camera rotates around the text's up axis; false uses its exact angles.
  bool face_camera = true;
  // scale multiplies the text's world size without adding pixels, so large
  // signs read from afar without outgrowing the entity's fixed panel.
  float scale = 1.0f;
};

// WorldTextEntity is one owned point_worldtext handle. Remove ends its native
// lifetime; InvalidateAfterEngineReset forgets an already-destroyed entity.
class MODLOCK_API WorldTextEntity {
 public:
  virtual ~WorldTextEntity() = default;

  // SetMessage replaces the rendered text.
  virtual void SetMessage(std::string_view message) = 0;

  // SetOrigin moves the entity to world-space position with Euler degrees in
  // the game's pitch/yaw/roll order.
  virtual void SetOrigin(const modlock::Vec3& position, const modlock::EulerAngles& angles) = 0;

  // Handle returns the entity's packed handle while it is live. SetMessage
  // may replace the entity, so callers read it again after each change.
  virtual std::optional<std::uint32_t> Handle() const = 0;

  // Remove schedules destruction of the backing entity; the handle must not
  // be used afterwards.
  virtual void Remove() = 0;

  // InvalidateAfterEngineReset clears the backing handle without calling the
  // engine. Use only after the engine has already destroyed its entities.
  virtual void InvalidateAfterEngineReset() {}
};

// WorldTextEntityFactory is the single surface between render code and the
// game's point_worldtext creation path. Implementations own the game-memory
// mechanics behind Create; nothing else in render speaks raw game ABI.
class MODLOCK_API WorldTextEntityFactory {
 public:
  virtual ~WorldTextEntityFactory() = default;

  // Create spawns one enabled, fullbright world-text entity at origin and
  // returns an owned handle, or an error naming the failed game step.
  virtual std::expected<std::unique_ptr<WorldTextEntity>, std::string> Create(
      std::string_view message, const modlock::Vec3& origin, const modlock::EulerAngles& angles,
      const WorldTextStyle& style) = 0;
};

}  // namespace modlock::render
