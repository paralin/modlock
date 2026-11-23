//go:build deps_only

#include "wasm/mod_thread.h"

#include <algorithm>
#include <array>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace modlock::wasm {
namespace {

// kReads are the Host methods that return game state and change nothing. A
// read of the same request returns the same answer for the whole of a window,
// which is what a mod running in the game thread's frame sees today.
constexpr std::array kReads = std::to_array<std::string_view>({
    "Players",
    "Pawn",
    "Pawns",
    "Abilities",
    "HeroBuild",
    "ReadField",
    "ReadModifierField",
    "ReadFieldList",
    "EntityClass",
    "EntityName",
    "EntityPosition",
    "ModifierState",
    "Modifiers",
    "ObjectEntity",
    "ReadNpc",
    "PickupPresent",
    "FindEntities",
    "FindEntitiesByName",
    "Trace",
    "MatchClock",
    "Rift",
    "Buttons",
});

// kImmutables are the reads whose answer the mod's own calls and the game
// leave alone, but for WriteFile, which clears the mod thread's copies. They
// hop once.
constexpr std::array kImmutables = std::to_array<std::string_view>({
    "ReadFile",
    "ReadFileBytes",
});

// kWrites are the Host methods that return Empty, which the mod thread answers
// with success before the game has run them. A call whose refusal a mod acts
// on is left out and hops: SetPlayerSetting, WriteFile, HoldReload, Handoff
// and PlayerHandoff report a bad name, a full quota or an oversize blob, and
// they run too rarely to gain from a buffer.
constexpr std::array kWrites = std::to_array<std::string_view>({
    "Log",
    "ServerCommand",
    "Chat",
    "CenterText",
    "Announce",
    "Precache",
    "AddMetric",
    "Spectate",
    "Watch",
    "FreeCamera",
    "MoveCamera",
    "ClearCamera",
    "Unwatch",
    "Respawn",
    "ChangeTeam",
    "ClearItems",
    "Freeze",
    "RestoreStamina",
    "SetStamina",
    "RefreshAbility",
    "SetAbility",
    "GiveItem",
    "BuyItem",
    "TrainAbility",
    "ReplaceAbility",
    "HoldModifier",
    "GiveModifier",
    "Teleport",
    "Zipline",
    "MovePlayer",
    "Steer",
    "Drive",
    "AdjustSouls",
    "AdjustAbilityPoints",
    "StartingSouls",
    "ResetHero",
    "Heal",
    "RestoreHero",
    "ScreenEffect",
    "ClearScreenEffect",
    "SilenceVoice",
    "SilencePlayerVoice",
    "WriteField",
    "WriteFieldList",
    "HoldModifierState",
    "SetFreeForAll",
    "SetTeamSeating",
    "MoveObject",
    "SetText",
    "SetObjectHidden",
    "ShowObjectOnlyTo",
    "ShowObjectToAll",
    "RemoveObject",
    "RemoveBot",
    "BlockInput",
    "BlockPlayerInput",
    "Press",
    "WatchInput",
    "RemapInput",
    "Ui",
    "ShowEntityOnlyTo",
    "ShowEntityToAll",
    "ClearMap",
    "KillEntity",
    "RestoreNpcs",
    "Pause",
    "HoldMatchClock",
    "StartRift",
    "MoveEntity",
    "PushEntity",
    "AdjustSound",
    "StopSound",
    "Kill",
    "SetVelocity",
    "WatchMovement",
    "WatchProjectiles",
    "HoldEntityModifier",
    "GiveEntityModifier",
});

// kWriteFile replaces a file ReadFile may have returned, so it empties the
// mod thread's copies.
constexpr std::string_view kWriteFile = "WriteFile";

// Relax tells the processor the thread is in a spin loop.
inline void Relax() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// current is the ModThread whose thread this is, or null on any other.
thread_local const ModThread* current = nullptr;

}  // namespace

CallClass ClassOf(std::string_view method) {
  static const auto table = [] {
    std::unordered_map<std::string_view, CallClass> classes;
    for (const auto name : kReads) {
      classes.emplace(name, CallClass::kRead);
    }
    for (const auto name : kImmutables) {
      classes.emplace(name, CallClass::kImmutable);
    }
    for (const auto name : kWrites) {
      classes.emplace(name, CallClass::kWrite);
    }
    return classes;
  }();
  const auto found = table.find(method);
  return found == table.end() ? CallClass::kHop : found->second;
}

template <typename Ready>
bool ModThread::Bell::Wait(Ready ready, bool spin, const std::atomic<bool>& partner,
                           std::atomic<bool>* self,
                           std::optional<std::chrono::steady_clock::time_point> deadline) {
  using Clock = std::chrono::steady_clock;
  if (ready()) {
    return true;
  }

  // Watch the condition while the partner runs. Once its flag clears, give it
  // the grace for a change already on its way, and stop at the deadline. The
  // clock is read every sixteenth look while the partner runs, and at every
  // look once it does not, so a handoff that lands at once does not pay for it.
  if (spin) {
    std::optional<Clock::time_point> cleared;
    for (uint32_t look = 1;; ++look) {
      Relax();
      if (ready()) {
        waits_.spins.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
      if (partner.load()) {
        cleared.reset();
        if ((look & 15) == 0 && deadline && Clock::now() >= *deadline) {
          break;
        }
        continue;
      }
      const auto now = Clock::now();
      if (!cleared) {
        cleared = now;
      }
      if (now - *cleared >= kPartnerGrace || (deadline && now >= *deadline)) {
        break;
      }
    }
  }
  if (deadline && Clock::now() >= *deadline) {
    return ready();
  }

  waits_.sleeps.fetch_add(1, std::memory_order_relaxed);
  if (self) {
    self->store(false);
  }
  const bool satisfied = Sleep(ready, deadline);
  if (self) {
    self->store(true);
  }
  return satisfied;
}

template <typename Ready>
bool ModThread::Bell::Sleep(Ready ready,
                            std::optional<std::chrono::steady_clock::time_point> deadline) {
  // Registering as a sleeper before the check under the lock pairs with
  // Notify's look at sleepers_.
  std::unique_lock lock(mu_);
  sleepers_.fetch_add(1);
  bool satisfied = true;
  if (deadline) {
    satisfied = cv_.wait_until(lock, *deadline, ready);
  } else {
    cv_.wait(lock, ready);
  }
  sleepers_.fetch_sub(1);
  return satisfied;
}

void ModThread::Bell::Notify() {
  if (sleepers_.load() == 0) {
    return;
  }
  if (waiter_running_) {
    waiter_running_->store(true);
  }

  // Take the lock so the notice cannot fall between a sleeper's check and its
  // wait.
  {
    std::lock_guard lock(mu_);
  }
  cv_.notify_all();
}

template <typename Change>
void ModThread::Bell::Set(Change change) {
  if (waiter_running_) {
    waiter_running_->store(true);
  }
  {
    std::lock_guard lock(mu_);
    change();
  }
  cv_.notify_all();
}

ModThread::ModThread(Runtime& runtime, ModThreadSettings settings, Instance::HostCall game_call)
    : runtime_(runtime), settings_(settings), game_call_(std::move(game_call)) {}

std::expected<std::unique_ptr<ModThread>, std::string> ModThread::Load(
    Runtime& runtime, std::span<const uint8_t> module, const Limits& limits,
    Instance::HostCall game_call, ModThreadSettings settings) {
  std::unique_ptr<ModThread> mod(new ModThread(runtime, settings, std::move(game_call)));
  auto instance = Instance::Load(runtime, module, limits,
                                 [raw = mod.get()](const Call& call) { return raw->Route(call); });
  if (!instance) {
    return std::unexpected(instance.error());
  }
  mod->instance_ = std::move(*instance);
  return std::move(mod);
}

ModThread::~ModThread() {
  Stop();
}

std::expected<Reply, std::string> ModThread::Begin(const Call& start) {
  // Run the Start event where the game answers each call directly, and start
  // the thread only for a mod that came up.
  Completion done = Deliver(start);
  if (done.reply) {
    started_ = true;
    thread_ = std::thread([this] { Run(); });
  }
  return std::move(done.reply);
}

void ModThread::Post(Call call) {
  if (stopping_.load() || done_.load()) {
    return;
  }
  if (call.method() != "Frame") {
    if (settings_.posts_in_place) {
      if (auto done = Borrow(call)) {
        ++in_place_;
        const std::lock_guard lock(mu_);
        completed_.push_back(std::move(*done));
        return;
      }
    }
    ++queued_posts_;
  }
  {
    // A Frame already queued gives way to the newer one, which takes the back
    // of the queue so the events posted between them keep their order.
    std::lock_guard lock(mu_);
    if (call.method() == "Frame") {
      const auto old = std::ranges::find_if(
          queue_, [](const Call& queued) { return queued.method() == "Frame"; });
      if (old != queue_.end()) {
        queue_.erase(old);
        pending_.fetch_sub(1);
      }
    }
    queue_.push_back(std::move(call));
    pending_.fetch_add(1);
    queued_.store(queue_.size());
  }
  to_mod_.Notify();
}

std::optional<Completion> ModThread::Borrow(const Call& call) {
  // Borrow the instance only from a mod that is idle with nothing queued: the
  // mod thread is then asleep and touches nothing, and no event waits that the
  // event would overtake.
  if (!started_ || stopping_.load() || done_.load() || in_window_ || applying_ ||
      borrowed_.load() || pending_.load() != 0 || instance_->Failure()) {
    return std::nullopt;
  }
  borrowed_.store(true);
  ApplyBuffer(nullptr);
  Completion done = Deliver(call);

  // An event posted during the borrow waited for it to end.
  borrowed_.store(false);
  to_mod_.Notify();
  return done;
}

std::optional<Completion> ModThread::Decide(const Call& call) {
  auto done = Borrow(call);
  auto counted = std::ranges::find(decisions_, call.method(), &DecisionCount::method);
  if (counted == decisions_.end()) {
    counted = decisions_.insert(decisions_.end(), DecisionCount{.method = call.method()});
  }
  ++(done ? counted->answered : counted->defaulted);
  return done;
}

WindowStats ModThread::Window(std::chrono::steady_clock::time_point deadline,
                              std::optional<Call> frame) {
  using Clock = std::chrono::steady_clock;
  const auto began = Clock::now();
  WindowStats stats;
  if (!started_ || stopping_.load() || in_window_ || applying_ || borrowed_.load()) {
    return stats;
  }
  in_window_ = true;
  game_running_.store(true);
  stats.in_place = std::exchange(in_place_, 0);
  stats.queued = std::exchange(queued_posts_, 0);
  stats.decisions = std::exchange(decisions_, {});

  // Bring the game up to date with what the mod wrote, and the snapshot with
  // the game, and only then wake the mod, so its Frame reads the new snapshot.
  ApplyBuffer(&stats);
  Replay(&stats);
  PostCompleted();
  if (frame) {
    Post(std::move(*frame));
  }

  // A hop that waited since the last window is answered whatever the deadline,
  // so a mod that hops without end still moves one call a window.
  if (hop_.load() == HopState::kPosted) {
    Serve(&stats);
  }

  // Answer hops until the mod is idle with nothing queued, or time is up. The
  // game thread watches while the mod runs and sleeps only when the mod does,
  // and the deadline bounds the wait.
  const auto ready = [this] {
    return hop_.load() == HopState::kPosted || pending_.load() == 0 || done_.load();
  };
  while (Clock::now() < deadline) {
    if (!to_game_.Wait(ready, settings_.spin, mod_running_, nullptr, deadline)) {
      break;
    }
    if (hop_.load() != HopState::kPosted) {
      stats.idle = true;
      break;
    }
    Serve(&stats);
  }

  // The writes the mod made since its last hop stay buffered for the next
  // window, so the Frame event's writes land one tick late. Applying the buffer
  // once more here, when the window closes idle, would remove that lag.
  CountReads(&stats);
  stats.time = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began);
  game_running_.store(false);
  in_window_ = false;
  return stats;
}

std::vector<Completion> ModThread::Completed() {
  std::lock_guard lock(mu_);
  return std::exchange(completed_, {});
}

void ModThread::Stop() {
  // Setting the flag and waking the mod happen under the lock a sleeping hop
  // waiter checks its condition under, so the wake cannot be lost. A waiting
  // hop sees the flag and returns an error, which is the answer the joining
  // game thread gives it, since it serves no more hops.
  to_mod_.Set([this] { stopping_.store(true); });
  if (instance_) {
    instance_->Interrupt();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::optional<std::string> ModThread::Failure() const {
  std::lock_guard lock(mu_);
  return failure_;
}

bool ModThread::OnModThread() const {
  return current == this;
}

void ModThread::Run() {
  current = this;
  Call call;
  const auto ready = [&] {
    return stopping_.load() || TakeEvent(call);
  };
  mod_running_.store(true);
  while (!stopping_.load()) {
    mod_running_.store(false);
    to_mod_.Sleep(ready, std::nullopt);
    mod_running_.store(true);
    if (stopping_.load()) {
      break;
    }
    Completion done = Deliver(call);
    const bool failed = instance_->Failure().has_value();
    {
      std::lock_guard lock(mu_);
      completed_.push_back(std::move(done));
    }
    pending_.fetch_sub(1);
    to_game_.Notify();
    if (failed) {
      break;
    }
  }

  // A failed or stopped mod runs no more events; the game thread's wait ends.
  mod_running_.store(false);
  done_.store(true);
  to_game_.Notify();
  current = nullptr;
}

bool ModThread::TakeEvent(Call& call) {
  if (queued_.load() == 0 || borrowed_.load()) {
    return false;
  }
  std::lock_guard lock(mu_);
  if (queue_.empty()) {
    return false;
  }
  call = std::move(queue_.front());
  queue_.pop_front();
  queued_.store(queue_.size());
  return true;
}

Completion ModThread::Deliver(const Call& call) {
  Completion done{.method = call.method(), .reply = instance_->Deliver(call)};

  // Capture what the instance says about the call while no other thread can
  // run it.
  if (instance_->Slow()) {
    done.slow = true;
    done.report = instance_->Report();
    done.description = instance_->Describe();
  }
  if (instance_->Failure()) {
    std::lock_guard lock(mu_);
    failure_ = instance_->Failure();
  }
  return done;
}

Reply ModThread::Route(const Call& call) {
  // The game thread runs the mod directly, so its calls need no handoff, but
  // they follow every write the mod made before.
  if (call.method() == "Submit") {
    return Submit(call);
  }
  if (!OnModThread()) {
    return Direct(call);
  }

  // The mod thread answers what it can from the buffer and the snapshot.
  switch (ClassOf(call.method())) {
    case CallClass::kWrite:
      if (settings_.buffer_writes && BufferWrite(call)) {
        ++issued_;
        return Reply();
      }
      return Hop(call, true);
    case CallClass::kRead:
      if (settings_.replay_reads) {
        if (auto answer = ReadSnapshot(call)) {
          return *std::move(answer);
        }
      }
      return Hop(call, false);
    case CallClass::kImmutable: {
      const std::string key = Key(call);
      if (const auto kept = immutable_.find(key); kept != immutable_.end()) {
        return kept->second;
      }
      Reply reply = Hop(call, false);
      if (reply.error().empty()) {
        immutable_.emplace(key, reply);
      }
      return reply;
    }
    case CallClass::kHop:
      if (call.method() == kWriteFile) {
        immutable_.clear();
      }
      return Hop(call, true);
  }
  return Hop(call, true);
}

Reply ModThread::Direct(const Call& call) {
  ApplyBuffer(nullptr);
  const CallClass kind = ClassOf(call.method());
  if (kind == CallClass::kWrite || kind == CallClass::kHop) {
    ++issued_;
    ++executed_;
  }
  return game_call_(call);
}

Reply ModThread::Submit(const Call& call) {
  // The game thread has no mod to spare, so a call submitted there runs at
  // once; the mod thread's calls take their places in the buffer.
  return futures_.Submit(call, [this](uint64_t id, const Call& submitted) {
    if (OnModThread()) {
      Queue(id, submitted);
    } else {
      futures_.Complete(id, Direct(submitted));
    }
  });
}

void ModThread::Queue(uint64_t id, const Call& call) {
  switch (ClassOf(call.method())) {
    case CallClass::kWrite:
      ++issued_;
      if (settings_.buffer_writes && BufferWrite(call)) {
        futures_.Complete(id, Reply());
      } else {
        Enqueue({.call = call, .id = id});
      }
      return;
    case CallClass::kRead:
      if (settings_.replay_reads) {
        if (auto answer = ReadSnapshot(call)) {
          futures_.Complete(id, *std::move(answer));
          return;
        }
      }
      Enqueue({.call = call, .id = id, .changes = false});
      return;
    case CallClass::kImmutable:
      if (const auto kept = immutable_.find(Key(call)); kept != immutable_.end()) {
        futures_.Complete(id, kept->second);
      } else {
        Enqueue({.call = call, .id = id, .changes = false});
      }
      return;
    case CallClass::kHop:
      if (call.method() == kWriteFile) {
        immutable_.clear();
      }
      ++issued_;
      Enqueue({.call = call, .id = id});
      return;
  }
}

void ModThread::PostCompleted() {
  const CompletedEvent event = futures_.Take();
  if (event.calls_size() == 0) {
    return;
  }
  Call call;
  call.set_method("Completed");
  call.set_request(event.SerializeAsString());
  Post(std::move(call));
}

bool ModThread::BufferWrite(const Call& call) {
  std::lock_guard lock(mu_);
  if (buffer_.size() >= settings_.write_limit) {
    return false;
  }
  buffer_.push_back({.call = call});
  buffered_.store(buffer_.size());
  return true;
}

void ModThread::Enqueue(Buffered entry) {
  std::lock_guard lock(mu_);
  buffer_.push_back(std::move(entry));
  buffered_.store(buffer_.size());
}

std::optional<Reply> ModThread::ReadSnapshot(const Call& call) {
  const auto snapshot = LatestSnapshot();
  std::string key = Key(call);
  const Entry* entry = nullptr;
  if (snapshot) {
    if (const auto found = snapshot->entries.find(key); found != snapshot->entries.end()) {
      entry = &found->second;
    }
  }

  // Strict mode lets a read use the snapshot only while the snapshot covers
  // every changing call the mod has made. A hop's own apply does not refresh
  // it, so reads keep hopping until the next window rebuilds it. The entry
  // stays marked used, so the rebuild replays it.
  const bool covers = !snapshot || !settings_.strict || snapshot->epoch >= issued_;
  if (entry) {
    entry->used.store(true, std::memory_order_relaxed);
    if (covers) {
      hits_.fetch_add(1, std::memory_order_relaxed);
      return entry->reply;
    }
    stale_.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  // The snapshot lacks the read: remember it, so the next window replays it.
  misses_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(mu_);
  if (missed_keys_.size() < settings_.replay_keys && missed_keys_.insert(key).second) {
    missed_.push_back(call);
  }
  return std::nullopt;
}

Reply ModThread::Hop(const Call& call, bool changes) {
  Reply reply;
  if (stopping_.load()) {
    reply.set_error("the mod is stopping");
    return reply;
  }
  if (changes) {
    ++issued_;
  }
  hop_call_ = call;
  hop_changes_ = changes;
  hop_.store(HopState::kPosted);
  to_game_.Notify();

  // Wait for the game thread's answer, or for the stop that ends the wait.
  const auto answered = [this] {
    return hop_.load() == HopState::kAnswered || stopping_.load();
  };
  to_mod_.Wait(answered, settings_.spin, game_running_, &mod_running_, std::nullopt);
  if (hop_.load() == HopState::kAnswered) {
    reply = std::move(hop_reply_);
  } else {
    reply.set_error("the mod is stopping");
  }
  hop_.store(HopState::kNone);
  return reply;
}

void ModThread::ApplyBuffer(WindowStats* stats) {
  if (applying_ || buffered_.load() == 0) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    applying_batch_.swap(buffer_);
    buffered_.store(0);
  }

  // Run the writes in the order the mod made them. A decision these writes
  // provoke finds the mod busy.
  applying_ = true;
  for (Buffered& entry : applying_batch_) {
    Reply reply = game_call_(entry.call);
    executed_ += entry.changes ? 1 : 0;
    if (stats) {
      ++stats->writes;
      stats->failed += reply.error().empty() ? 0 : 1;
    }
    if (entry.id != 0) {
      futures_.Complete(entry.id, std::move(reply));
    }
  }
  applying_ = false;
  applying_batch_.clear();
}

void ModThread::Replay(WindowStats* stats) {
  if (!settings_.replay_reads) {
    return;
  }

  // Replay what the mod used of the last snapshot, and what it asked for and
  // the snapshot lacked, up to the limit.
  std::vector<std::pair<std::string, Call>> wanted;
  std::unordered_set<std::string> seen;
  if (const auto previous = LatestSnapshot()) {
    for (const auto& [key, entry] : previous->entries) {
      if (entry.used.load(std::memory_order_relaxed)) {
        wanted.emplace_back(key, entry.call);
        seen.insert(key);
      }
    }
  }
  std::vector<Call> missed;
  {
    std::lock_guard lock(mu_);
    missed.swap(missed_);
    missed_keys_.clear();
  }
  for (Call& call : missed) {
    if (wanted.size() >= settings_.replay_keys) {
      break;
    }
    std::string key = Key(call);
    if (seen.insert(key).second) {
      wanted.emplace_back(std::move(key), std::move(call));
    }
  }

  // The snapshot holds the game as it is now, after the mod's writes.
  auto next = std::make_shared<Snapshot>();
  next->epoch = executed_;
  applying_ = true;
  for (auto& [key, call] : wanted) {
    Entry& entry = next->entries[key];
    entry.reply = game_call_(call);
    entry.call = std::move(call);
  }
  applying_ = false;
  stats->replayed = static_cast<uint32_t>(wanted.size());
  {
    const std::lock_guard lock(snapshot_mutex_);
    snapshot_ = std::move(next);
  }
}

void ModThread::Serve(WindowStats* stats) {
  // The game sees the mod's calls in order, so what it wrote goes first.
  ApplyBuffer(stats);
  Reply reply = game_call_(hop_call_);
  if (hop_changes_) {
    ++executed_;
  }
  ++stats->hops;
  hop_reply_ = std::move(reply);
  to_mod_.Set([this] { hop_.store(HopState::kAnswered); });
}

void ModThread::CountReads(WindowStats* stats) {
  const Counters now{hits_.load(), misses_.load(), stale_.load(), waits_.spins.load(),
                     waits_.sleeps.load()};
  stats->hits = now.hits - reported_.hits;
  stats->misses = now.misses - reported_.misses;
  stats->stale = now.stale - reported_.stale;
  stats->spins = now.spins - reported_.spins;
  stats->sleeps = now.sleeps - reported_.sleeps;
  reported_ = now;
}

std::string ModThread::Key(const Call& call) {
  std::string key;
  key.reserve(call.method().size() + 1 + call.request().size());
  key += call.method();
  key += '\0';
  key += call.request();
  return key;
}

}  // namespace modlock::wasm
