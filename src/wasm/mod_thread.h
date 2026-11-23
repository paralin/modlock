#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/wasm/profile/profile.pb.h"
#include "wasm/instance.h"

namespace modlock::wasm {

// CallClass says where a host call from the mod thread runs and what the mod
// thread may answer without the game thread.
enum class CallClass {
  // kRead returns game state and changes nothing. The mod thread answers it
  // from the snapshot when the snapshot holds the same request, and hops
  // otherwise.
  kRead,
  // kWrite returns Empty. The mod thread buffers it and answers success; the
  // game thread applies the buffer in order.
  kWrite,
  // kImmutable returns something that never changes while the mod lives. It
  // hops once and is kept.
  kImmutable,
  // kHop is any other call, including one the table does not name: the mod
  // thread waits while the game thread answers it.
  kHop,
};

// ClassOf returns the class of a host method. The table lives in mod_thread.cc
// beside the reason for each entry; it should come from a method option in
// wasm.proto.
[[nodiscard]] CallClass ClassOf(std::string_view method);

// kPartnerGrace is how long a waiter keeps watching after its partner's
// running flag clears, before it sleeps. A partner that just cleared the flag
// is usually about to answer, and a handoff between two threads that both run
// costs tens of nanoseconds, where waking a sleeper costs microseconds. With
// no grace both threads sleep on every round trip.
inline constexpr std::chrono::microseconds kPartnerGrace{5};

// ModThreadSettings are the knobs of one ModThread. A probe sets them to
// compare the designs.
struct ModThreadSettings {
  // buffer_writes answers a kWrite call at once and applies it later. Off,
  // every write hops.
  bool buffer_writes = true;
  // replay_reads answers a kRead call from the snapshot. Off, every read hops.
  bool replay_reads = true;
  // strict sends a read to the game thread whenever the mod has made a call
  // that changes the game since the snapshot was taken, so every read sees the
  // mod's own writes. Off, a read sees the game as the window started.
  bool strict = true;
  // spin makes a waiter watch an atomic while its partner runs, before it
  // sleeps on a condition variable. Off, a waiter sleeps at once.
  bool spin = true;
  // replay_keys is the most distinct reads the game thread replays at the start
  // of a window; a mod that reads more distinct requests than this hops for
  // the rest.
  size_t replay_keys = 512;
  // write_limit is the most writes the buffer holds. A mod that outruns the
  // game thread's windows waits in a hop instead of growing memory.
  size_t write_limit = 8192;
};

// WindowStats is what one Window did, for the host to report.
struct WindowStats {
  // hops is the calls the game thread answered for the mod during the window.
  uint32_t hops = 0;
  // writes is the buffered writes applied, of which failed returned an error.
  uint32_t writes = 0;
  uint32_t failed = 0;
  // replayed is the reads replayed to build the snapshot. hits and misses are
  // the reads the mod made since the last window: answered from the snapshot,
  // or hopped because the snapshot lacked them. stale is the reads hopped
  // because the mod had written since the snapshot, which strict mode sends.
  uint32_t replayed = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t stale = 0;
  // spins and sleeps count the waits of both threads since the last window:
  // those that the watched atomic ended, and those that fell back to the
  // condition variable. A sleep costs microseconds a spin does not.
  uint64_t spins = 0;
  uint64_t sleeps = 0;
  // time is how long the window lasted on the game thread, answering hops
  // included.
  std::chrono::nanoseconds time{};
  // idle is true when the window ended because the mod finished its events,
  // and false when the deadline came first.
  bool idle = false;
};

// Completion is one finished event: the mod's reply or why it failed, and what
// the instance reported about the call, captured on the thread that ran it.
struct Completion {
  std::string method;
  std::expected<Reply, std::string> reply;
  // slow is true when the event ran past the instance's slow limit; report
  // and description then say where the time went.
  bool slow = false;
  std::string report;
  profile::SlowFrame description;
};

// ModThread runs one mod's events on a thread of its own while every call the
// mod makes into the game still runs on the game thread, the one that created
// it. The mod sees the same synchronous host_call either way.
//
// The game thread owns the mod through four calls. Post queues an event for the
// mod thread. Window, after the engine's frame, lets the mod thread run and
// answers what it asks for until it is idle or a deadline passes. Decide
// delivers an event that needs an answer now, on the game thread, when the mod
// is idle. Completed hands back the finished events. Every one of these runs
// on the game thread; nothing here is safe from a third thread except Failure.
//
// A call from the mod thread takes one of four paths, by its class:
//   - A write goes to a buffer, in order, and the mod gets success at once.
//   - A read finds the same request in the snapshot, which Window rebuilds by
//     replaying the reads the mod made since the last window, or else hops.
//   - An immutable read hops the first time and is kept.
//   - Anything else hops: the mod thread posts it and waits, and the game
//     thread answers it inside Window after applying every buffered write
//     before it, so the game sees the mod's calls in the order it made them.
//
// A hop is a handoff on one atomic that the waiting side watches while its
// partner runs, so a window that answers a burst of hops costs each a cache
// line transfer in each direction, not a thread switch. Each thread keeps a
// flag that says it runs: the mod thread sets its own except while it sleeps,
// and the game thread sets its own for the whole of a window. A waiter spins
// only while its partner's flag is set, then waits kPartnerGrace for a change
// already on its way, and then sleeps. A hop issued outside a window thus
// sleeps at once instead of spinning for a game thread that is not there. The
// spin has no limit while the flag stays set, so a partner the system
// preempted keeps its waiter spinning; a progress counter that the spinner
// watches would end that, and is not built.
// Calls from the game thread itself, which run Start and a decision, go
// straight to the game after the buffer is applied.
class ModThread {
 public:
  // Load compiles module, runs its initialization on the calling thread and
  // returns the mod, not yet started. game_call answers every call the mod
  // makes, and runs only on the game thread. runtime must outlive the mod.
  [[nodiscard]] static std::expected<std::unique_ptr<ModThread>, std::string> Load(
      Runtime& runtime, std::span<const uint8_t> module, const Limits& limits,
      Instance::HostCall game_call, ModThreadSettings settings = {});

  // Stop is called by the destructor.
  ~ModThread();
  ModThread(const ModThread&) = delete;
  ModThread& operator=(const ModThread&) = delete;

  // Begin delivers the Start event on the calling thread, within the limits'
  // start budget, so the mod's setup answers every call directly, and then
  // starts the mod thread. A failed start returns its reason and starts
  // nothing. Call it once, before any other method but Failure.
  [[nodiscard]] std::expected<Reply, std::string> Begin(const Call& start);

  // Post queues an event for the mod thread, which delivers events in the order
  // posted. Events about to run when a Frame is posted are not reordered, but a
  // Frame already queued is dropped for the newer one, which goes to the back:
  // a mod that falls behind sees one frame, with the latest time, instead of
  // a backlog. Its Completion appears once the event ends.
  void Post(Call call);

  // Decide delivers call to the mod now, on the game thread, and returns the
  // outcome, when the mod is idle and its event queue is empty, so a decision
  // never overtakes a queued event. It also refuses inside a Window, while a
  // buffer is being applied and during another Decide. It applies the buffer
  // first, so the mod sees the game its earlier writes made. Otherwise it
  // returns nullopt at once and the caller takes its own default, such as an
  // unchanged hit, an unclaimed command or a busy answer. A mod that fails here
  // fails as it does anywhere: see Failure.
  [[nodiscard]] std::optional<Completion> Decide(const Call& call);

  // Window runs the mod for one tick, after the engine's frame. It applies the
  // buffered writes, rebuilds the snapshot, posts frame if there is one, and
  // answers the mod's hops, applying the buffer before each, until the mod is
  // idle with nothing queued or deadline passes. A hop already waiting when the
  // window opens is answered whatever the deadline, so a mod that hops
  // forever still advances one call a window. A mod still running at the
  // deadline goes on, and its next hop waits for the next window.
  //
  // Writes the mod makes in the last stretch of a window, after its final hop,
  // reach the game in the next window, one tick late. Applying the buffer once
  // more where the window closes quiet would remove the lag; it is left out
  // until a mod's measured need justifies the extra applying pass.
  WindowStats Window(std::chrono::steady_clock::time_point deadline,
                     std::optional<Call> frame = std::nullopt);

  // Completed returns the events that finished since the last call, in order,
  // other than those Decide delivered.
  [[nodiscard]] std::vector<Completion> Completed();

  // Stop interrupts a running call, answers a waiting hop with an error, and
  // joins the thread. The flag and the wake-up of a hop waiter happen under one
  // lock, so a hop cannot start waiting between them. Buffered writes the last
  // window did not apply are dropped, so a caller that wants them runs one more
  // Window first. It is safe to call twice.
  void Stop();

  // Failure returns why the mod failed, or nullopt while it runs. It is the
  // one method any thread may call.
  [[nodiscard]] std::optional<std::string> Failure() const;

 private:
  // Waits counts how the threads' waits ended, for the window's counters.
  struct Waits {
    std::atomic<uint64_t> spins = 0;
    std::atomic<uint64_t> sleeps = 0;
  };

  // Bell hands one thread a change that another is waiting for. The waiter
  // checks a condition on atomics, spins for a while, and then sleeps; the
  // notifier touches the condition variable only when somebody sleeps.
  class Bell {
   public:
    explicit Bell(Waits& waits) : waits_(waits) {}

    // Wait returns true once ready() holds, or false at deadline. When spin
    // is set it watches ready while partner is set and for kPartnerGrace after
    // it clears, and then sleeps; self, when given, is cleared while the
    // waiter sleeps and set when it wakes. ready reads state the notifier
    // changed before Notify, with sequentially consistent atomics, so a change
    // made while the waiter goes to sleep is never lost.
    template <typename Ready>
    bool Wait(Ready ready, bool spin, const std::atomic<bool>& partner, std::atomic<bool>* self,
              std::optional<std::chrono::steady_clock::time_point> deadline);

    // Sleep waits on the condition variable at once, and is not counted: it is
    // how an idle mod thread waits for its next event.
    template <typename Ready>
    bool Sleep(Ready ready, std::optional<std::chrono::steady_clock::time_point> deadline);

    // Notify wakes the waiter if it sleeps; call it after changing what the
    // waiter's condition reads with a sequentially consistent store. The waiter
    // registers as a sleeper before it checks the condition under the lock, so
    // either it sees the change or Notify sees it, and then wakes it under the
    // lock.
    void Notify();

    // Set runs change while holding the lock the waiter checks its condition
    // under, then wakes the waiter. A change made this way cannot fall between
    // the waiter's check and its sleep, whatever the memory order of the
    // state it changes.
    template <typename Change>
    void Set(Change change);

   private:
    Waits& waits_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<int> sleepers_ = 0;
  };

  // Hop is the mod thread's one outstanding request to the game thread.
  enum class HopState { kNone, kPosted, kAnswered };

  // Entry is one replayed read: the call, the game's answer when the window
  // began, and whether the mod used it since, which keeps it for the next
  // window.
  struct Entry {
    Call call;
    Reply reply;
    mutable std::atomic<bool> used = false;
  };

  // Snapshot is the game as the window began, for the reads the mod made in the
  // window before. It is immutable once published, except for the used flags.
  // epoch counts the calls that changed the game and had run when it was taken.
  struct Snapshot {
    std::unordered_map<std::string, Entry> entries;
    uint64_t epoch = 0;
  };

  ModThread(Runtime& runtime, ModThreadSettings settings, Instance::HostCall game_call);

  // Route answers a call from the mod, on whichever thread runs the mod.
  Reply Route(const Call& call);
  // OnModThread is true on the mod thread.
  bool OnModThread() const;
  // Run is the mod thread's loop.
  void Run();
  // TakeEvent pops the next event if the mod may run one.
  bool TakeEvent(Call& call);
  // Deliver runs one event in the instance, on the calling thread.
  Completion Deliver(const Call& call);

  // BufferWrite appends call to the buffer, or returns false when it is full.
  bool BufferWrite(const Call& call);
  // ReadSnapshot answers a read from the snapshot, or returns nullopt after
  // noting the miss for the next window to replay.
  std::optional<Reply> ReadSnapshot(const Call& call);

  // LatestSnapshot returns the latest snapshot, or null before the first window
  // built one.
  std::shared_ptr<const Snapshot> LatestSnapshot() const {
    const std::lock_guard lock(snapshot_mutex_);
    return snapshot_;
  }

  // Hop asks the game thread to answer call and waits for its reply. A call
  // that changes the game counts toward the epoch that strict mode compares.
  Reply Hop(const Call& call, bool changes);

  // ApplyBuffer applies the buffered writes in order, on the game thread, and
  // returns how many it applied. It does nothing while it already runs.
  void ApplyBuffer(WindowStats* stats);
  // Replay builds the next snapshot from the entries the mod used and the
  // reads it missed, on the game thread, and publishes it.
  void Replay(WindowStats* stats);
  // Serve answers the hop the mod posted, on the game thread.
  void Serve(WindowStats* stats);
  // CountReads adds the reads' counters since the last window to stats.
  void CountReads(WindowStats* stats);
  // Key returns the identity of a read: its method and its request bytes.
  static std::string Key(const Call& call);

  Runtime& runtime_;
  const ModThreadSettings settings_;
  Instance::HostCall game_call_;
  std::unique_ptr<Instance> instance_;

  // The game thread's own state. It is touched only there.
  // applying_ is true while the buffer is being applied; a decision then is
  // refused, since the game is in the middle of the mod's commands.
  bool applying_ = false;
  // borrowed_ is true while Decide runs an event on the game thread.
  std::atomic<bool> borrowed_ = false;
  // in_window_ is true inside Window, which refuses a Decide: the game is in
  // the middle of the tick's own sequence.
  bool in_window_ = false;
  // executed_ counts the changing calls the game thread has run for the mod,
  // buffered writes and hops alike. The snapshot's epoch is its value when the
  // snapshot was taken.
  uint64_t executed_ = 0;

  // Counters are the outcomes counted since the mod began; reported_ holds
  // them as of the last window.
  struct Counters {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t stale = 0;
    uint64_t spins = 0;
    uint64_t sleeps = 0;
  };

  Counters reported_;
  std::vector<Call> applying_batch_;
  bool started_ = false;

  // The mod thread's own state. It is touched only there, or on the game
  // thread while the mod is idle and Decide or Begin runs the instance.
  // issued_ counts the changing calls the mod has made, to compare with a
  // snapshot's epoch.
  uint64_t issued_ = 0;
  std::unordered_map<std::string, Reply> immutable_;

  // The handoff. hop_call_, hop_changes_ and hop_reply_ belong to whichever side
  // hop_ says may touch them.
  std::atomic<HopState> hop_ = HopState::kNone;
  Call hop_call_;
  bool hop_changes_ = false;
  Reply hop_reply_;
  // to_mod_ wakes the mod thread for an event, an answer or a stop. to_game_
  // wakes the game thread, inside a window, for a hop, the end of the mod's
  // last event or the mod's exit.
  Waits waits_;
  Bell to_mod_{waits_};
  Bell to_game_{waits_};
  // mod_running_ is set while the mod thread runs or spins, and clear while it
  // sleeps; game_running_ is set for the whole of a window.
  std::atomic<bool> mod_running_ = false;
  std::atomic<bool> game_running_ = false;
  // pending_ counts the events posted and not yet finished.
  std::atomic<uint32_t> pending_ = 0;
  std::atomic<bool> stopping_ = false;
  // done_ is set by the mod thread as it exits, after a failure or a stop, so
  // a wait on it ends.
  std::atomic<bool> done_ = false;
  // snapshot_ is the latest snapshot, replaced whole by the game thread under
  // snapshot_mutex_ once a window, and held for a pointer copy by the mod
  // thread's reads. std::atomic<std::shared_ptr> is not available in every
  // standard library the host builds with.
  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const Snapshot> snapshot_;
  // hits_, misses_ and stale_ count the mod's reads by outcome.
  std::atomic<uint64_t> hits_ = 0;
  std::atomic<uint64_t> misses_ = 0;
  std::atomic<uint64_t> stale_ = 0;

  // mu_ guards what both threads hand each other: the event queue, the write
  // buffer, the reads missed since the last window, the finished events and the
  // failure. queued_ mirrors the queue's size for the mod thread's wait.
  mutable std::mutex mu_;
  std::deque<Call> queue_;
  std::atomic<size_t> queued_ = 0;
  std::vector<Call> buffer_;
  // buffered_ mirrors the buffer's size, so a hop with nothing buffered skips
  // the lock.
  std::atomic<size_t> buffered_ = 0;
  std::vector<Call> missed_;
  std::unordered_set<std::string> missed_keys_;
  std::vector<Completion> completed_;
  std::optional<std::string> failure_;

  // thread_ is last, so every member it uses outlives it.
  std::thread thread_;
};

}  // namespace modlock::wasm
