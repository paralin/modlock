#include "modlock/gameinterop/native_damage.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>

#include "modlock/gameinterop/entity_abi.h"

namespace modlock::gameinterop {

std::expected<NativeDamage, std::string> NativeDamage::Resolve(const ModuleImage& server,
                                                               void* schema_system) {
  // Resolve the damage calls.
  NativeDamage calls;
  const SignatureCall signatures[] = {
      {"damage.construct", &calls.construct_},
      {"damage.destroy", &calls.destroy_},
      {"entity.take-damage", &calls.damage_},
  };
  auto resolved = ResolveSignatures(server, signatures);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Resolve the damage info's size and fields.
  auto size = SchemaClassSizeOf(schema_system, "server.dll", "CTakeDamageInfo");
  auto flags = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_nDamageFlags");
  auto health = SchemaFieldOf(schema_system, "server.dll", "CBaseEntity", "m_iHealth");
  auto life = SchemaFieldOf(schema_system, "server.dll", "CBaseEntity", "m_lifeState");
  auto ability = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_hAbility");
  auto hit_group = SchemaFieldOf(schema_system, "server.dll", "CTakeDamageInfo", "m_iHitGroupId");
  if (!hit_group) {
    return std::unexpected(hit_group.error());
  }

  // Every field must resolve with room for its value.
  if (!size) {
    return std::unexpected(size.error());
  }
  if (!flags) {
    return std::unexpected(flags.error());
  }
  if (!health) {
    return std::unexpected(health.error());
  }
  if (!ability) {
    return std::unexpected(ability.error());
  }
  if (!life) {
    return std::unexpected(life.error());
  }
  if (flags->size < sizeof(uint64_t) || flags->offset > *size ||
      *size - flags->offset < sizeof(uint64_t) || health->size < sizeof(int32_t) ||
      life->size < sizeof(uint8_t) || ability->size < sizeof(uint32_t) ||
      hit_group->size < sizeof(int32_t)) {
    return std::unexpected("native damage field storage is incomplete");
  }

  // Keep the offsets.
  calls.size_ = *size;
  calls.flags_offset_ = flags->offset;
  calls.health_offset_ = health->offset;
  calls.life_state_offset_ = life->offset;
  calls.ability_offset_ = ability->offset;
  calls.hit_group_offset_ = hit_group->offset;

  // Forced deaths bypass mitigation and procs while preserving engine credit.
  for (const char* name : {"DFLAG_SUPPRESS_DAMAGE_MODIFICATION", "DFLAG_ALLOW_SUICIDE",
                           "DFLAG_DO_NOT_PROC", "DFLAG_IGNORE_RESISTANCES"}) {
    auto flag = SchemaEnumValueOf(schema_system, "server.dll", "TakeDamageFlags_t", name);
    if (!flag) {
      return std::unexpected(flag.error());
    }
    calls.kill_flags_ |= static_cast<uint64_t>(*flag);
  }

  // Environmental damage additionally suppresses credit and rewards.
  calls.hazard_flags_ = calls.kill_flags_;
  for (const char* name : {"DFLAG_SUPPRESS_DIRECT_GOLD_BOUNTY", "DFLAG_SUPPRESS_COINS_GOLD_BOUNTY",
                           "DFLAG_SUPPRESS_KILL_CREDIT", "DFLAG_SUPPRESS_DEATH_CREDIT"}) {
    auto flag = SchemaEnumValueOf(schema_system, "server.dll", "TakeDamageFlags_t", name);
    if (!flag) {
      return std::unexpected(flag.error());
    }
    calls.hazard_flags_ |= static_cast<uint64_t>(*flag);
  }
  return calls;
}

std::expected<void, std::string> NativeDamage::Kill(void* pawn, void* attacker, void* inflictor,
                                                    void* ability) const {
  // A living zero-health pawn still needs its death transition.
  if (!pawn || !ReferenceHandleOf(pawn)) {
    return std::unexpected("elimination pawn is absent");
  }
  uint8_t life = 0;
  std::memcpy(&life, static_cast<const std::byte*>(pawn) + life_state_offset_, sizeof(life));
  if (life != 0) {
    return {};
  }
  int32_t health = 0;
  std::memcpy(&health, static_cast<const std::byte*>(pawn) + health_offset_, sizeof(health));

  // Use the engine's ordinary attribution with forced, exact lethal damage.
  constexpr uint64_t kForceDeath = 0x10;
  const auto flags = (attacker ? kill_flags_ : hazard_flags_) | kForceDeath;
  return Apply(pawn, inflictor ? inflictor : (attacker ? attacker : pawn),
               attacker ? attacker : pawn, ability, static_cast<float>(std::max(1, health)), flags);
}

std::expected<void, std::string> NativeDamage::Hazard(void* pawn, float amount) const {
  if (!pawn || !ReferenceHandleOf(pawn)) {
    return std::unexpected("hazard pawn is absent");
  }
  return Apply(pawn, pawn, pawn, nullptr, amount, hazard_flags_);
}

std::expected<void, std::string> NativeDamage::Hit(void* victim, void* inflictor, void* attacker,
                                                   void* ability, float amount,
                                                   int32_t hit_group) const {
  if (!ReferenceHandleOf(victim) || !ReferenceHandleOf(inflictor) || !ReferenceHandleOf(attacker) ||
      (ability && !ReferenceHandleOf(ability))) {
    return std::unexpected("native damage participant is absent");
  }
  return Apply(victim, inflictor, attacker, ability, amount, 0, hit_group);
}

std::expected<void, std::string> NativeDamage::Apply(void* victim, void* inflictor, void* attacker,
                                                     void* ability, float amount,
                                                     uint64_t extra_flags,
                                                     int32_t hit_group) const {
  // Allocate and construct the damage info, destroyed when done.
  auto* memory = ::operator new(size_, std::align_val_t{16}, std::nothrow);
  if (!memory) {
    return std::unexpected("native damage allocation failed");
  }
  std::memset(memory, 0, size_);
  construct_(memory, inflictor, attacker, ability, amount, 0, 0);
  const auto release = [destroy = destroy_](void* value) {
    destroy(value);
    ::operator delete(value, std::align_val_t{16});
  };
  const std::unique_ptr<void, decltype(release)> info(memory, release);

  // Set the flags.
  uint64_t flags = 0;
  auto* field = static_cast<std::byte*>(memory) + flags_offset_;
  std::memcpy(&flags, field, sizeof(flags));
  flags = (flags & ~uint64_t{0x8}) | extra_flags;
  std::memcpy(field, &flags, sizeof(flags));

  // Name the ability and hit group, then apply the damage.
  if (ability) {
    const auto handle = *ReferenceHandleOf(ability);
    std::memcpy(static_cast<std::byte*>(memory) + ability_offset_, &handle, sizeof(handle));
  }
  if (hit_group >= 0) {
    std::memcpy(static_cast<std::byte*>(memory) + hit_group_offset_, &hit_group, sizeof(hit_group));
  }
  damage_(victim, memory, nullptr);
  return {};
}

}  // namespace modlock::gameinterop
