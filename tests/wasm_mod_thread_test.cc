// Contract tests for ModThread, which runs a mod's events on a thread of its
// own while every call the mod makes into the game runs on the game thread.
// The mods here are QuickJS scripts, and the game is a recording host that
// notes any call made from another thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/mod_thread.h"
#include "wasm/sandbox.h"

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
using modlock::wasm::Sandbox;
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
  // on_chat runs on the game's thread inside each Chat, as the game does when
  // one of the mod's commands provokes an event.
  std::function<void()> on_chat;

  Instance::HostCall Answer() {
    return [this](const Call& call) {
      if (std::this_thread::get_id() != thread) {
        off_thread = true;
      }
      Reply reply;
      if (call.method() == "Chat" && on_chat) {
        on_chat();
      }
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
// handlers[method] answers. A length may take several bytes.
const char kPrelude[] = R"(
const enc = new TextEncoder(), dec = new TextDecoder();
const text = (s) => enc.encode(s);
const varint = (n) => {
  const out = [];
  for (; n >= 128; n = Math.floor(n / 128)) out.push((n % 128) + 128);
  out.push(n);
  return out;
};
const concat = (parts) => {
  const out = [];
  for (const part of parts) for (const byte of part) out.push(byte);
  return Uint8Array.from(out);
};
const field = (tag, bytes) => concat([[tag], varint(bytes.length), bytes]);
const readVarint = (bytes, cursor) => {
  let n = 0, scale = 1;
  for (;;) {
    const byte = bytes[cursor.at++];
    n += (byte & 127) * scale;
    if (byte < 128) return n;
    scale *= 128;
  }
};
// fields returns the [number, value] pairs of an encoded message, where a
// value is a number or the bytes of a length-delimited field.
const fields = (bytes) => {
  const out = [], cursor = {at: 0};
  while (cursor.at < bytes.length) {
    const tag = bytes[cursor.at++];
    if ((tag & 7) === 0) {
      out.push([tag >> 3, readVarint(bytes, cursor)]);
    } else {
      const n = readVarint(bytes, cursor);
      out.push([tag >> 3, bytes.subarray(cursor.at, cursor.at + n)]);
      cursor.at += n;
    }
  }
  return out;
};
const get = (list, number) => (list.find((f) => f[0] === number) || [])[1];
const call = (method, request = new Uint8Array(0)) => {
  const bytes = __modlock.hostCall(concat([field(0x0a, text(method)), field(0x12, request)]));
  const reply = fields(bytes);
  const error = get(reply, 1), response = get(reply, 2);
  return {error: error ? dec.decode(error) : '', response: response || new Uint8Array(0)};
};
const read = (method) => dec.decode(call(method).response);
const reply = (bytes) => field(0x12, bytes);
const busy = (ms) => { const end = Date.now() + ms; while (Date.now() < end) {} };
)";

// kFutures gives a script submit(calls), which sends [method, request] pairs
// in one Submit and returns {error, ids}, and completed(event), which lists
// "id:error:response" for each call a Completed event delivers.
const char kFutures[] = R"(
const submit = (calls) => {
  const request = concat(calls.map(([method, body]) =>
    field(0x0a, concat([field(0x0a, text(method)), field(0x12, body || new Uint8Array(0))]))));
  const {error, response} = call('Submit', request);
  const ids = [];
  for (const [number, packed] of fields(response)) {
    const cursor = {at: 0};
    while (number === 1 && cursor.at < packed.length) ids.push(readVarint(packed, cursor));
  }
  return {error, ids};
};
const completed = (event) => fields(event).map(([, done]) => {
  const parts = fields(done), answer = fields(get(parts, 2) || new Uint8Array(0));
  const error = get(answer, 1), response = get(answer, 2);
  return get(parts, 1) + ':' + (error ? dec.decode(error) : '') + ':' + (response ? dec.decode(response) : '');
});
)";

// kDispatch routes each event to handlers[method] and returns its reply.
const char kDispatch[] = R"(
__modlock.event = (ev) => {
  const sent = fields(ev);
  const method = dec.decode(get(sent, 1));
  const request = get(sent, 2) || new Uint8Array(0);
  const handler = handlers[method];
  return (handler && handler(request)) || new Uint8Array(0);
};
)";

// Rig is one loaded mod and its game.
class Rig {
 public:
  Game game;
  std::unique_ptr<ModThread> mod;

  // Up loads a script made of handlers and starts it. A post queues unless the
  // settings say otherwise, so a test can hold the mod busy with one.
  ::testing::AssertionResult Up(const std::string& handlers,
                                ModThreadSettings settings = {.posts_in_place = false}) {
    auto loaded =
        ModThread::Load(SharedRuntime(), ReadFile(QUICKJS_WASM), Limits{}, game.Answer(), settings);
    if (!loaded) {
      return ::testing::AssertionFailure() << loaded.error();
    }
    mod = std::move(*loaded);
    StartEvent start;
    start.set_source(std::string(kPrelude) + kFutures + handlers + kDispatch);
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

  // A write whose refusal the mod can act on hops, so it hears the game's
  // answer, as a missing modifier definition is.
  EXPECT_EQ(ClassOf("GiveModifier"), CallClass::kHop);
  EXPECT_EQ(ClassOf("GiveEntityModifier"), CallClass::kHop);
  EXPECT_EQ(ClassOf("Chat"), CallClass::kWrite);
}

TEST(ModThread, ADecisionDuringTheBufferIsCountedApart) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
let ran = false;
const handlers = {
  Frame: () => {
    if (ran) return;
    ran = true;
    call('Chat', text('a'));
  },
  Decision: () => { return reply(text('ok')); },
};)"));

  // The game asks for a decision from inside the write the mod made, which
  // runs in the next window, while the buffer applies.
  std::optional<Completion> asked;
  rig.game.on_chat = [&] {
    asked = rig.mod->Decide(Event("Decision"));
  };
  rig.Tick();
  EXPECT_FALSE(asked.has_value());
  rig.Tick();
  EXPECT_FALSE(asked.has_value());
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"Chat:a"}));

  // The refusal is the apply's own, and not the mod's busyness or a window's.
  const auto stats = rig.Tick();
  ASSERT_EQ(stats.decisions.size(), 1U);
  EXPECT_EQ(stats.decisions[0].method, "Decision");
  EXPECT_EQ(stats.decisions[0].answered, 0U);
  EXPECT_EQ(stats.decisions[0].defaulted, 0U);
  EXPECT_EQ(stats.decisions[0].applying, 1U);
  EXPECT_FALSE(rig.game.off_thread);
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

TEST(ModThread, AReadStaysInTheReplaySetForReplayFramesWindows) {
  // The mod reads ReadField on its first frame and again on the frame named by
  // the script's gap, and none between.
  const auto script = [](int again) {
    return "let frame = 0;\nconst handlers = {Frame: () => {\n  ++frame;\n  if (frame === 1 || "
           "frame === " +
           std::to_string(again) + ") read('ReadField');\n}};";
  };

  // One window of memory keeps what the last window read, so a read after a
  // quiet frame misses again.
  Rig forgetful;
  ASSERT_TRUE(forgetful.Up(script(4)));
  const auto first = forgetful.Tick();
  EXPECT_EQ(first.misses, 1U);
  const auto top = first.missed.Top(8);
  ASSERT_EQ(top.size(), 1U);
  EXPECT_EQ(top[0].method, "ReadField");
  EXPECT_EQ(top[0].misses, 1U);
  EXPECT_EQ(top[0].keys, 1U);
  forgetful.Tick();
  forgetful.Tick();
  const auto again = forgetful.Tick();
  EXPECT_EQ(again.misses, 1U);
  EXPECT_EQ(again.hits, 0U);

  // Three windows keep it through two quiet frames, but not three.
  Rig keeps;
  ASSERT_TRUE(keeps.Up(script(4), {.posts_in_place = false, .replay_frames = 3}));
  keeps.Tick();
  keeps.Tick();
  keeps.Tick();
  const auto kept = keeps.Tick();
  EXPECT_EQ(kept.hits, 1U);
  EXPECT_EQ(kept.misses, 0U);
  EXPECT_EQ(kept.replayed, 1U);

  Rig lapses;
  ASSERT_TRUE(lapses.Up(script(5), {.posts_in_place = false, .replay_frames = 3}));
  for (int i = 0; i < 4; ++i) {
    lapses.Tick();
  }
  const auto lapsed = lapses.Tick();
  EXPECT_EQ(lapsed.hits, 0U);
  EXPECT_EQ(lapsed.misses, 1U);
  EXPECT_FALSE(forgetful.game.off_thread);
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

TEST(ModThread, PostRunsInPlaceWhileTheModIsIdle) {
  Rig rig;
  ASSERT_TRUE(rig.Up(R"(
const handlers = {
  Ev: () => { call('Log', text('e')); },
  Frame: () => {},
  Decision: () => { return reply(text('ok')); },
};)",
                     {.posts_in_place = true}));

  // An idle mod runs a posted event on the game thread at once, so a decision
  // that follows it finds the mod idle.
  rig.mod->Post(Event("Ev"));
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"Log:e"}));
  ASSERT_TRUE(rig.mod->Decide(Event("Decision")));
  auto stats = rig.mod->Window(Clock::now() + 1s);
  EXPECT_EQ(stats.in_place, 1U);
  EXPECT_EQ(stats.queued, 0U);
  ASSERT_EQ(stats.decisions.size(), 1U);
  EXPECT_EQ(stats.decisions[0].method, "Decision");
  EXPECT_EQ(stats.decisions[0].answered, 1U);
  EXPECT_EQ(stats.decisions[0].defaulted, 0U);
  const auto done = rig.mod->Completed();
  ASSERT_EQ(done.size(), 1U);
  EXPECT_EQ(done[0].method, "Ev");

  // A Frame always queues, and an event posted behind a queued one waits its
  // turn, so the decision finds the mod busy and takes its default.
  rig.mod->Post(FrameAt(1));
  rig.mod->Post(Event("Ev"));
  EXPECT_FALSE(rig.mod->Decide(Event("Decision")));
  stats = rig.mod->Window(Clock::now() + 5s);
  EXPECT_TRUE(stats.idle);
  EXPECT_EQ(stats.in_place, 0U);
  EXPECT_EQ(stats.queued, 1U);
  ASSERT_EQ(stats.decisions.size(), 1U);
  EXPECT_EQ(stats.decisions[0].defaulted, 1U);
  EXPECT_EQ(stats.decisions[0].applying, 0U);
  const auto queued = rig.mod->Completed();
  ASSERT_EQ(queued.size(), 2U);
  EXPECT_EQ(queued[0].method, "Frame");
  EXPECT_EQ(queued[1].method, "Ev");
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

// kSubmitting is a mod that submits a read, a write, a read and a hop in one
// Submit, keeps what its Completed events deliver, and reports it.
const char kSubmitting[] = R"(
let ids = [], events = 0, seen = [];
const handlers = {
  Frame: () => {
    if (ids.length) return;
    ids = submit([['ReadField'], ['WriteField'], ['ReadField'], ['CreateModel']]).ids;
  },
  Completed: (request) => { ++events; seen.push(...completed(request)); },
  Report: () => reply(text(ids.join(',') + '|' + events + '|' + seen.join(','))),
  Cap: () => {
    const first = submit(Array.from({length: 255}, () => ['Chat', text('c')]));
    const nested = submit([['Submit']]);
    const second = submit([['Chat', text('c')]]);
    return reply(text(first.error + first.ids.length + '|' + nested.error + nested.ids.length + '|' + second.error));
  },
};)";

constexpr char kReplies[] = "1,2,3,4|1|1::0,2::,3::1,4::model";
constexpr char kCapReport[] = "255|1|the mod already holds 256 submitted calls, and may hold 256";
constexpr char kNestedReply[] = "256:a submitted call cannot submit:";

TEST(Futures, SubmittedCallsReturnInOneCompletedEventOnAThread) {
  Rig rig;
  ASSERT_TRUE(rig.Up(kSubmitting));

  // The calls run in the windows, in the order submitted, and their replies
  // come back together in one event.
  for (int i = 0; i < 6; ++i) {
    rig.Tick();
  }
  EXPECT_TRUE(rig.Settle().idle);
  rig.mod->Post(Event("Report"));
  EXPECT_TRUE(rig.Settle().idle);
  const auto done = rig.mod->Completed();
  ASSERT_FALSE(done.empty());
  EXPECT_EQ(Text(done.back()), kReplies);
  EXPECT_EQ(rig.game.calls, (std::vector<std::string>{"WriteField", "CreateModel"}));
  EXPECT_FALSE(rig.game.off_thread);
}

TEST(Futures, ASubmitPastTheCapAndANestedSubmitAreAnswered) {
  Rig rig;
  ASSERT_TRUE(rig.Up(kSubmitting));
  rig.mod->Post(Event("Cap"));
  EXPECT_TRUE(rig.Settle().idle);
  auto done = rig.mod->Completed();
  ASSERT_FALSE(done.empty());
  EXPECT_EQ(Text(done.back()), kCapReport);

  // The refused nested call is answered with its error, in the same event as
  // the calls that ran.
  for (int i = 0; i < 3; ++i) {
    rig.Tick();
  }
  rig.mod->Post(Event("Report"));
  EXPECT_TRUE(rig.Settle().idle);
  done = rig.mod->Completed();
  ASSERT_FALSE(done.empty());
  EXPECT_NE(Text(done.back()).find(kNestedReply), std::string::npos) << Text(done.back());
}

// LoadInstance loads the QuickJS mod running script as an instance with no
// thread of its own, and starts it.
std::expected<Sandbox, std::string> LoadInstance(Game& game, const std::string& handlers) {
  auto sandbox =
      Sandbox::Load(SharedRuntime(), ReadFile(QUICKJS_WASM), Limits{}, game.Answer(), std::nullopt);
  if (!sandbox) {
    return sandbox;
  }
  StartEvent start;
  start.set_source(std::string(kPrelude) + kFutures + handlers + kDispatch);
  if (auto started = sandbox->Deliver(Event("Start", start)); !started) {
    return std::unexpected(started.error());
  }
  return sandbox;
}

TEST(Futures, SubmittedCallsReturnInOneCompletedEventOnAnInstance) {
  Game game;
  auto sandbox = LoadInstance(game, kSubmitting);
  ASSERT_TRUE(sandbox) << sandbox.error();
  ASSERT_TRUE(sandbox->Deliver(FrameAt(1)));

  // Nothing runs until the tick that flushes, and then every call runs in
  // order and the replies come back as one event.
  EXPECT_TRUE(game.calls.empty());
  const auto event = sandbox->Flush();
  ASSERT_TRUE(event);
  EXPECT_EQ(event->calls_size(), 4);
  EXPECT_EQ(game.calls, (std::vector<std::string>{"WriteField", "CreateModel"}));
  EXPECT_FALSE(sandbox->Flush());

  ASSERT_TRUE(sandbox->Deliver(Event("Completed", *event)));
  const auto report = sandbox->Deliver(Event("Report"));
  ASSERT_TRUE(report);
  EXPECT_EQ(report->response(), kReplies);
}

TEST(Futures, AnInstanceAnswersAPastTheCapAndANestedSubmit) {
  Game game;
  auto sandbox = LoadInstance(game, kSubmitting);
  ASSERT_TRUE(sandbox) << sandbox.error();
  const auto report = sandbox->Deliver(Event("Cap"));
  ASSERT_TRUE(report);
  EXPECT_EQ(report->response(), kCapReport);

  // The replies keep their places until the flush takes them, and taking
  // them frees the places.
  const auto event = sandbox->Flush();
  ASSERT_TRUE(event);
  EXPECT_EQ(event->calls_size(), 256);
  ASSERT_TRUE(sandbox->Deliver(Event("Completed", *event)));
  const auto again = sandbox->Deliver(Event("Cap"));
  ASSERT_TRUE(again);
  EXPECT_EQ(again->response(), kCapReport);
  const auto seen = sandbox->Deliver(Event("Report"));
  ASSERT_TRUE(seen);
  EXPECT_NE(seen->response().find(kNestedReply), std::string::npos) << seen->response();
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
  sleeping.spin = false;
  MeasureHops("direct on the game thread", {}, true);
  MeasureHops("hop, partners spin", {}, false);
  MeasureHops("hop, waits sleep at once", sleeping, false);
}

}  // namespace
