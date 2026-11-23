//go:build deps_only

#include "wasm/controller_table.h"

#include <set>
#include <utility>

namespace modlock::wasm {

namespace {

// Append adds the values of from to the end of to.
template <typename To, typename From>
void Append(To& to, const From& from) {
  to.insert(to.end(), from.begin(), from.end());
}

}  // namespace

void ControllerTable::Listen(Wanted wanted) {
  wanted_ = std::move(wanted);
  Publish();
}

void ControllerTable::Watch(const void* watcher, int32_t slot, bool watch) {
  // A stop forgets the watcher, and the last one the slot's copy.
  if (!watch) {
    const auto found = streams_.find(slot);
    if (found == streams_.end() || found->second.watchers.erase(watcher) == 0) {
      return;
    }
    if (found->second.watchers.empty()) {
      streams_.erase(found);
      Publish();
    }
    return;
  }

  // The first watcher of a slot starts its next epoch.
  auto& stream = streams_[slot];
  const bool first = stream.watchers.empty();
  if (!stream.watchers.try_emplace(watcher).second || !first) {
    return;
  }
  stream.epoch = ++epochs_[slot];
  Publish();
}

void ControllerTable::Release(const void* watcher) {
  bool changed = false;
  for (auto it = streams_.begin(); it != streams_.end();) {
    // Forget the watcher, and each slot it watched alone.
    it->second.watchers.erase(watcher);
    if (it->second.watchers.empty()) {
      it = streams_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) {
    Publish();
  }
}

void ControllerTable::Apply(
    int32_t slot, const google::protobuf::RepeatedPtrField<controllers::ControllerFrame>& frames,
    bool ended, Clock::time_point now) {
  // A slot nobody watches takes nothing.
  const auto found = streams_.find(slot);
  if (found == streams_.end()) {
    return;
  }
  auto& stream = found->second;

  // Apply the current epoch's frames, changes only once a full frame synced.
  for (const auto& frame : frames) {
    if (frame.epoch() != stream.epoch || (!frame.full() && !stream.synced)) {
      continue;
    }
    ApplyFrame(stream, frame);
    stream.last = now;
  }
  if (ended) {
    Drop(stream);
  }
}

void ControllerTable::Read(const void* watcher,
                           google::protobuf::RepeatedPtrField<controllers::PlayerControllers>* out,
                           Clock::time_point now) {
  for (auto& [slot, stream] : streams_) {
    const auto mine = stream.watchers.find(watcher);
    if (mine == stream.watchers.end()) {
      continue;
    }

    // Name the stream's state, as Controllers.tla's State does.
    auto* player = out->Add();
    player->set_player(slot);
    if (!stream.synced) {
      player->set_stream(controllers::CONTROLLER_STREAM_NONE);
      continue;
    }
    if (now - stream.last > kDeadline) {
      player->set_stream(controllers::CONTROLLER_STREAM_STALE);
    } else if (!stream.focused) {
      player->set_stream(controllers::CONTROLLER_STREAM_UNFOCUSED);
    } else {
      player->set_stream(controllers::CONTROLLER_STREAM_LIVE);
    }

    // List each device with the edges this watcher has not read.
    auto& edges = mine->second;
    for (const auto& [id, device] : stream.devices) {
      auto* controller = player->add_controllers();
      *controller = device;
      const auto unread = edges.find(id);
      if (unread == edges.end()) {
        continue;
      }
      auto* state = controller->mutable_state();
      state->mutable_pressed()->Add(unread->second.pressed.begin(), unread->second.pressed.end());
      state->mutable_released()->Add(unread->second.released.begin(),
                                     unread->second.released.end());
      if (state->has_gamepad()) {
        auto* pad = state->mutable_gamepad();
        for (const auto button : unread->second.pad_pressed) {
          pad->add_pressed(button);
        }
        for (const auto button : unread->second.pad_released) {
          pad->add_released(button);
        }
      }
    }
    edges.clear();
  }
}

void ControllerTable::ApplyFrame(Stream& stream, const controllers::ControllerFrame& frame) {
  // A full frame replaces the devices, and the edges of those it no longer
  // lists; a change frame removes the detached ones.
  if (frame.full()) {
    std::set<uint32_t> listed;
    for (const auto& info : frame.added()) {
      listed.insert(info.id());
    }
    std::erase_if(stream.devices, [&](const auto& entry) { return !listed.contains(entry.first); });
    for (auto& [watcher, edges] : stream.watchers) {
      std::erase_if(edges, [&](const auto& entry) { return !listed.contains(entry.first); });
    }
  }
  for (const auto id : frame.removed()) {
    stream.devices.erase(id);
    for (auto& [watcher, edges] : stream.watchers) {
      edges.erase(id);
    }
  }

  // Describe the attached devices.
  for (const auto& info : frame.added()) {
    auto& device = stream.devices[info.id()];
    *device.mutable_info() = info;
    device.mutable_state()->set_id(info.id());
  }

  // Keep each changed state, and hand its edges to every watcher.
  for (const auto& state : frame.changed()) {
    const auto device = stream.devices.find(state.id());
    if (device == stream.devices.end()) {
      continue;
    }
    for (auto& [watcher, edges] : stream.watchers) {
      auto& unread = edges[state.id()];
      Append(unread.pressed, state.pressed());
      Append(unread.released, state.released());
      for (const auto button : state.gamepad().pressed()) {
        unread.pad_pressed.push_back(static_cast<controllers::GamepadButton>(button));
      }
      for (const auto button : state.gamepad().released()) {
        unread.pad_released.push_back(static_cast<controllers::GamepadButton>(button));
      }
    }
    auto* kept = device->second.mutable_state();
    *kept = state;
    kept->clear_pressed();
    kept->clear_released();
    if (kept->has_gamepad()) {
      kept->mutable_gamepad()->clear_pressed();
      kept->mutable_gamepad()->clear_released();
    }
  }
  stream.focused = frame.focused();
  stream.synced = true;
}

void ControllerTable::Drop(Stream& stream) {
  stream.synced = false;
  stream.focused = false;
  stream.devices.clear();
  for (auto& [watcher, edges] : stream.watchers) {
    edges.clear();
  }
}

void ControllerTable::Publish() const {
  if (!wanted_) {
    return;
  }
  controllers::ControllerWants wants;
  for (const auto& [slot, stream] : streams_) {
    auto* want = wants.add_wants();
    want->set_player(slot);
    want->set_epoch(stream.epoch);
  }
  wanted_(wants);
}

}  // namespace modlock::wasm
