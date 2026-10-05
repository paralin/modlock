// Contract tests for the WebAssembly mod sandbox: the Go, Luau and Python
// example mods answer events through the protobuf boundary, and a trap, a
// runaway loop, an oversized memory or a bad boundary call stops only that
// mod. Mods share compiled modules through the runtime.
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "wasm/instance.h"

namespace {

using modlock::wasm::Call;
using modlock::wasm::ChatRequest;
using modlock::wasm::CommandEvent;
using modlock::wasm::CommandResult;
using modlock::wasm::FrameEvent;
using modlock::wasm::Instance;
using modlock::wasm::Limits;
using modlock::wasm::LogRequest;
using modlock::wasm::Reply;
using modlock::wasm::Runtime;
using modlock::wasm::StartEvent;
using modlock::wasm::StartResult;

// ReadFile returns the bytes of a built module.
std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Zip returns an uncompressed zip of files, by path, as modlock build writes
// a Luau or Python mod's sources. It leaves the checksums zero, which the
// runtime does not read.
std::string Zip(const std::vector<std::pair<std::string, std::string>>& files) {
  // Little appends value as size little-endian bytes; bytes past the fourth
  // are zero.
  const auto little = [](std::string& out, uint32_t value, int size) {
    for (int i = 0; i < size; ++i) out += static_cast<char>(i < 4 ? value >> (8 * i) : 0);
  };

  // Write each file after its local header, and its central directory entry.
  std::string zip, directory;
  for (const auto& [name, data] : files) {
    const auto offset = static_cast<uint32_t>(zip.size());
    little(zip, 0x04034b50, 4);
    little(zip, 20, 2);
    little(zip, 0, 2 + 2 + 4 + 4);
    little(zip, data.size(), 4);
    little(zip, data.size(), 4);
    little(zip, name.size(), 2);
    little(zip, 0, 2);
    zip += name + data;
    little(directory, 0x02014b50, 4);
    little(directory, 20, 2);
    little(directory, 20, 2);
    little(directory, 0, 2 + 2 + 4 + 4);
    little(directory, data.size(), 4);
    little(directory, data.size(), 4);
    little(directory, name.size(), 2);
    little(directory, 0, 2 + 2 + 2 + 2 + 4);
    little(directory, offset, 4);
    directory += name;
  }

  // End with the directory and the record that locates it.
  const auto start = static_cast<uint32_t>(zip.size());
  zip += directory;
  little(zip, 0x06054b50, 4);
  little(zip, 0, 2 + 2);
  little(zip, files.size(), 2);
  little(zip, files.size(), 2);
  little(zip, directory.size(), 4);
  little(zip, start, 4);
  little(zip, 0, 2);
  return zip;
}

// Text returns the contents of a source file.
std::string Text(const std::string& path) {
  const auto bytes = ReadFile(path.c_str());
  return {bytes.begin(), bytes.end()};
}

// Wat compiles WebAssembly text to a module.
std::vector<uint8_t> Wat(std::string_view text) {
  auto module = wasmtime::wat2wasm(text);
  EXPECT_TRUE(module) << module.err().message();
  return module.ok();
}

// Recorder answers host calls and keeps every call.
struct Recorder {
  std::vector<Call> calls;

  Instance::HostCall Answer() {
    return [this](const Call& call) {
      calls.push_back(call);
      return Reply{};
    };
  }
};

// Event returns a call delivering request to the mod's method.
Call Event(std::string method, const google::protobuf::MessageLite& request) {
  Call call;
  call.set_method(std::move(method));
  call.set_request(request.SerializeAsString());
  return call;
}

// Command returns a call delivering a chat command from player.
Call Command(int32_t player, std::string line) {
  CommandEvent event;
  event.set_player(player);
  event.set_line(std::move(line));
  return Event("Command", event);
}

// Parse decodes the request or response bytes of a call as Message.
template <typename Message>
Message Parse(const std::string& bytes) {
  Message message;
  EXPECT_TRUE(message.ParseFromString(bytes));
  return message;
}

TEST(WasmInstance, GoModAnswersEvents) {
  Runtime runtime;
  Recorder recorder;
  auto instance = Instance::Load(runtime, ReadFile(HELLO_GO_WASM), Limits{}, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  // Start asks for frames because the mod registered a frame handler.
  auto started = (*instance)->Deliver(Event("Start", StartEvent{}));
  ASSERT_TRUE(started) << started.error();
  ASSERT_EQ(started->error(), "");
  EXPECT_TRUE(Parse<StartResult>(started->response()).frames());

  // The hello command is claimed and greets its player.
  auto hello = (*instance)->Deliver(Command(3, "hello  there"));
  ASSERT_TRUE(hello) << hello.error();
  EXPECT_TRUE(Parse<CommandResult>(hello->response()).claimed());
  ASSERT_EQ(recorder.calls.size(), 1);
  EXPECT_EQ(recorder.calls[0].method(), "Chat");
  const auto chat = Parse<ChatRequest>(recorder.calls[0].request());
  EXPECT_EQ(chat.player(), 3);
  EXPECT_EQ(chat.text(), "Hello from Go!");

  // Other commands pass through to the game.
  auto other = (*instance)->Deliver(Command(3, "say hi"));
  ASSERT_TRUE(other) << other.error();
  EXPECT_FALSE(Parse<CommandResult>(other->response()).claimed());

  // The first frame is logged once.
  FrameEvent tick;
  tick.set_tick(7);
  const auto frame = Event("Frame", tick);
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_EQ(recorder.calls.size(), 2);
  EXPECT_EQ(recorder.calls[1].method(), "Log");
  EXPECT_EQ(Parse<LogRequest>(recorder.calls[1].request()).message(), "first frame at tick 7");
}

TEST(WasmInstance, LuauModAnswersEvents) {
  Runtime runtime;
  Recorder recorder;
  auto instance = Instance::Load(runtime, ReadFile(LUAU_WASM), Limits{}, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  // Start runs the example with the library and asks for frames because the
  // mod registered a frame handler.
  const std::string library = LUAU_LIBRARY;
  StartEvent start;
  start.set_source(Zip({
      {"main.luau", Text(HELLO_LUAU_MAIN)},
      {"@modlock/init.luau", Text(library + "/init.luau")},
      {"@modlock/wire.luau", Text(library + "/wire.luau")},
  }));
  auto started = (*instance)->Deliver(Event("Start", start));
  ASSERT_TRUE(started) << started.error();
  ASSERT_EQ(started->error(), "");
  EXPECT_TRUE(Parse<StartResult>(started->response()).frames());

  // The hello command is claimed and greets its player.
  auto hello = (*instance)->Deliver(Command(3, "hello  there"));
  ASSERT_TRUE(hello) << hello.error();
  EXPECT_TRUE(Parse<CommandResult>(hello->response()).claimed());
  ASSERT_EQ(recorder.calls.size(), 1);
  EXPECT_EQ(recorder.calls[0].method(), "Chat");
  const auto chat = Parse<ChatRequest>(recorder.calls[0].request());
  EXPECT_EQ(chat.player(), 3);
  EXPECT_EQ(chat.text(), "Hello from Luau!");

  // Other commands pass through to the game.
  auto other = (*instance)->Deliver(Command(3, "say hi"));
  ASSERT_TRUE(other) << other.error();
  EXPECT_FALSE(Parse<CommandResult>(other->response()).claimed());

  // The first frame is logged once.
  FrameEvent tick;
  tick.set_tick(7);
  const auto frame = Event("Frame", tick);
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_EQ(recorder.calls.size(), 2);
  EXPECT_EQ(recorder.calls[1].method(), "Log");
  EXPECT_EQ(Parse<LogRequest>(recorder.calls[1].request()).message(), "first frame at tick 7");
}

TEST(WasmInstance, PythonModAnswersEvents) {
  Runtime runtime;
  Recorder recorder;
  auto instance = Instance::Load(runtime, ReadFile(PYTHON_WASM), Limits{}, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  // Start runs the example with the library and asks for frames because the
  // mod registered a frame handler.
  const std::string library = PYTHON_LIBRARY;
  StartEvent start;
  start.set_source(Zip({
      {"main.py", Text(HELLO_PYTHON_MAIN)},
      {"modlock/__init__.py", Text(library + "/__init__.py")},
      {"modlock/wire.py", Text(library + "/wire.py")},
  }));
  auto started = (*instance)->Deliver(Event("Start", start));
  ASSERT_TRUE(started) << started.error();
  ASSERT_EQ(started->error(), "");
  EXPECT_TRUE(Parse<StartResult>(started->response()).frames());

  // The hello command is claimed and greets its player.
  auto hello = (*instance)->Deliver(Command(3, "hello  there"));
  ASSERT_TRUE(hello) << hello.error();
  EXPECT_TRUE(Parse<CommandResult>(hello->response()).claimed());
  ASSERT_EQ(recorder.calls.size(), 1);
  EXPECT_EQ(recorder.calls[0].method(), "Chat");
  const auto chat = Parse<ChatRequest>(recorder.calls[0].request());
  EXPECT_EQ(chat.player(), 3);
  EXPECT_EQ(chat.text(), "Hello from Python!");

  // Other commands pass through to the game.
  auto other = (*instance)->Deliver(Command(3, "say hi"));
  ASSERT_TRUE(other) << other.error();
  EXPECT_FALSE(Parse<CommandResult>(other->response()).claimed());

  // The first frame is logged once.
  FrameEvent tick;
  tick.set_tick(7);
  const auto frame = Event("Frame", tick);
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_TRUE((*instance)->Deliver(frame));
  ASSERT_EQ(recorder.calls.size(), 2);
  EXPECT_EQ(recorder.calls[1].method(), "Log");
  EXPECT_EQ(Parse<LogRequest>(recorder.calls[1].request()).message(), "first frame at tick 7");
}

TEST(WasmInstance, TrapStopsTheMod) {
  Runtime runtime;
  Recorder recorder;
  auto instance = Instance::Load(runtime, Wat(R"((module
    (memory (export "memory") 1)
    (func (export "modlock_event") (param i32) (result i64) unreachable)))"),
                                 Limits{}, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  const auto frame = Event("Frame", FrameEvent{});
  EXPECT_FALSE((*instance)->Deliver(frame));
  ASSERT_TRUE((*instance)->Failure());
  EXPECT_FALSE((*instance)->Deliver(frame));
}

TEST(WasmInstance, BudgetStopsARunawayLoop) {
  Runtime runtime;
  Recorder recorder;
  Limits limits;
  limits.event_budget = std::chrono::milliseconds{50};
  auto instance = Instance::Load(runtime, Wat(R"((module
    (memory (export "memory") 1)
    (func (export "modlock_event") (param i32) (result i64) (loop br 0) i64.const 0)))"),
                                 limits, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  const auto frame = Event("Frame", FrameEvent{});
  const auto began = std::chrono::steady_clock::now();
  EXPECT_FALSE((*instance)->Deliver(frame));
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::seconds{2});
  EXPECT_TRUE((*instance)->Failure());
}

// A script mod evaluates its whole script in Start, which takes longer than
// one event may.
TEST(WasmInstance, StartGetsTheStartBudget) {
  Runtime runtime;
  Recorder recorder;
  Limits limits;
  limits.event_budget = std::chrono::milliseconds{1};
  auto instance = Instance::Load(runtime, Wat(R"((module
    (memory (export "memory") 1)
    (func (export "modlock_event") (param i32) (result i64) (local $i i32)
      (loop
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if 0 (i32.lt_u (local.get $i) (i32.const 100000000))))
      i64.const 0)))"),
                                 limits, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  auto started = (*instance)->Deliver(Event("Start", StartEvent{}));
  EXPECT_TRUE(started) << started.error();
  EXPECT_FALSE((*instance)->Deliver(Event("Frame", FrameEvent{})));
}

TEST(WasmInstance, BudgetExcludesHostCalls) {
  Runtime runtime;
  Limits limits;
  limits.event_budget = std::chrono::milliseconds{50};
  auto slow = [](const Call&) {
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return Reply{};
  };
  auto instance = Instance::Load(runtime, Wat(R"((module
    (import "modlock" "host_call" (func $call (param i32 i32) (result i32)))
    (memory (export "memory") 1)
    (func (export "modlock_event") (param i32) (result i64) (local $i i32)
      (drop (call $call (i32.const 0) (i32.const 0)))
      (loop
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if 0 (i32.lt_u (local.get $i) (i32.const 1000))))
      i64.const 0)))"),
                                 limits, slow);
  ASSERT_TRUE(instance) << instance.error();

  const auto frame = Event("Frame", FrameEvent{});
  auto delivered = (*instance)->Deliver(frame);
  EXPECT_TRUE(delivered) << delivered.error();
}

TEST(WasmInstance, MemoryLimitRefusesALargeMod) {
  Runtime runtime;
  Recorder recorder;
  Limits limits;
  limits.memory_bytes = 1 << 20;
  auto instance = Instance::Load(runtime, Wat(R"((module
    (memory (export "memory") 64)
    (func (export "modlock_event") (param i32) (result i64) i64.const 0)))"),
                                 limits, recorder.Answer());
  EXPECT_FALSE(instance);
}

TEST(WasmInstance, RequestOutsideMemoryStopsTheMod) {
  Runtime runtime;
  Recorder recorder;
  auto instance = Instance::Load(runtime, Wat(R"((module
    (import "modlock" "host_call" (func $call (param i32 i32) (result i32)))
    (memory (export "memory") 1)
    (func (export "modlock_event") (param i32) (result i64)
      (drop (call $call (i32.const 65530) (i32.const 100)))
      i64.const 0)))"),
                                 Limits{}, recorder.Answer());
  ASSERT_TRUE(instance) << instance.error();

  const auto frame = Event("Frame", FrameEvent{});
  auto delivered = (*instance)->Deliver(frame);
  ASSERT_FALSE(delivered);
  EXPECT_NE(delivered.error().find("outside memory"), std::string::npos) << delivered.error();
  EXPECT_TRUE(recorder.calls.empty());
}

TEST(WasmRuntime, SharesACompiledModuleWhileItIsHeld) {
  Runtime runtime;
  const auto bytes = Wat("(module)");

  // A second compile of the same bytes returns the held module.
  auto first = runtime.Compile(bytes);
  ASSERT_TRUE(first) << first.error();
  auto second = runtime.Compile(bytes);
  ASSERT_TRUE(second) << second.error();
  EXPECT_EQ(first->get(), second->get());
}

}  // namespace
