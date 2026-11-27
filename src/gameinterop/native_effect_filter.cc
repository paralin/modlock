#include "modlock/gameinterop/native_effect_filter.h"

#include <google/protobuf/io/coded_stream.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "native_network_messages.h"
#include "src/gameinterop/engine/particle.pb.h"
#include "src/gameinterop/engine/sound.pb.h"

namespace modlock::gameinterop {
namespace {

// kCreateParticle and kReleaseParticle are the native PARTICLE_MESSAGE types
// that make a particle at an index and give the index back.
constexpr int32_t kCreateParticle = 0;
constexpr int32_t kReleaseParticle = 9;

// kNoEntity is the networked handle of no entity, and kNetworkedIndexBits
// the entity index inside a networked handle.
constexpr uint32_t kNoEntity = 16777215;
constexpr uint32_t kNetworkedIndexBits = 0x3FFF;

// kCueIds are the messages the filter registers for.
constexpr std::array<int32_t, 2> kCueIds = {kParticleManagerMessageId, kSoundStartMessageId};

// ParseFramed reads one length-prefixed message into message.
bool ParseFramed(std::span<const uint8_t> serialized, google::protobuf::MessageLite& message) {
  if (serialized.empty() || serialized.size() > kMaxUserMessageSize) {
    return false;
  }
  google::protobuf::io::CodedInputStream input(serialized.data(),
                                               static_cast<int>(serialized.size()));
  uint32_t size = 0;
  if (!input.ReadVarint32(&size) || size != serialized.size() - input.CurrentPosition()) {
    return false;
  }
  return message.ParseFromArray(serialized.data() + input.CurrentPosition(),
                                static_cast<int>(size));
}

// ParticleCue names the particle a create makes, or nothing for any other
// particle message.
std::optional<NativeEffectFilter::Cue> ParticleCue(const engine::ParticleManager& message) {
  if (message.type() != kCreateParticle || !message.create_particle().has_particle_name_index()) {
    return std::nullopt;
  }
  const auto handle = message.create_particle().entity_handle();
  return NativeEffectFilter::Cue{
      .kind = NativeEffectFilter::Kind::kParticle,
      .name = message.create_particle().particle_name_index(),
      .source = handle == kNoEntity ? -1 : static_cast<int32_t>(handle & kNetworkedIndexBits),
  };
}

// SoundCue names the sound a start plays.
std::optional<NativeEffectFilter::Cue> SoundCue(std::span<const uint8_t> serialized) {
  engine::SoundStart message;
  if (!ParseFramed(serialized, message) || !message.has_soundevent_hash()) {
    return std::nullopt;
  }
  return NativeEffectFilter::Cue{
      .kind = NativeEffectFilter::Kind::kSound,
      .name = message.soundevent_hash(),
      .source = message.source_entity_index(),
  };
}

}  // namespace

struct NativeEffectFilter::Impl {
  Active active;
  Handler handler;
  NativePostFilter* posts = nullptr;
  std::vector<int32_t> ids;
  // kept holds, for each particle index whose create skipped recipients,
  // the recipients it skipped. A release or a new create at the index ends
  // the entry, so it holds at most the server's live particle indexes.
  std::unordered_map<uint32_t, uint64_t> kept;

  // Unregister before the handler the filters borrow is destroyed.
  ~Impl() {
    for (const auto id : ids) {
      posts->Unregister(id);
    }
  }

  // Filter posts one cue message to the recipients its rules leave.
  void Filter(int32_t id, const NativePostFilter::Post& post) {
    const auto kind = id == kSoundStartMessageId ? Kind::kSound : Kind::kParticle;
    if (!active(kind) && (kind == Kind::kSound || kept.empty())) {
      return post.Forward(post.recipients());
    }
    std::array<uint8_t, kMaxUserMessageSize> bytes{};
    const auto wire = post.Serialize(bytes);
    if (!wire) {
      std::fprintf(stderr, "[modlock] native effect serialization failed\n");
      return post.Forward(post.recipients());
    }
    if (kind == Kind::kSound) {
      const auto cue = SoundCue(*wire);
      return Send(post, cue ? handler(*cue) : 0);
    }
    Particle(post, *wire);
  }

  // Particle follows one particle message: a create asks the handler and
  // keeps what it skipped, and the later messages about its index skip the
  // same recipients.
  void Particle(const NativePostFilter::Post& post, std::span<const uint8_t> wire) {
    engine::ParticleManager message;
    if (!ParseFramed(wire, message)) {
      return post.Forward(post.recipients());
    }
    const auto index = message.index();
    if (message.type() != kCreateParticle) {
      const auto found = kept.find(index);
      if (found == kept.end()) {
        return post.Forward(post.recipients());
      }
      const auto skipped = found->second;
      if (message.type() == kReleaseParticle) {
        kept.erase(found);
      }
      return Send(post, skipped);
    }

    // A create starts the index afresh.
    kept.erase(index);
    const auto cue = ParticleCue(message);
    const auto skipped = cue && active(Kind::kParticle) ? handler(*cue) : 0;
    if (skipped & post.recipients()[0]) {
      kept[index] = skipped;
    }
    Send(post, skipped);
  }

  // Send posts the message to its recipients outside skipped.
  static void Send(const NativePostFilter::Post& post, uint64_t skipped) {
    if (!(skipped & post.recipients()[0])) {
      return post.Forward(post.recipients());
    }
    auto rest = post.recipients();
    rest[0] &= ~skipped;
    if (std::ranges::any_of(rest, [](uint64_t word) { return word != 0; })) {
      post.Forward(rest);
    }
  }
};

uint32_t NativeEffectFilter::SoundName(std::string_view name) {
  return MurmurHash2(name, kSoundNameSeed);
}

std::optional<NativeEffectFilter::Cue> NativeEffectFilter::Decode(
    int32_t message_id, std::span<const uint8_t> serialized) {
  if (message_id == kSoundStartMessageId) {
    return SoundCue(serialized);
  }
  engine::ParticleManager message;
  if (message_id != kParticleManagerMessageId || !ParseFramed(serialized, message)) {
    return std::nullopt;
  }
  return ParticleCue(message);
}

NativeEffectFilter::NativeEffectFilter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NativeEffectFilter::NativeEffectFilter(NativeEffectFilter&&) noexcept = default;
NativeEffectFilter& NativeEffectFilter::operator=(NativeEffectFilter&&) noexcept = default;
NativeEffectFilter::~NativeEffectFilter() = default;

std::expected<NativeEffectFilter, std::string> NativeEffectFilter::Install(NativePostFilter& posts,
                                                                           Active active,
                                                                           Handler handler) {
  if (!active || !handler) {
    return std::unexpected("native effect filter handler is empty");
  }
  auto impl = std::make_unique<Impl>();
  impl->active = std::move(active);
  impl->handler = std::move(handler);
  impl->posts = &posts;
  for (const auto id : kCueIds) {
    auto registered =
        posts.Register(id, [filter = impl.get()](int32_t id, const NativePostFilter::Post& post) {
          filter->Filter(id, post);
        });
    if (!registered) {
      return std::unexpected(registered.error());
    }
    impl->ids.push_back(id);
  }
  return NativeEffectFilter(std::move(impl));
}

}  // namespace modlock::gameinterop
