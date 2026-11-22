#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>

#include "modlock/export.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/native_damage.h"
#include "modlock/gameinterop/world_entities.h"

namespace modlock::gameinterop {

// VacatedSeats clears what an ended seat leaves in the world. The game keeps
// a departed human's controller on its team, so the scoreboard lists them, and
// their hero standing, which the game revives on its own timer. Once a seat
// ends (ConnectionEventSink::OnSeatEnded), its hero is killed and kept from
// respawning, and its controller is moved to team 0, unless a new seat has
// taken the slot by then. A seat that is only away keeps both, since its
// player returns to them. Everything runs on the engine thread.
class MODLOCK_API VacatedSeats {
 public:
  // kDelay is how long after a seat ends its slot is cleared: the engine
  // finishes the disconnect first, and a bot's controller and hero are gone
  // by then.
  static constexpr std::chrono::milliseconds kDelay{1000};

  // Vacate notes that slot's seat ended at now.
  void Vacate(int32_t slot, std::chrono::steady_clock::time_point now);

  // Clear clears each slot vacated kDelay before now whose seat no new one has
  // replaced. Call it from the engine frame.
  void Clear(std::chrono::steady_clock::time_point now);

  // Blocked reports whether pawn is a cleared seat's hero in a slot no new
  // seat has taken, whose respawn the game must not run.
  bool Blocked(uint32_t pawn) const;

  // Holding reports whether some cleared hero's respawn is blocked.
  bool Holding() const { return !laid_.empty(); }

 private:
  // Vacancy is one ended seat: its slot's seat generation when it ended.
  struct Vacancy {
    uint64_t seat = 0;
    std::chrono::steady_clock::time_point due;
  };

  // Laid is a cleared seat's hero: its slot and seat generation.
  struct Laid {
    int32_t slot = 0;
    uint64_t seat = 0;
  };

  // ClearSlot kills slot's hero and moves its controller to team 0, and
  // returns the hero's handle, or nullopt when the controller is gone.
  std::optional<uint32_t> ClearSlot(int32_t slot);

  // Resolve finds the game calls ClearSlot needs, once.
  std::expected<void, std::string> Resolve();

  std::map<int32_t, Vacancy> vacancies_;
  std::map<uint32_t, Laid> laid_;
  std::optional<MappedModuleImage> image_;
  std::optional<NativeDamage> damage_;
  std::optional<WorldEntities::Calls> calls_;
  size_t hero_offset_ = 0;
  size_t health_offset_ = 0;
};

}  // namespace modlock::gameinterop
