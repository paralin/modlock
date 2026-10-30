//go:build deps_only

#include "wasm/call_profile.h"

#include <algorithm>

#include "google/protobuf/descriptor.h"
#include "utf8_validity.h"
#include "wasm/console_lines.h"
#include "wasm/files.h"

namespace modlock::wasm {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;

// kKeyParts are the names of the string fields that join into a message's
// key: what a call reaches in the game, rather than its own resources.
constexpr std::string_view kKeyParts[] = {"class_name", "designer_name", "field",  "input",
                                          "modifier",   "path",          "service"};

// kMaxDepth bounds how deep the scan follows nested messages.
constexpr int kMaxDepth = 8;

// Wire reads protocol buffer fields from an encoded message, stopping at the
// first malformed byte.
struct Wire {
  std::string_view rest;

  bool Varint(uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && !rest.empty(); shift += 7) {
      const auto byte = static_cast<uint8_t>(rest.front());
      rest.remove_prefix(1);
      value |= uint64_t{byte & 0x7fu} << shift;
      if ((byte & 0x80u) == 0) {
        return true;
      }
    }
    return false;
  }

  bool Skip(size_t size) {
    if (rest.size() < size) {
      return false;
    }
    rest.remove_prefix(size);
    return true;
  }

  // Next reads the next field's number and, for a length-delimited field,
  // its bytes into value; other fields leave value empty.
  bool Next(int& number, std::string_view& value) {
    uint64_t tag = 0;
    if (!Varint(tag)) {
      return false;
    }
    number = static_cast<int>(tag >> 3);
    value = {};
    uint64_t scalar = 0;
    switch (tag & 7) {
      case 0:
        return Varint(scalar);
      case 1:
        return Skip(8);
      case 5:
        return Skip(4);
      case 2:
        if (!Varint(scalar) || scalar > rest.size()) {
          return false;
        }
        value = rest.substr(0, scalar);
        rest.remove_prefix(scalar);
        return true;
      default:
        return false;
    }
  }
};

// Truncate shortens key to at most CallProfile::kMaxKeyBytes, at a UTF-8
// character boundary.
std::string_view Truncate(std::string_view key) {
  if (key.size() <= CallProfile::kMaxKeyBytes) {
    return key;
  }
  size_t size = CallProfile::kMaxKeyBytes;
  while (size > 0 && (static_cast<uint8_t>(key[size]) & 0xc0u) == 0x80u) {
    size--;
  }
  return key.substr(0, size);
}

}  // namespace

CallProfile::CallProfile() {
  const auto* host =
      google::protobuf::DescriptorPool::generated_pool()->FindServiceByName("modlock.wasm.Host");
  for (int i = 0; host && i < host->method_count(); i++) {
    const auto* method = host->method(i);
    index_.emplace(method->name(), methods_.size());
    methods_.push_back({method->name(), PlanFor(method->input_type())});
  }
  methods_.push_back({});
}

const CallProfile::Plan* CallProfile::PlanFor(const Descriptor* type) {
  // An entry exists from the start of the type's build, so a type that holds
  // itself sees no plan for the inner copy instead of recursing.
  if (const auto found = plans_.find(type); found != plans_.end()) {
    return found->second ? &*found->second : nullptr;
  }
  plans_.emplace(type, std::nullopt);
  std::vector<const FieldDescriptor*> fields;
  for (int i = 0; i < type->field_count(); i++) {
    fields.push_back(type->field(i));
  }
  std::ranges::sort(fields, {}, &FieldDescriptor::number);
  Plan plan;
  for (const auto* field : fields) {
    if (field->type() == FieldDescriptor::TYPE_STRING) {
      if (field->name() == "command") {
        plan.commands.push_back(field->number());
      }
      if (field->name() == "path") {
        plan.path = field->number();
      }
      if (std::ranges::find(kKeyParts, field->name()) != std::end(kKeyParts) &&
          plan.parts.size() < kMaxParts) {
        plan.parts.push_back(field->number());
      }
    } else if (field->type() == FieldDescriptor::TYPE_MESSAGE) {
      if (const auto* nested = PlanFor(field->message_type())) {
        plan.nested.emplace_back(field->number(), nested);
      }
    }
  }
  if (plan.parts.empty() && plan.commands.empty() && plan.nested.empty()) {
    return nullptr;
  }
  auto& kept = plans_[type];
  kept = std::move(plan);
  return &*kept;
}

void CallProfile::Count(const Call& call, const Reply& reply, bool refused) {
  counted_ = true;
  const auto found = index_.find(call.method());
  const bool unknown = found == index_.end();
  const size_t method = unknown ? methods_.size() - 1 : found->second;
  const Outcome outcome{.failed = !reply.error().empty() || refused || unknown,
                        .refused = refused || unknown};
  bool key_refused = false;
  if (unknown) {
    CountKey(method, call.method(), outcome);
  } else if (const auto* plan = methods_[method].plan) {
    Scan(method, *plan, call.request(), outcome, kMaxDepth, key_refused);
  }
  auto& counts = methods_[method].counts;
  counts.calls++;
  counts.failed += outcome.failed || key_refused;
  counts.refused += outcome.refused || key_refused;
}

void CallProfile::Scan(size_t method, const Plan& plan, std::string_view message, Outcome outcome,
                       int depth, bool& refused) {
  std::string_view parts[kMaxParts];
  bool path_refused = false;
  Wire wire{message};
  int number = 0;
  std::string_view value;
  while (wire.Next(number, value)) {
    if (const auto part = std::ranges::find(plan.parts, number); part != plan.parts.end()) {
      parts[part - plan.parts.begin()] = value;
    }
    if (number == plan.path) {
      path_refused = !FileName(value);
    }
    if (std::ranges::find(plan.commands, number) != plan.commands.end()) {
      for (const auto& command : SplitConsoleLine(value)) {
        std::string line(command.name);
        line += ' ';
        line += command.arguments;
        const bool denied = !RefuseConsoleLine(line).empty();
        refused |= denied;
        CountKey(method, command.name,
                 {.failed = outcome.failed || denied, .refused = outcome.refused || denied});
      }
    }
    if (depth > 0) {
      for (const auto& [field, nested] : plan.nested) {
        if (field == number) {
          Scan(method, *nested, value, outcome, depth - 1, refused);
        }
      }
    }
  }
  joined_.clear();
  for (size_t i = 0; i < plan.parts.size(); i++) {
    if (parts[i].empty()) {
      continue;
    }
    if (!joined_.empty()) {
      joined_ += '.';
    }
    joined_ += parts[i];
  }
  if (joined_.empty()) {
    return;
  }
  refused |= path_refused;
  CountKey(method, joined_,
           {.failed = outcome.failed || path_refused, .refused = outcome.refused || path_refused});
}

void CallProfile::CountKey(size_t method, std::string_view key, Outcome outcome) {
  scratch_.assign(methods_[method].name);
  scratch_ += '\n';
  scratch_ += Truncate(key);
  auto found = keys_.find(scratch_);
  if (found == keys_.end()) {
    if (keys_.size() >= kMaxKeys) {
      overflow_++;
      return;
    }
    found = keys_.emplace(scratch_, Key{.method = method}).first;
  }
  auto& counts = found->second.counts;
  counts.calls++;
  counts.failed += outcome.failed;
  counts.refused += outcome.refused;
}

std::optional<profile::CallProfile> CallProfile::Take() {
  if (!counted_) {
    return std::nullopt;
  }
  counted_ = false;
  const auto fill = [](Counts& counts, profile::Counts* out) {
    out->set_calls(counts.calls);
    out->set_failed(counts.failed);
    out->set_refused(counts.refused);
    counts = {};
  };
  profile::CallProfile taken;
  for (auto& method : methods_) {
    if (method.counts.calls == 0) {
      continue;
    }
    auto* out = taken.add_methods();
    out->set_method(method.name);
    fill(method.counts, out->mutable_counts());
  }
  for (auto& [entry, key] : keys_) {
    if (key.counts.calls == 0) {
      continue;
    }
    const auto& name = methods_[key.method].name;
    const auto text = std::string_view(entry).substr(name.size() + 1);
    auto* out = taken.add_keys();
    out->set_method(name);
    out->set_key(utf8_range::IsStructurallyValid(text) ? text : "(not UTF-8)");
    fill(key.counts, out->mutable_counts());
  }
  taken.set_overflow(overflow_);
  overflow_ = 0;
  return taken;
}

}  // namespace modlock::wasm
