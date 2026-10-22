#include "modlock/gameinterop/stamina_observer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "google/protobuf/io/coded_stream.h"
#include "modlock/gameinterop/entity_abi.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"
#include "proto/modlock/stamina_consumed.pb.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

constexpr int32_t kDamageMessageId = 300;
constexpr int32_t kStaminaConsumedMessageId = 337;
constexpr int32_t kHeadHitgroup = 1;
constexpr size_t kMaximumSerializedEventBytes = 1024;
constexpr size_t kHeadshotDedupLimit = 64;

template <typename Message>
bool ParseFramed(std::span<const uint8_t> serialized, Message& message) {
  // Accept one length prefixed message that fills the buffer exactly.
  if (serialized.empty() || serialized.size() > kMaximumSerializedEventBytes) {
    return false;
  }
  google::protobuf::io::CodedInputStream input(serialized.data(),
                                               static_cast<int>(serialized.size()));
  uint32_t length = 0;
  if (!input.ReadVarint32(&length) || length == 0 ||
      length != serialized.size() - static_cast<size_t>(input.CurrentPosition())) {
    return false;
  }
  return message.ParseFromCodedStream(&input) && input.ConsumedEntireMessage();
}

}  // namespace

void StaminaEvidenceTracker::StartEpoch() {
  // Each epoch starts with nothing spent and no evidence.
  ++epoch_;
  spent_ = 0;
  live_ = false;
  lost_ = false;
  last_time_.reset();
  last_values_.reset();
}

void StaminaEvidenceTracker::ResetHeadshots() {
  recent_headshots_.clear();
  pending_headshots_ = 0;
}

std::optional<modlock::StaminaEvidence> StaminaEvidenceTracker::Bind(
    const std::optional<PawnObserver::Sample>& sample) {
  // Without an identified pawn there is nothing to bind.
  if (!sample || sample->steam_id == 0 || sample->pawn_handle == 0 ||
      sample->pawn_handle == 0xffffffff) {
    identity_.reset();
    ResetHeadshots();
    return std::nullopt;
  }

  // A new pawn, player or session starts a new epoch.
  const Identity next{sample->pawn_handle, sample->steam_id, sample->session_generation};
  if (!identity_ || *identity_ != next) {
    identity_ = next;
    StartEpoch();
    ResetHeadshots();
  }

  // Evidence exists only while spending is live and unbroken.
  if (!live_ || lost_ || epoch_ == 0) {
    return std::nullopt;
  }
  modlock::StaminaEvidence result;
  result.set_epoch(epoch_);
  result.set_total_spent(spent_);
  return result;
}

void StaminaEvidenceTracker::Invalidate() {
  lost_ = true;
}

void StaminaEvidenceTracker::Record(std::span<const uint8_t> serialized) {
  // Read a receipt for the bound pawn; a malformed one breaks the evidence.
  if (!identity_) {
    return;
  }
  modlock::engine::StaminaConsumed message;
  if (!ParseFramed(serialized, message) || !message.has_entindex_target() ||
      message.entindex_target() < 0) {
    Invalidate();
    return;
  }
  if (message.entindex_target() != static_cast<int32_t>(identity_->pawn_handle & 0x7fff)) {
    return;
  }

  // A receipt with bad numbers or a repeated time is ambiguous.
  const float before = message.stamina_before();
  const float after = message.stamina_after();
  const float time = message.gametime();
  if (!message.has_stamina_before() || !message.has_stamina_after() || !message.has_gametime() ||
      !std::isfinite(before) || !std::isfinite(after) || !std::isfinite(time) || before < 0 ||
      after < 0 || after > before || time < 0 || (last_time_ && time < *last_time_) ||
      (lost_ && before > after && last_time_ && time <= *last_time_) ||
      (before > after && last_time_ && time == *last_time_ &&
       last_values_ == std::pair{before, after})) {
    // Duplicate receipts are ambiguous, so do not silently double-count or discard them.
    Invalidate();
    return;
  }

  // After a loss, only a positive receipt starts a new epoch.
  if (lost_) {
    // A valid zero-cost observation can advance the known event clock, but
    // only an attributed positive receipt proves that spending is live again.
    if (before == after) {
      last_time_ = time;
      last_values_ = std::pair{before, after};
      return;
    }
    StartEpoch();
  }

  // Add the cost to the running total.
  const double next = spent_ + (static_cast<double>(before) - after);
  if (!std::isfinite(next)) {
    Invalidate();
    return;
  }
  spent_ = next;
  live_ = live_ || before > after;
  last_time_ = time;
  last_values_ = std::pair{before, after};
}

void StaminaEvidenceTracker::RecordDamage(std::span<const uint8_t> serialized) {
  // Accept a well formed headshot by the bound pawn.
  if (!identity_) {
    return;
  }
  modlock::engine::Damage message;
  if (!ParseFramed(serialized, message) || !message.has_damage() || message.damage() <= 0 ||
      !message.has_entindex_attacker() || message.entindex_attacker() < 0 ||
      !message.has_entindex_victim() || message.entindex_victim() < 0 ||
      message.entindex_attacker() == message.entindex_victim() || !message.has_hitgroup_id() ||
      message.hitgroup_id() != kHeadHitgroup || !message.has_server_tick() ||
      message.server_tick() < 0 ||
      message.entindex_attacker() != static_cast<int32_t>(identity_->pawn_handle & 0x7fff)) {
    return;
  }

  // Count each headshot once, in a bounded window of recent ones.
  const HeadshotKey key{message.entindex_attacker(), message.entindex_victim(),
                        message.server_tick()};
  if (std::ranges::find(recent_headshots_, key) != recent_headshots_.end()) {
    return;
  }
  if (recent_headshots_.size() == kHeadshotDedupLimit) {
    recent_headshots_.erase(recent_headshots_.begin());
  }
  recent_headshots_.push_back(key);
  if (pending_headshots_ != std::numeric_limits<uint32_t>::max()) {
    ++pending_headshots_;
  }
}

uint32_t StaminaEvidenceTracker::ConsumeHeadshots() {
  return std::exchange(pending_headshots_, 0);
}

struct StaminaObserver::Impl {
#if defined(_WIN32)
  // Windows bf_write layout from sourcesdk/public/tier1/bitbuf.h. The current
  // networksystem serializer writes a varint byte count before the protobuf.
  struct BitWriter {
    uint8_t* data;
    int32_t bytes;
    int32_t bits;
    int32_t cursor = 0;
    const char* debug_name = "modlock stamina";
    bool overflow = false;
    bool assert_overflow = false;
    bool aligned = true;
  };
  static_assert(offsetof(BitWriter, debug_name) == 24);
  static_assert(offsetof(BitWriter, overflow) == 32);
  static_assert(sizeof(BitWriter) == 40);
  using PostEvent = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                             unsigned long, int32_t);
  static inline std::mutex mutex;
  static inline Impl* active = nullptr;
  // Retained after unhooking so a thunk already dispatched by the engine can
  // still forward. The game module outlives this observer.
  static inline PostEvent original = nullptr;
  void* events = nullptr;
  void* messages = nullptr;
  size_t serialize_slot = 0;
  DWORD frame_thread = 0;
  StaminaEvidenceTracker tracker;
  std::optional<ThunkOwner> owner;
  unsigned trace_count = 0;
  bool trace_live = false;

  void Trace(const char* reason, std::span<const uint8_t> bytes = {}) {
    if (!InteropTraceEnabled() || trace_count++ >= 64) {
      return;
    }
    std::fprintf(stderr, "[modlock] stamina trace: %s thread=%lu frame_thread=%lu bytes=", reason,
                 GetCurrentThreadId(), frame_thread);
    for (const auto byte : bytes) {
      std::fprintf(stderr, "%02x", byte);
    }
    std::fprintf(stderr, "\n");
  }

  static void Clear() {
    std::lock_guard lock(mutex);
    active = nullptr;
  }

  static void OnEvent(void* self, int32_t slot, bool local, int32_t count, const uint64_t* clients,
                      void* serializer, const void* message, unsigned long size,
                      int32_t buffer_type) {
    PostEvent forward;
    {
      std::lock_guard lock(mutex);
      forward = original;
      if (active && self == active->events) {
        active->ReadEvent(serializer, message);
      }
    }
    if (forward) {
      forward(self, slot, local, count, clients, serializer, message, size, buffer_type);
    }
  }

  void ReadEvent(void* serializer, const void* message) {
    // Read only stamina and damage messages on the frame thread.
    if (frame_thread == 0 || !serializer || !message) {
      return;
    }
    const auto id = NetworkMessageIdOf(serializer);
    if (id != kStaminaConsumedMessageId && id != kDamageMessageId) {
      return;
    }
    if (GetCurrentThreadId() != frame_thread || !messages) {
      Trace("native event thread or serializer unavailable");
      if (id == kStaminaConsumedMessageId) {
        tracker.Invalidate();
      }
      return;
    }

    // Serialize the message back to its wire bytes.
    alignas(8) std::array<uint8_t, 1024> bytes{};
    BitWriter writer{bytes.data(), static_cast<int32_t>(bytes.size()),
                     static_cast<int32_t>(bytes.size() * 8)};
    using Serialize = bool (*)(void*, BitWriter&, const void*);
    if (!NetworkMessagesMethod<Serialize>(messages, serialize_slot)(messages, writer, message) ||
        writer.overflow || writer.cursor <= 0 ||
        writer.cursor > static_cast<int32_t>(bytes.size() * 8) || writer.cursor % 8 != 0) {
      Trace("native event serialization failed");
      if (id == kStaminaConsumedMessageId) {
        tracker.Invalidate();
      }
      return;
    }

    // Pass the bytes to the tracker.
    const auto serialized = std::span(bytes.data(), static_cast<size_t>(writer.cursor / 8));
    Trace(id == kStaminaConsumedMessageId ? "stamina receipt" : "damage receipt", serialized);
    if (id == kStaminaConsumedMessageId) {
      tracker.Record(serialized);
    } else {
      tracker.RecordDamage(serialized);
    }
  }
#endif
};

StaminaObserver::StaminaObserver(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StaminaObserver::StaminaObserver(StaminaObserver&&) noexcept = default;
StaminaObserver& StaminaObserver::operator=(StaminaObserver&&) noexcept = default;
StaminaObserver::~StaminaObserver() = default;

std::expected<StaminaObserver, std::string> StaminaObserver::Install() {
#if defined(_WIN32)
  // Install once, resolving the event system first.
  std::lock_guard lock(Impl::mutex);
  if (Impl::active) {
    return std::unexpected("stamina observer already installed");
  }
  const auto resolved = ResolveEngineInterface(L"engine2.dll", "GameEventSystemServerV001");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  // Resolve the message serializer and both virtual slots.
  void* events = *resolved;
  const auto messages = ResolveEngineInterface(L"networksystem.dll", "NetworkMessagesVersion001");
  if (!messages) {
    return std::unexpected(messages.error());
  }
  const auto post_slot = ResolveVirtualSlot("game-event-system.post-event");
  const auto serialize_slot = ResolveVirtualSlot("network-messages.serialize");
  if (!post_slot) {
    return std::unexpected(post_slot.error());
  }
  if (!serialize_slot) {
    return std::unexpected(serialize_slot.error());
  }

  // Hook the post slot.
  auto impl = std::make_unique<Impl>();
  impl->events = events;
  impl->messages = *messages;
  impl->serialize_slot = *serialize_slot;
  auto hook = VtableSlotHook::Install(events, *post_slot, reinterpret_cast<void*>(&Impl::OnEvent));
  if (!hook) {
    return std::unexpected(hook.error());
  }

  // Route events to this observer while it lives.
  Impl::original = reinterpret_cast<Impl::PostEvent>(hook->Original());
  impl->owner.emplace(std::move(*hook), &Impl::Clear);
  Impl::active = impl.get();
  return StaminaObserver(std::move(impl));
#else
  return std::unexpected("native stamina observation requires the Windows host build");
#endif
}

void StaminaObserver::Observe(std::optional<PawnObserver::Sample>& sample) {
#if defined(_WIN32)
  // Evidence is valid only on one frame thread.
  std::lock_guard lock(Impl::mutex);
  auto& state = *impl_;
  const auto thread = GetCurrentThreadId();
  if (state.frame_thread != 0 && state.frame_thread != thread) {
    state.tracker.Invalidate();
    if (sample) {
      sample->stamina_evidence.reset();
      sample->headshot_count = 0;
    }
    return;
  }

  // Bind the sample and report the evidence.
  state.frame_thread = thread;
  const auto evidence = state.tracker.Bind(sample);
  const auto headshot_count = state.tracker.ConsumeHeadshots();
  if (evidence.has_value() != state.trace_live) {
    state.Trace(evidence ? "evidence live" : "evidence lost");
    state.trace_live = evidence.has_value();
  }
  if (sample) {
    sample->stamina_evidence = evidence;
    sample->headshot_count = headshot_count;
  }
#else
  if (sample) {
    sample->stamina_evidence.reset();
    sample->headshot_count = 0;
  }
#endif
}

}  // namespace modlock::gameinterop
