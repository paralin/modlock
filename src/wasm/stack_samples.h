#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <wasmtime.hh>
#include <wasmtime/profiling.h>

#include "src/wasm/profile/profile.pb.h"

namespace modlock::wasm {

// StackTally folds the stacks sampled during one call into counts by stack.
// It keeps every distinct stack until Take, which returns the few with the
// most samples.
class StackTally {
 public:
  // kTopStacks is how many stacks Take returns.
  static constexpr size_t kTopStacks = 3;

  // Add counts the samples of stack.
  void Add(const profile::HotStack& stack);

  // Add counts every stack in samples, and its total.
  void Add(const profile::StackSamples& samples);

  // Take returns the kTopStacks stacks with the most samples, most first, and
  // the total of all samples, then starts empty. It returns an empty message
  // when nothing was sampled.
  [[nodiscard]] profile::StackSamples Take();

 private:
  // stacks_ maps a stack's frames, encoded, to its samples.
  std::unordered_map<std::string, profile::HotStack> stacks_;
  uint32_t total_ = 0;
};

// GuestSampler samples the Wasm call stack of a compiled mod with Wasmtime's
// guest profiler. The stack's frames name the module's functions from its name
// section, and carry no file or line.
class GuestSampler {
 public:
  // GuestSampler starts a profile of module, which the store runs.
  GuestSampler(const wasmtime::Engine& engine, const wasmtime::Module& module);
  ~GuestSampler();
  GuestSampler(const GuestSampler&) = delete;
  GuestSampler& operator=(const GuestSampler&) = delete;

  // Sample records the stack the store is running, as the epoch deadline
  // callback sees it. since is the processor time since the last sample.
  void Sample(const wasmtime::Store& store, std::chrono::nanoseconds since);

  // Finish ends the profile and returns the stacks it sampled, each with its
  // count and at most kMaxFrames frames from the innermost out. A profile it
  // cannot read comes back empty.
  [[nodiscard]] profile::StackSamples Finish();

  // kMaxFrames bounds the frames kept of each stack.
  static constexpr size_t kMaxFrames = 8;

 private:
  struct Deleter {
    void operator()(wasmtime_guestprofiler_t* profiler) const {
      wasmtime_guestprofiler_delete(profiler);
    }
  };
  std::unique_ptr<wasmtime_guestprofiler_t, Deleter> profiler_;
};

}  // namespace modlock::wasm
