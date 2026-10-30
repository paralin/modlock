#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/wasm/profile/profile.pb.h"
#include "src/wasm/wasm.pb.h"

namespace modlock::wasm {

// CallProfile counts the host calls one mod makes: for each Host method, its
// calls, those that failed and those the host refused at its boundary, and
// the same for each key the calls name.
//
// A key names what a call reaches, read from the request by field name, as
// PlayerScope reads players, so a new method is profiled without a list. The
// string fields named class_name, designer_name, field, input, modifier, path
// and service join, in field order, into the message's key, such as
// "CCitadelPlayerPawn.m_iHealth"; each command of a field named command is a
// key of its own; and a field holding a message or a list of them adds the
// keys of each. A path a mod may not use, and a command it may not run, are
// refused under their own key, by the host's own rules for them.
//
// The keys are read from the request's encoding without decoding it, and a
// key already seen costs a lookup, so counting allocates only for a new key.
// The profile keeps kMaxKeys distinct keys and counts each call past them
// that names another as overflow. It runs on one thread at a time.
class CallProfile {
 public:
  // kMaxKeys bounds the distinct keys the profile keeps, and kMaxKeyBytes
  // the length of each.
  static constexpr size_t kMaxKeys = 256;
  static constexpr size_t kMaxKeyBytes = 128;

  CallProfile();

  // Count records call and the host's reply to it. refused is true when the
  // host refused the call before reaching the game, as a limited scope does.
  void Count(const Call& call, const Reply& reply, bool refused);

  // Take returns what was counted since the last Take, or nullopt when
  // nothing was, and starts the next interval.
  [[nodiscard]] std::optional<profile::CallProfile> Take();

 private:
  // Plan is where a message's keys sit in its encoding: parts are the field
  // numbers of its key's parts in field order, commands and paths the fields
  // whose values are refused by name, and nested the message fields with keys
  // of their own.
  struct Plan {
    std::vector<int> parts;
    std::vector<int> commands;
    int path = 0;
    std::vector<std::pair<int, const Plan*>> nested;
  };

  // kMaxParts bounds the parts of one message's key.
  static constexpr size_t kMaxParts = 4;

  // Counts are one interval's calls under one method or key.
  struct Counts {
    uint64_t calls = 0;
    uint64_t failed = 0;
    uint64_t refused = 0;
  };

  // Method is one Host method and how its requests hold keys, or the last
  // entry, with an empty name and no plan, for names the host has no method
  // for.
  struct Method {
    std::string_view name;
    const Plan* plan = nullptr;
    Counts counts;
  };

  // Key is one key's calls under the method at index method.
  struct Key {
    size_t method = 0;
    Counts counts;
  };

  // Outcome is how a call ended: failed, refused as a whole, or refused for
  // the key under which it is counted.
  struct Outcome {
    bool failed = false;
    bool refused = false;
  };

  // PlanFor returns the plan of type, built once, or null for a type without
  // keys.
  const Plan* PlanFor(const google::protobuf::Descriptor* type);
  // Scan counts each key the encoded message holds, as plan places them.
  // depth bounds the nesting a crafted request can make the scan follow.
  void Scan(size_t method, const Plan& plan, std::string_view message, Outcome outcome, int depth,
            bool& refused);
  // CountKey counts one key of method.
  void CountKey(size_t method, std::string_view key, Outcome outcome);

  // methods_ holds every Host method by its position, then the unknown entry;
  // index_ finds a method's position by name.
  std::vector<Method> methods_;
  std::unordered_map<std::string_view, size_t> index_;
  // plans_ holds each message type's plan, or null for one without keys.
  std::unordered_map<const google::protobuf::Descriptor*, std::optional<Plan>> plans_;
  // keys_ maps each kept key, after its method's name and a newline, to its
  // counts. A method name holds no newline, so no two methods share an entry.
  // scratch_ builds each lookup and joined_ each message's key, both without
  // allocating once warm.
  std::unordered_map<std::string, Key> keys_;
  std::string scratch_;
  std::string joined_;
  uint64_t overflow_ = 0;
  // counted_ is true once anything was counted since the last Take.
  bool counted_ = false;
};

}  // namespace modlock::wasm
