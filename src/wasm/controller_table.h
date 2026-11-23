#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "google/protobuf/repeated_ptr_field.h"
#include "src/wasm/controllers/controllers.pb.h"

namespace modlock::wasm {

// ControllerTable holds the controllers that players' own clients stream to
// the host, for the mods that watch them, as Controllers.tla checks. Each
// player's slot has the watchers that want it and a want epoch that grows
// each time a first watcher appears. A frame applies only while some watcher
// wants the slot and only for the current epoch, and a change frame only once
// a full frame synced the copy, so a frame from a closed watch cannot
// resurrect it. Each watcher keeps the presses and releases since its last
// Read, so a tap between two of its frames reaches it. Everything runs on the
// engine thread, and a Read costs one copy per watched player whatever rate
// the client sends at.
class ControllerTable {
 public:
  // kDeadline is how long a stream stays live without a frame; a client sends
  // a heartbeat at least every 50 milliseconds.
  static constexpr std::chrono::milliseconds kDeadline{200};

  using Clock = std::chrono::steady_clock;

  // Wanted is called with every wanted slot and its epoch each time the set
  // changes.
  using Wanted = std::function<void(const controllers::ControllerWants&)>;

  // Listen sends each later change of the wanted slots to wanted, and the
  // current set at once; an empty wanted stops.
  void Listen(Wanted wanted);

  // Watch starts or stops watcher's watch of slot. The first watcher starts a
  // new epoch and the last one drops the slot's copy.
  void Watch(const void* watcher, int32_t slot, bool watch);

  // Release stops every watch watcher holds, as when its mod stops.
  void Release(const void* watcher);

  // Apply takes the frames slot's client sent, oldest first, received at now.
  // ended drops the copy after them: the client's stream ended.
  void Apply(int32_t slot,
             const google::protobuf::RepeatedPtrField<controllers::ControllerFrame>& frames,
             bool ended, Clock::time_point now);

  // Read appends each slot watcher watches to out, with the presses and
  // releases since watcher's last Read, which it then forgets.
  void Read(const void* watcher,
            google::protobuf::RepeatedPtrField<controllers::PlayerControllers>* out,
            Clock::time_point now);

 private:
  // Edges are one device's presses and releases a watcher has not read.
  struct Edges {
    std::vector<uint32_t> pressed;
    std::vector<uint32_t> released;
    std::vector<controllers::GamepadButton> pad_pressed;
    std::vector<controllers::GamepadButton> pad_released;
  };

  // Stream is one watched slot: its watchers, each one's unread edges by
  // device, and the copy of the client's devices by id once a full frame of
  // the current epoch synced it.
  struct Stream {
    std::map<const void*, std::map<uint32_t, Edges>> watchers;
    uint32_t epoch = 0;
    bool synced = false;
    bool focused = false;
    Clock::time_point last;
    std::map<uint32_t, controllers::Controller> devices;
  };

  // ApplyFrame applies one frame of stream's epoch.
  static void ApplyFrame(Stream& stream, const controllers::ControllerFrame& frame);

  // Drop forgets stream's copy and every unread edge.
  static void Drop(Stream& stream);

  // Publish sends the wanted slots to the listener.
  void Publish() const;

  // streams_ holds each watched slot, and epochs_ each slot's last epoch,
  // which outlives its watches so an epoch never repeats.
  std::map<int32_t, Stream> streams_;
  std::map<int32_t, uint32_t> epochs_;
  Wanted wanted_;
};

}  // namespace modlock::wasm
