//go:build deps_only

#include "wasm/player_scope.h"

#include <array>
#include <memory>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"

namespace modlock::wasm {

namespace {

// kServerWide names the host requests that change the game for every player,
// which a limited mod may not make. A bot it added would join the whole
// server, outside its players.
constexpr std::array<std::string_view, 10> kServerWide = {
    "ServerCommand", "SetFreeForAll", "BlockInput",     "Pause",       "HoldMatchClock",
    "StartRift",     "ClearMap",      "RemoveEntities", "RestoreNpcs", "AddBot",
};

// RequestType returns the request message type of method in the named
// service, or null when the service has no such method.
const google::protobuf::Descriptor* RequestType(std::string_view service, std::string_view method) {
  const auto* found =
      google::protobuf::DescriptorPool::generated_pool()->FindServiceByName(std::string(service));
  if (!found) {
    return nullptr;
  }
  const auto* called = found->FindMethodByName(std::string(method));
  return called ? called->input_type() : nullptr;
}

}  // namespace

std::string PlayerScope::Refuse(const Call& call) const {
  if (!players_) {
    return {};
  }
  for (const auto method : kServerWide) {
    if (call.method() == method) {
      return "this mod serves only some players and may not call " + call.method();
    }
  }
  if (!Addresses("modlock.wasm.Host", call.method(), call.request())) {
    return "this mod does not serve the player " + call.method() + " names";
  }
  return {};
}

bool PlayerScope::Hears(const Call& event) const {
  return !players_ || Addresses("modlock.wasm.Mod", event.method(), event.request());
}

void PlayerScope::Keep(google::protobuf::RepeatedPtrField<players::Connection>& players) const {
  if (!players_) {
    return;
  }
  for (int i = players.size() - 1; i >= 0; --i) {
    if (!Serves(players.Get(i).player())) {
      players.DeleteSubrange(i, 1);
    }
  }
}

bool PlayerScope::Addresses(std::string_view service, std::string_view method,
                            const std::string& message) const {
  // A method without a known request type names no player.
  const auto* type = RequestType(service, method);
  if (!type) {
    return true;
  }
  const auto* player = type->FindFieldByName("player");
  const auto* players = type->FindFieldByName("players");
  using Type = google::protobuf::FieldDescriptor::CppType;
  const bool single = player && !player->is_repeated() && player->cpp_type() == Type::CPPTYPE_INT32;
  const bool many = players && players->is_repeated() && players->cpp_type() == Type::CPPTYPE_INT32;
  if (!single && !many) {
    return true;
  }

  // Decode the message and check each slot it names. One that does not
  // decode reaches the host, which refuses it as malformed.
  std::unique_ptr<google::protobuf::Message> decoded(
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(type)->New());
  if (!decoded->ParseFromString(message)) {
    return true;
  }
  const auto* reflection = decoded->GetReflection();
  if (single && !Serves(reflection->GetInt32(*decoded, player))) {
    return false;
  }
  if (many) {
    for (int i = 0; i < reflection->FieldSize(*decoded, players); ++i) {
      if (!Serves(reflection->GetRepeatedInt32(*decoded, players, i))) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace modlock::wasm
