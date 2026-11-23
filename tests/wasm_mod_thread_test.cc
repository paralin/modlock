// Contract tests for ModThread, which runs a mod's events on a thread of its
// own while every call the mod makes into the game runs on the game thread.
// The mods here are QuickJS scripts, and the game is a recording host that
// notes any call made from another thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/mod_thread.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using modlock::wasm::Call;
using modlock::wasm::CallClass;
using modlock::wasm::ClassOf;
using modlock::wasm::Completion;
using modlock::wasm::FrameEvent;
using modlock::wasm::Instance;
using modlock::wasm::Limits;
using modlock::wasm::ModThread;
using modlock::wasm::ModThreadSettings;
using modlock::wasm::Reply;
using modlock::wasm::Runtime;
using modlock::wasm::StartEvent;
using modlock::wasm::WindowStats;

// ReadFile returns the bytes of a built module.
std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// SharedRuntime compiles the QuickJS module once for all the tests.
Runtime& SharedRuntime() {
  static Runtime runtime;
  return runtime;
}

// Event returns a call delivering request to the mod's method.
Call Event(std::string method, const google::protobuf::MessageLite& request) {
  Call call;
  call.set_method(std::move(method));
  call.set_request(request.SerializeAsString());
  return call;
}

Call Event(std::string method) {
  Call call;
  call.set_method(std::move(method));
  return call;
}

// FrameAt returns a Frame event at tick.
Call FrameAt(uint64_t tick) {
  FrameEvent frame;
  frame.set_tick(tick);
  return Event("Frame", frame);
}

// Game is the engine's side: a counter that WriteField raises and ReadField
// returns, and the log of every call that changed it or the world. Its calls
// belong to the thread that made it, and any other thread trips off_thread.
struct Game {
  const std::thread::id thread = std::this_thread::get_id();
  std::atomic<bool> off_thread = false;
  int64_t value = 0;
  bool record = true;
  std::vector<std::string> calls;
  std::vector<Clock::time_point> hops;

  Instance::HostCall Answer() {
    return [this](const Call& call) {
      if (std::this_thread::get_id() != thread) {
        off_thread = true;
      }
      Reply reply;
      if (call.method() == "ReadField") {
        reply.set_response(std::to_string(value));
        return reply;
      }
      if (call.method() == "WriteField") {
        ++value;
      }
      if (call.method() == "CreateModel") {
        hops.push_back(Clock::now());
        reply.set_response("model");
      }
      if (record) {
        calls.push_back(call.method() + (call.request().empty() ? "" : ":" + call.request()));
      }
      return reply;
    };
  }
};

// kPrelude gives a script the host calls over protobuf bytes: call(method,
// request) returns {error, response}, and an event arrives as a Call whose
// handlers[method] answers. Lengths stay under 128, so each fits one byte.
const char kPrelude[] = R"(
const enc = new TextEncoder(), dec = new TextDecoder();
const text = (s) => enc.encode(s);
const call = (method, request = new Uint8Array(0)) => {
  const name = text(method);
  const out = new Uint8Array(4 + name.length + request.length);
  out[0] = 0x0a; out[1] = name.length; out.set(name, 2);
  out[2 + name.length] = 0x12; out[3 + name.length] = request.length;
  out.set(request, 4 + name.length);
  const bytes = __modlock.hostCall(out);
  let o = 0, error = '', response = new Uint8Array(0);
  while (o < bytes.length) {
    const tag = bytes[o++], n = bytes[o++], field = bytes.subarray(o, o + n);
    o += n;
    if (tag === 0x0a) error = dec.decode(field); else response = field;
  }
  return {error, response};
};
const read = (method) => dec.decode(call(method).response);
const reply = (bytes) => {
  const out = new Uint8Array(2 + bytes.length);
  out[0] = 0x12; out[1] = bytes.length; out.set(bytes, 2);
  return out;
};
const busy = (ms) => { const end = Date.now() + ms; while (Date.now() < end) {} };
)";

// kDispatch routes each event to handlers[method] and returns its reply.
const char kDispatch[] = R"(
__modlock.event = (ev) => {
  const n = ev[1];
  const method = dec.decode(ev.subarray(2, 2 + n));
  const at = 2 + n;
  const request = at < ev.length ? ev.subarray(at + 2, at + 2 + ev[at + 1]) : new Uint8Array(0);
  const handler = handlers[method];
  return (handler && handler(request)) || new Uint8Array(0);
};
)";

// Rig is one loaded mod and its game.
class Rig {
 public:
  Game game;
  std::unique_ptr<ModThread> mod;

  // Up loads a script made of handlers and starts it.
  ::testing::AssertionResult Up(const std::string& handlers, ModThreadSettings settings = {}) {
    auto loaded =
        ModThread::Load(SharedRuntime(), ReadFile(QUICKJS_WASM), Limits{}, game.Answer(), settings);
    if (!loaded) {
      return ::testing::AssertionFailure() << loaded.error();
    }
    mod = std::move(*loaded);
    StartEvent start;
    start.set_source(std::string(kPrelude) + handlers + kDispatch);
    auto started = mod->Begin(Event("Start", start));
    if (!started) {
      return ::testing::AssertionFailure() << started.error();
    }
    return ::testing::AssertionSuccess();
  }

  // Tick runs one window that posts a Frame and waits for the mod up to limit.
  WindowStats Tick(std::chrono::milliseconds limit = 5000ms) {
    return mod->Window(Clock::now() + limit, FrameAt(++tick_));
  }

  // Settle runs windows with no new event until the mod is idle.
  WindowStats Settle(std::chrono::milliseconds limit = 5000ms) {
    WindowStats stats;
    const auto end = Clock::now() + limit;
    do {
      stats = mod->Window(Clock::now() + 20ms);
    } while (!stats.idle && Clock::now() < end);
    return stats;
  }

 private:
  uint64_t tick_ = 0;
};

// Text returns the response of a finished event.
std::string Text(const Completion& done) {
  return done.reply ? done.reply->response() : "failed: " + done.reply.error();
}

TEST(ModThread, ClassTable) {
  EXPECT_EQ(ClassOf("ReadField"), CallClass::kRead);
  EXPECT_EQ(ClassOf("WriteField"), CallClass::kWrite);
  EXPECT_EQ(ClassOf("ReadFile"), CallClass::kImmutable);
  EXPECT_EQ(ClassOf("CreateModel"), CallClass::kHop);
  EXPECT_EQ(ClassOf("NotAMethod"), CallClass::kHop);
  EXPECT_EQ(ClassOf("WriteFile"), CallClass::kHop);
}

TEST(ModThread, RepeatedReadHitsTheSnapshotInTheNextWindow) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {Frame: () => reply(text(read('ReadField')))};"));

  // The first window has no snapshot, so the read hops and is remembered.
  const auto first = rig.Tick();
  EXPECT_TRUE(first.idle);
  EXPECT_EQ(first.hops, 1U);
  EXPECT_EQ(first.misses, 1U);
  EXPECT_EQ(first.hits, 0U);

  // The second replays it before the mod runs, and the mod never hops.
  const auto second = rig.Tick();
  EXPECT_TRUE(second.idle);
  EXPECT_EQ(second.replayed, 1U);
  EXPECT_EQ(second.hits, 1U);
  EXPECT_EQ(second.hops, 0U);
  EXPECT_FALSE(rig.game.off_thread);

  const auto done = rig.mod->Completed();
  ASSERT_EQ(done.size(), 2U);
  EXPECT_EQ(Text(done[1]), "0");
}

TEST(ModThread, WritesAndHopsReachTheGameInIssueOrder) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
let ran = false;
const handlers = {Frame: () => {
  if (ran) return;
  ran = true;
  call('Chat', text('a'));
  call('Chat', text('b'));
  call('CreateModel');
  call('Chat', text('c'));
}};)"));

  // The hop applies the writes before it, in order, and answers; the write
  // after it waits in the buffer.
  rig.Tick();
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"Chat:a", "Chat:b", "CreateModel"}));

  // The next window applies it, one tick late.
  rig.Tick();
  EXPECT_EQ(rig.game.calls,
            (std::vector<std::string>{"Chat:a", "Chat:b", "CreateModel", "Chat:c"}));
  EXPECT_FALSE(rig.game.off_thread);
}

TEST(ModThread, StrictReadSeesTheModsOwnWrite) {
  const std::string script = R"(
const handlers = {Frame: () => {
  const before = read('ReadField');
  call('WriteField');
  const after = read('ReadField');
  return reply(text(before + ',' + after));
}};)";

  // Once the snapshot holds the read, a read after a write still hops, so the
  // mod sees its own write.
  Rig strict;
  ASSERT_TRUE(strict.Up(script));
  strict.Tick();
  const auto stats = strict.Tick();
  EXPECT_EQ(stats.stale, 1U);
  EXPECT_EQ(stats.hits, 1U);
  EXPECT_EQ(stats.hops, 1U);
  auto done = strict.mod->Completed();
  ASSERT_EQ(done.size(), 2U);
  EXPECT_EQ(Text(done[1]), "1,2");

  // Without strict mode the second read repeats the window's start.
  ModThreadSettings relaxed;
  relaxed.strict = false;
  Rig loose;
  ASSERT_TRUE(loose.Up(script, relaxed));
  loose.Tick();
  const auto later = loose.Tick();
  EXPECT_EQ(later.stale, 0U);
  EXPECT_EQ(later.hops, 0U);
  done = loose.mod->Completed();
  ASSERT_EQ(done.size(), 2U);
  EXPECT_EQ(Text(done[1]), "1,1");
}

TEST(ModThread, AnImmutableReadHopsOnce) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
const handlers = {Frame: () => { call('ReadFile'); call('ReadFile'); }};)"));
  rig.Tick();
  rig.Tick();
  EXPECT_EQ(std::count(rig.game.calls.begin(), rig.game.calls.end(), "ReadFile"), 1);
}

TEST(ModThread, DecideBorrowsAnIdleModAndRefusesABusyOne) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
const handlers = {
  Frame: () => { call('Chat', text('f')); },
  Hop: () => { call('CreateModel'); },
  Decision: () => { call('Log', text('d')); return reply(text('ok')); },
};)"));

  // An idle mod answers in place, after the writes it made earlier, and its
  // own calls run on the game thread.
  rig.Tick();
  const auto idle = rig.mod->Decide(Event("Decision"));
  ASSERT_TRUE(idle);
  EXPECT_EQ(Text(*idle), "ok");
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"Chat:f", "Log:d"}));
  EXPECT_FALSE(rig.game.off_thread);

  // An event just posted is ahead of the decision, which must not overtake it.
  rig.mod->Post(Event("Hop"));
  EXPECT_FALSE(rig.mod->Decide(Event("Decision")));

  // A mod waiting on a hop is busy.
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(rig.mod->Decide(Event("Decision")));

  // Once the window answers it, the mod is idle again.
  EXPECT_TRUE(rig.Settle().idle);
  EXPECT_TRUE(rig.mod->Decide(Event("Decision")));
  EXPECT_FALSE(rig.game.off_thread);
}

TEST(ModThread, StopInterruptsARunawayEvent) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {Loop: () => { for (;;) {} }};"));
  rig.mod->Post(Event("Loop"));
  std::this_thread::sleep_for(50ms);

  const auto began = Clock::now();
  rig.mod->Stop();
  EXPECT_LT(Clock::now() - began, 500ms);
  EXPECT_TRUE(rig.mod->Failure());
}

TEST(ModThread, StopWakesAHopThatWaits) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {Hop: () => { call('CreateModel'); }};"));
  rig.mod->Post(Event("Hop"));
  std::this_thread::sleep_for(100ms);

  // Nobody serves the hop, so the stop has to wake it.
  const auto began = Clock::now();
  rig.mod->Stop();
  EXPECT_LT(Clock::now() - began, 500ms);
  EXPECT_TRUE(rig.game.calls.empty());
}

TEST(ModThread, StopTwiceIsSafe) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {};"));
  rig.mod->Stop();
  rig.mod->Stop();
}

TEST(ModThread, WindowEndsNearItsDeadlineWhileTheModComputes) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {Frame: () => { busy(50); return reply(text('done')); }};"));

  // The window leaves while the mod still runs.
  const auto began = Clock::now();
  const auto stats = rig.Tick(10ms);
  const auto took = Clock::now() - began;
  EXPECT_FALSE(stats.idle);
  EXPECT_GE(took, 10ms);
  EXPECT_LT(took, 40ms);
  EXPECT_TRUE(rig.mod->Completed().empty());

  // A later window finds it finished.
  EXPECT_TRUE(rig.Settle().idle);
  const auto done = rig.mod->Completed();
  ASSERT_EQ(done.size(), 1U);
  EXPECT_EQ(Text(done[0]), "done");
}

TEST(ModThread, AHopWaitingAtTheWindowsOpeningIsServedPastItsDeadline) {
  Rig rig;
  ASSERT_TRUE(rig.Up("const handlers = {Hop: () => { call('CreateModel'); }};"));
  rig.mod->Post(Event("Hop"));
  std::this_thread::sleep_for(50ms);

  // The deadline has passed before the window opens, and the hop is answered.
  const auto stats = rig.mod->Window(Clock::now() - 1ms);
  EXPECT_EQ(stats.hops, 1U);
  EXPECT_TRUE(rig.Settle().idle);
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"CreateModel"}));
}

TEST(ModThread, QueuedFramesCoalesce) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
const handlers = {
  Hop: () => { call('CreateModel'); },
  Ev: () => {},
  Frame: (request) => reply(request.subarray(1, 2)),
};)"));

  // Hold the mod on a hop while the engine posts a Frame, an event and a
  // newer Frame: the old Frame goes, and the event keeps its place.
  rig.mod->Post(Event("Hop"));
  std::this_thread::sleep_for(50ms);
  rig.mod->Post(FrameAt(1));
  rig.mod->Post(Event("Ev"));
  rig.mod->Post(FrameAt(2));
  EXPECT_TRUE(rig.Settle().idle);

  const auto done = rig.mod->Completed();
  ASSERT_EQ(done.size(), 3U);
  EXPECT_EQ(done[0].method, "Hop");
  EXPECT_EQ(done[1].method, "Ev");
  EXPECT_EQ(done[2].method, "Frame");
  EXPECT_EQ(Text(done[2]), std::string(1, '\x02'));
}

// Percentile returns the p-th percentile of sorted samples.
int64_t Percentile(const std::vector<int64_t>& sorted, double p) {
  return sorted[static_cast<size_t>(p * (sorted.size() - 1))];
}

// MeasureHops runs the script's Bench event, which makes count hops, and
// prints the time between consecutive hops at the game: the cost of a hop
// plus the script's own work per call.
void MeasureHops(const char* label, ModThreadSettings settings, bool direct) {
  constexpr int kCount = 2000;
  Rig rig;
  rig.game.record = false;
  ASSERT_TRUE(rig.Up("const handlers = {Bench: () => { for (let i = 0; i < " +
                         std::to_string(kCount) + "; ++i) call('CreateModel'); }};",
                     settings));
  WindowStats stats;
  if (direct) {
    ASSERT_TRUE(rig.mod->Decide(Event("Bench")));
  } else {
    stats = rig.mod->Window(Clock::now() + 30s, Event("Bench"));
    ASSERT_TRUE(stats.idle);
    ASSERT_EQ(stats.hops, static_cast<uint32_t>(kCount));
  }
  ASSERT_EQ(rig.game.hops.size(), static_cast<size_t>(kCount));
  EXPECT_FALSE(rig.game.off_thread);

  std::vector<int64_t> gaps;
  for (size_t i = 1; i < rig.game.hops.size(); ++i) {
    gaps.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(rig.game.hops[i] -
                                                                        rig.game.hops[i - 1])
                       .count());
  }
  std::sort(gaps.begin(), gaps.end());
  std::printf(
      "%-28s median %7lld ns  p99 %8lld ns  spins %llu  sleeps %llu\n", label,
      static_cast<long long>(Percentile(gaps, 0.5)), static_cast<long long>(Percentile(gaps, 0.99)),
      static_cast<unsigned long long>(stats.spins), static_cast<unsigned long long>(stats.sleeps));
}

TEST(ModThread, HopRoundTripBenchmark) {
  ModThreadSettings sleeping;
  sleeping.spin = 0us;
  MeasureHops("direct on the game thread", {}, true);
  MeasureHops("hop, mod spins 50 us", {}, false);
  MeasureHops("hop, mod sleeps at once", sleeping, false);
}

}  // namespace
