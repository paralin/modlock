//go:build deps_only

#include "wasm/stack_samples.h"

#include <algorithm>
#include <map>
#include <vector>

#include "google/protobuf/struct.pb.h"
#include "google/protobuf/util/json_util.h"

namespace modlock::wasm {
namespace {

// Field returns the member name of an object, or null.
const google::protobuf::Value* Field(const google::protobuf::Struct& object, const char* name) {
  const auto found = object.fields().find(name);
  return found == object.fields().end() ? nullptr : &found->second;
}

// Item returns the element at index of an array, or null.
const google::protobuf::Value* Item(const google::protobuf::Value& array, int64_t index) {
  if (!array.has_list_value() || index < 0 || index >= array.list_value().values_size()) {
    return nullptr;
  }
  return &array.list_value().values(static_cast<int>(index));
}

}  // namespace

void StackTally::Add(const profile::HotStack& stack) {
  profile::HotStack key = stack;
  key.set_samples(0);
  auto [at, inserted] = stacks_.try_emplace(key.SerializeAsString(), stack);
  if (!inserted) {
    at->second.set_samples(at->second.samples() + stack.samples());
  }
  total_ += stack.samples();
}

void StackTally::Add(const profile::StackSamples& samples) {
  uint32_t listed = 0;
  for (const auto& stack : samples.stacks()) {
    Add(stack);
    listed += stack.samples();
  }
  // Samples of stacks the sender did not list count toward the total alone.
  total_ += samples.total() > listed ? samples.total() - listed : 0;
}

profile::StackSamples StackTally::Take() {
  std::vector<profile::HotStack*> order;
  for (auto& [key, stack] : stacks_) {
    order.push_back(&stack);
  }
  const size_t top = std::min(order.size(), kTopStacks);
  std::partial_sort(order.begin(), order.begin() + top, order.end(),
                    [](const auto* a, const auto* b) { return a->samples() > b->samples(); });
  profile::StackSamples out;
  out.set_total(total_);
  for (size_t i = 0; i < top; ++i) {
    *out.add_stacks() = std::move(*order[i]);
  }
  stacks_.clear();
  total_ = 0;
  return out;
}

GuestSampler::GuestSampler(const wasmtime::Engine& engine, const wasmtime::Module& module) {
  static char name[] = "mod";
  static const wasm_name_t kName = {.size = 3, .data = name};
  const wasmtime_guestprofiler_modules_t modules[] = {{&kName, module.capi()}};
  profiler_.reset(wasmtime_guestprofiler_new(engine.capi(), &kName, 10'000'000, modules, 1));
}

GuestSampler::~GuestSampler() = default;

void GuestSampler::Sample(const wasmtime::Store& store, std::chrono::nanoseconds since) {
  wasmtime_guestprofiler_sample(profiler_.get(), store.capi(), static_cast<uint64_t>(since.count()));
}

profile::StackSamples GuestSampler::Finish() {
  // End the profile, which is the Firefox profiler's processed format.
  wasm_byte_vec_t out{};
  wasmtime_error_t* error = wasmtime_guestprofiler_finish(profiler_.release(), &out);
  if (error) {
    wasmtime_error_delete(error);
    return {};
  }
  const std::string text(out.data, out.size);
  wasm_byte_vec_delete(&out);
  google::protobuf::Struct root;
  if (!google::protobuf::json::JsonStringToMessage(text, &root).ok()) {
    return {};
  }

  // Find the thread's tables: samples name a stack, a stack a frame and the
  // stack outside it, a frame a function, and a function a name.
  const auto* threads = Field(root, "threads");
  if (!threads || threads->list_value().values().empty()) {
    return {};
  }
  const auto& thread = threads->list_value().values(0).struct_value();
  const auto* samples = Field(thread, "samples");
  const auto* stacks = Field(thread, "stackTable");
  const auto* frames = Field(thread, "frameTable");
  const auto* functions = Field(thread, "funcTable");
  const auto* strings = Field(thread, "stringArray");
  if (!samples || !stacks || !frames || !functions || !strings) {
    return {};
  }
  const auto* sampled = Field(samples->struct_value(), "stack");
  const auto* prefix = Field(stacks->struct_value(), "prefix");
  const auto* frame_of = Field(stacks->struct_value(), "frame");
  const auto* func_of = Field(frames->struct_value(), "func");
  const auto* name_of = Field(functions->struct_value(), "name");
  if (!sampled || !prefix || !frame_of || !func_of || !name_of) {
    return {};
  }

  // Count the samples of each stack.
  std::map<int64_t, uint32_t> counts;
  for (const auto& value : sampled->list_value().values()) {
    if (value.has_number_value()) {
      ++counts[static_cast<int64_t>(value.number_value())];
    }
  }

  // Name each stack's functions from the innermost out.
  profile::StackSamples result;
  for (const auto& [stack, count] : counts) {
    auto* hot = result.add_stacks();
    hot->set_samples(count);
    result.set_total(result.total() + count);
    auto at = stack;
    while (at >= 0 && hot->frames_size() < static_cast<int>(kMaxFrames)) {
      const auto* frame = Item(*frame_of, at);
      const auto* func = frame ? Item(*func_of, static_cast<int64_t>(frame->number_value())) : nullptr;
      const auto* name = func ? Item(*name_of, static_cast<int64_t>(func->number_value())) : nullptr;
      const auto* function = name ? Item(*strings, static_cast<int64_t>(name->number_value())) : nullptr;
      if (function && function->has_string_value()) {
        hot->add_frames()->set_function(function->string_value());
      }
      const auto* outer = Item(*prefix, at);
      at = outer && outer->has_number_value() ? static_cast<int64_t>(outer->number_value()) : -1;
    }
  }
  return result;
}

}  // namespace modlock::wasm
