//go:build deps_only

#include "wasm/input.h"

#include <algorithm>
#include <cstring>

#include "modlock/gameinterop/entity_abi.h"
#include "wasm/game.h"
#include "wasm/game_internal.h"

namespace modlock::wasm {

Input::Input(GameServices& services) : services_(services) {
  services_.InputSources().Add(&changes_);
}

Input::~Input() {
  services_.InputSources().Remove(&changes_);
  services_.Controllers().Release(this);
}

std::vector<Input::ButtonChange> Input::TakeButtonChanges() {
  return changes_.TakeChanges();
}

void Input::WorldEnding() {
  // A world that ends never delivers the pending presses.
  changes_.DropPresses();
}

void Input::Leave(int32_t slot) {
  changes_.Leave(slot);
  services_.Controllers().Watch(this, slot, false);
}

void Input::Clear() {
  changes_.Clear();
  services_.Controllers().Release(this);
}

// The input calls replace the mod's changes; the shared hook applies them.

std::expected<void, std::string> Input::BlockInput(const wasm::InputRequest& request) {
  changes_.Block(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Input::BlockPlayerInput(const wasm::PlayerInputRequest& request) {
  changes_.BlockPlayer(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Input::Press(const wasm::PlayerInputRequest& request) {
  changes_.Press(request.player(), request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Input::WatchInput(const wasm::InputRequest& request) {
  changes_.Watch(request.buttons());
  services_.Input();
  return {};
}

std::expected<void, std::string> Input::RemapInput(const wasm::RemapInputRequest& request) {
  changes_.Remap(request);
  services_.Input();
  return {};
}

std::expected<wasm::ButtonsResponse, std::string> Input::Buttons(
    const players::PlayerRequest& request) {
  // Resolve the controller's pawn fields.
  auto pawn_field = services_.Field("CBasePlayerController", "m_hPawn");
  if (!pawn_field) {
    return std::unexpected(pawn_field.error());
  }
  auto hero_field = services_.Field("CCitadelPlayerController", "m_hHeroPawn");
  if (!hero_field) {
    return std::unexpected(hero_field.error());
  }

  // Resolve the button fields and the live entity system.
  auto movement = services_.Field("CBasePlayerPawn", "m_pMovementServices");
  if (!movement) {
    return std::unexpected(movement.error());
  }
  auto buttons = services_.Field("CPlayer_MovementServices", "m_nButtons");
  if (!buttons) {
    return std::unexpected(buttons.error());
  }
  auto entities = gameinterop::ResolveLiveEntitySystem();
  if (!entities) {
    return std::unexpected(entities.error());
  }

  // A player's controller is the entity at index slot + 1. A dead player's
  // input reaches the pawn that observes, so both pawns are read.
  const auto all = gameinterop::EntityInstances(*entities);
  const auto controller = std::ranges::find_if(all, [&](void* entity) {
    const auto handle = gameinterop::ReferenceHandleOf(entity);
    return handle &&
           static_cast<int32_t>(gameinterop::EntityIndexOf(*handle)) == request.player() + 1;
  });
  if (controller == all.end()) {
    return std::unexpected("the player is not connected");
  }
  wasm::ButtonsResponse response;
  for (const auto& field : {*pawn_field, *hero_field}) {
    // Read the pawn's movement services.
    uint32_t handle = 0;
    std::memcpy(&handle, static_cast<unsigned char*>(*controller) + field.offset, sizeof(handle));
    auto* pawn = static_cast<unsigned char*>(gameinterop::EntityInstance(*entities, handle));
    if (pawn == nullptr) {
      continue;
    }
    unsigned char* services = nullptr;
    std::memcpy(&services, pawn + movement->offset, sizeof(services));
    if (services == nullptr) {
      continue;
    }

    // InButtonState holds its vtable, then the held mask.
    uint64_t held = 0;
    std::memcpy(&held, services + buttons->offset + sizeof(void*), sizeof(held));
    response.set_buttons(response.buttons() | held);
  }
  return response;
}

std::expected<void, std::string> Input::WatchControllers(
    const controllers::WatchControllersRequest& request) {
  if (request.player() < 0 || request.player() >= gameinterop::kMaxPlayerSlots) {
    return std::unexpected("no player has that slot");
  }
  services_.Controllers().Watch(this, request.player(), request.watch());
  return {};
}

void Input::Controllers(google::protobuf::RepeatedPtrField<controllers::PlayerControllers>* out) {
  services_.Controllers().Read(this, out, ControllerTable::Clock::now());
}

}  // namespace modlock::wasm
