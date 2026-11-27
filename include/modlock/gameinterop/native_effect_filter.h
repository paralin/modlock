#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "modlock/export.h"
#include "modlock/gameinterop/native_post_filter.h"

namespace modlock::gameinterop {

// NativeEffectFilter keeps chosen particles and sounds the server sends from
// chosen recipients. It reads each particle create and each sound start, asks
// its handler which recipients must not get that cue, and posts the message
// to the others alone. A particle kept from a recipient stays kept from them:
// the later messages about its index skip them too, until the index is
// released or created again. The filter changes only who receives a message,
// so the event that caused the cue resolves as before. A message it cannot
// read goes to every recipient. The filter is a NativePostFilter route, so it
// runs where that filter does, and it cannot reach a cue a client predicts
// itself, which the server never sends.
class MODLOCK_API NativeEffectFilter {
 public:
  // Kind is the cue's medium.
  enum class Kind : uint8_t { kParticle, kSound };

  // Cue is one particle or sound the server starts.
  struct Cue {
    Kind kind = Kind::kParticle;
    // name is a particle system's 64-bit resource name, or a sound event
    // name's 32-bit hash, as SoundName computes it.
    uint64_t name = 0;
    // source is the index of the entity the cue plays on, or -1 for none.
    int32_t source = -1;
  };

  // Handler returns the recipient slots, one bit each, that must not get
  // cue. It runs synchronously on the engine thread for each cue while
  // Active reports its kind.
  using Handler = std::function<uint64_t(const Cue&)>;

  // Active reports whether any rule names a cue of kind; a post of an
  // inactive kind with no particle kept goes through without a decode.
  using Active = std::function<bool(Kind)>;

  // SoundName hashes a sound event name, such as player.melee.parry.shared,
  // as the sound start message carries it.
  static uint32_t SoundName(std::string_view name);

  // Decode reads one length-prefixed particle create or sound start of
  // message_id, and returns nothing for any other message or bytes it cannot
  // read.
  static std::optional<Cue> Decode(int32_t message_id, std::span<const uint8_t> serialized);

  // Install registers the filter with posts for the particle and sound start
  // messages until destruction. posts must outlive the filter.
  static std::expected<NativeEffectFilter, std::string> Install(NativePostFilter& posts,
                                                                Active active, Handler handler);
  NativeEffectFilter(NativeEffectFilter&&) noexcept;
  NativeEffectFilter& operator=(NativeEffectFilter&&) noexcept;
  ~NativeEffectFilter();

 private:
  struct Impl;
  explicit NativeEffectFilter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
