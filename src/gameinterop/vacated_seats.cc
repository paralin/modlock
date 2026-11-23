#include "modlock/gameinterop/vacated_seats.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/entity_abi.h"
#include "pawn_observer_internal.h"

namespace modlock::gameinterop {

void VacatedSeats::Vacate(int32_t slot, std::chrono::steady_clock::time_point now) {
  if (!IsPlayerSlot(slot)) {
    return;
  }
  vacancies_[slot] = {.seat = ConnectionTracker::SeatGeneration(slot), .due = now + kDelay};
}

void VacatedSeats::Clear(std::chrono::steady_clock::time_point now) {
  // A hero stays blocked only while no new seat has taken its slot.
  std::erase_if(killed_, [](const auto& entry) {
    return ConnectionTracker::SeatGeneration(entry.second.slot) != entry.second.seat;
  });

  for (auto it = vacancies_.begin(); it != vacancies_.end();) {
    // A new seat in the slot owns its controller and hero.
    const auto [slot, vacancy] = *it;
    if (ConnectionTracker::SeatGeneration(slot) != vacancy.seat) {
      it = vacancies_.erase(it);
      continue;
    }

    // Wait out the delay and the engine's disconnect.
    if (now < vacancy.due || ConnectionTracker::StateForSlot(slot).occupied) {
      ++it;
      continue;
    }
    it = vacancies_.erase(it);
    if (const auto hero = ClearSlot(slot); hero && *hero != kInvalidEHandleIndex) {
      killed_[*hero] = {.slot = slot, .seat = vacancy.seat};
    }
  }
}

bool VacatedSeats::Blocked(uint32_t pawn) const {
  const auto found = killed_.find(pawn);
  return found != killed_.end() &&
         ConnectionTracker::SeatGeneration(found->second.slot) == found->second.seat;
}

std::expected<void, std::string> VacatedSeats::Resolve() {
  // Each call resolves once, from the server module and its schema.
  if (damage_ && calls_) {
    return {};
  }
  if (!image_) {
    auto image = MappedModuleImage::ForModule(L"server.dll");
    if (!image) {
      return std::unexpected(image.error());
    }
    image_ = std::move(*image);
  }
  auto schema = ResolveSchemaSystem();
  if (!schema) {
    return std::unexpected(schema.error());
  }
  const SchemaOffset fields[] = {
      {"CCitadelPlayerController", "m_hHeroPawn", sizeof(uint32_t), &hero_offset_},
      {"CBaseEntity", "m_iHealth", sizeof(int32_t), &health_offset_},
  };
  if (auto resolved = ResolveSchemaOffsets(*schema, "server.dll", fields); !resolved) {
    return std::unexpected(resolved.error());
  }
  auto damage = NativeDamage::Resolve(*image_, *schema);
  if (!damage) {
    return std::unexpected(damage.error());
  }
  auto calls = WorldEntities::ResolveCalls(*image_, *schema);
  if (!calls) {
    return std::unexpected(calls.error());
  }
  damage_ = std::move(*damage);
  calls_ = std::move(*calls);
  return {};
}

std::optional<uint32_t> VacatedSeats::ClearSlot(int32_t slot) {
  // Find the slot's controller at index slot + 1.
  if (auto resolved = Resolve(); !resolved) {
    std::fprintf(stderr, "[modlock] seat %d keeps its hero: %s\n", slot, resolved.error().c_str());
    std::fflush(stderr);
    return std::nullopt;
  }
  auto system = ResolveLiveEntitySystem();
  if (!system) {
    return std::nullopt;
  }
  auto* identity = IdentityOfIndex(EntityListOf(*system), static_cast<uint32_t>(slot) + 1);
  void* controller = identity ? InstanceOf(identity) : nullptr;
  const auto controller_handle = controller ? ReferenceHandleOf(controller) : std::nullopt;
  if (!controller_handle) {
    return std::nullopt;
  }

  // Kill its hero if it stands. A dead hero draws nothing and casts nothing;
  // removing one would leave its abilities and items networked, which closes
  // clients.
  uint32_t hero = kInvalidEHandleIndex;
  std::memcpy(&hero, static_cast<const unsigned char*>(controller) + hero_offset_, sizeof(hero));
  const char* outcome = "no hero";
  if (void* pawn = EntityInstance(*system, hero)) {
    int32_t health = 0;
    std::memcpy(&health, static_cast<const unsigned char*>(pawn) + health_offset_, sizeof(health));
    outcome = "hero killed";
    if (health > 0) {
      if (auto killed = damage_->Kill(pawn); !killed) {
        outcome = "hero left standing";
        std::fprintf(stderr, "[modlock] seat %d hero kill failed: %s\n", slot,
                     killed.error().c_str());
      }
    }
  }

  // Take the controller off its team, so the scoreboard stops listing it.
  WorldEntities world(*calls_);
  const auto moved = world.FireInput(*controller_handle, "ChangeTeam", EntityValue{0});
  std::fprintf(stderr, "[modlock] seat %d vacated: %s, controller %s\n", slot, outcome,
               moved && *moved ? "off its team" : "kept its team");
  std::fflush(stderr);
  return hero;
}

}  // namespace modlock::gameinterop
