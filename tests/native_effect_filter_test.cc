#include "modlock/gameinterop/native_effect_filter.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include "src/gameinterop/engine/particle.pb.h"
#include "src/gameinterop/engine/sound.pb.h"
#include "src/gameinterop/native_network_messages.h"

namespace modlock::gameinterop {
namespace {

std::vector<uint8_t> Frame(const google::protobuf::MessageLite& message) {
  // Frame the message with its length, as the serializer does.
  const auto size = message.ByteSizeLong();
  std::vector<uint8_t> bytes(size + 5);
  size_t written = 0;
  {
    google::protobuf::io::ArrayOutputStream stream(bytes.data(), static_cast<int>(bytes.size()));
    google::protobuf::io::CodedOutputStream output(&stream);
    output.WriteVarint32(static_cast<uint32_t>(size));
    EXPECT_TRUE(message.SerializeToCodedStream(&output));
    written = output.ByteCount();
  }
  bytes.resize(written);
  return bytes;
}

// The parry particle and the parrier's pawn from the Forge capture: a
// networked handle whose low 14 bits are entity 3160.
constexpr uint64_t kParryParticle = 0xea4556457abca994;
constexpr uint32_t kParrierHandle = 1444952;

std::vector<uint8_t> Particle(int32_t type, uint32_t index) {
  engine::ParticleManager message;
  message.set_type(type);
  message.set_index(index);
  if (type == 0) {
    message.mutable_create_particle()->set_particle_name_index(kParryParticle);
    message.mutable_create_particle()->set_entity_handle(kParrierHandle);
  }
  return Frame(message);
}

std::vector<uint8_t> Sound(std::string_view name, int32_t source) {
  engine::SoundStart message;
  message.set_soundevent_guid(7);
  message.set_soundevent_hash(NativeEffectFilter::SoundName(name));
  message.set_source_entity_index(source);
  return Frame(message);
}

// FakePost records what the filter forwards of one post to slots 0, 1 and 2.
class FakePost final : public NativePostFilter::Post {
 public:
  explicit FakePost(std::vector<uint8_t> wire) : wire_(std::move(wire)) { all_[0] = 0b111; }

  const NativePostFilter::Recipients& recipients() const override { return all_; }

  std::optional<std::span<const uint8_t>> Serialize(std::span<uint8_t> bytes) const override {
    serialized = true;
    std::copy(wire_.begin(), wire_.end(), bytes.begin());
    return std::span<const uint8_t>(bytes.data(), wire_.size());
  }

  void Forward(const NativePostFilter::Recipients& to) const override { forwards.push_back(to); }

  bool ForwardCopy(const NativePostFilter::Recipients&, std::span<const uint8_t>) const override {
    ADD_FAILURE() << "the effect filter never rewrites a message";
    return false;
  }

  // Got returns the one mask the post went to, or nothing when it went to
  // no one.
  std::optional<uint64_t> Got() const {
    EXPECT_LE(forwards.size(), 1U);
    if (forwards.empty()) {
      return std::nullopt;
    }
    return forwards[0][0];
  }

  mutable bool serialized = false;
  mutable std::vector<NativePostFilter::Recipients> forwards;

 private:
  std::vector<uint8_t> wire_;
  NativePostFilter::Recipients all_{};
};

TEST(NativeEffectFilter, DecodesTheParticleAndSoundACueStarts) {
  const auto particle = NativeEffectFilter::Decode(kParticleManagerMessageId, Particle(0, 4));
  ASSERT_TRUE(particle);
  EXPECT_EQ(particle->kind, NativeEffectFilter::Kind::kParticle);
  EXPECT_EQ(particle->name, kParryParticle);
  EXPECT_EQ(particle->source, 3160);

  const auto sound =
      NativeEffectFilter::Decode(kSoundStartMessageId, Sound("Player.Melee.Parry.Shared", 3160));
  ASSERT_TRUE(sound);
  EXPECT_EQ(sound->kind, NativeEffectFilter::Kind::kSound);
  EXPECT_EQ(sound->name, NativeEffectFilter::SoundName("player.melee.parry.shared"));
  EXPECT_EQ(sound->source, 3160);

  // An update names no cue, and nor do bytes the filter cannot read.
  EXPECT_FALSE(NativeEffectFilter::Decode(kParticleManagerMessageId, Particle(1, 4)));
  const auto bytes = Particle(0, 4);
  EXPECT_FALSE(NativeEffectFilter::Decode(kParticleManagerMessageId,
                                          std::span(bytes.data(), bytes.size() - 1)));
  EXPECT_FALSE(NativeEffectFilter::Decode(kPlayerChatMessageId, bytes));
}

TEST(NativeEffectFilter, KeepsAParticleFromItsViewersUntilItsIndexIsReleased) {
  // The rule keeps the parry particle from slot 1.
  auto posts = NativePostFilter::Create();
  std::vector<NativeEffectFilter::Cue> asked;
  auto filter = NativeEffectFilter::Install(
      posts, [](NativeEffectFilter::Kind) { return true; },
      [&](const NativeEffectFilter::Cue& cue) -> uint64_t {
        asked.push_back(cue);
        return cue.name == kParryParticle ? 0b010 : 0;
      });
  ASSERT_TRUE(filter) << filter.error();
  const auto post = [&](std::vector<uint8_t> wire) {
    const FakePost message(std::move(wire));
    posts.Dispatch(kParticleManagerMessageId, message);
    return message.Got();
  };

  // The create and every later message about its index skip slot 1.
  EXPECT_EQ(post(Particle(0, 4)), 0b101U);
  EXPECT_EQ(post(Particle(1, 4)), 0b101U);
  EXPECT_EQ(post(Particle(9, 4)), 0b101U);
  ASSERT_EQ(asked.size(), 1U);
  EXPECT_EQ(asked[0].source, 3160);

  // After the release the index belongs to whatever the server makes next,
  // and an index no create skipped reaches everyone.
  EXPECT_EQ(post(Particle(1, 4)), 0b111U);
  EXPECT_EQ(post(Particle(1, 5)), 0b111U);
}

TEST(NativeEffectFilter, KeepsASoundFromItsViewersAndLetsTheRestThrough) {
  // The rule keeps the parry sound from slots 0 and 2.
  auto posts = NativePostFilter::Create();
  const auto parry = NativeEffectFilter::SoundName("player.melee.parry.shared");
  auto filter = NativeEffectFilter::Install(
      posts, [](NativeEffectFilter::Kind) { return true; },
      [&](const NativeEffectFilter::Cue& cue) -> uint64_t {
        return cue.name == parry ? 0b101 : 0;
      });
  ASSERT_TRUE(filter) << filter.error();
  const FakePost hidden(Sound("player.melee.parry.shared", 3160));
  posts.Dispatch(kSoundStartMessageId, hidden);
  EXPECT_EQ(hidden.Got(), 0b010U);
  const FakePost other(Sound("status.stunned", 3160));
  posts.Dispatch(kSoundStartMessageId, other);
  EXPECT_EQ(other.Got(), 0b111U);

  // A message the filter cannot read goes to everyone.
  const FakePost unreadable({0x05, 0x01});
  posts.Dispatch(kSoundStartMessageId, unreadable);
  EXPECT_EQ(unreadable.Got(), 0b111U);

  // A rule that skips everyone posts nothing.
  auto everyone = NativePostFilter::Create();
  auto all = NativeEffectFilter::Install(
      everyone, [](NativeEffectFilter::Kind) { return true; },
      [](const NativeEffectFilter::Cue&) -> uint64_t { return ~uint64_t{0}; });
  ASSERT_TRUE(all) << all.error();
  const FakePost silent(Sound("player.melee.parry.shared", 3160));
  everyone.Dispatch(kSoundStartMessageId, silent);
  EXPECT_EQ(silent.Got(), std::nullopt);
}

TEST(NativeEffectFilter, PassesAKindNoRuleNamesWithoutReadingIt) {
  auto posts = NativePostFilter::Create();
  bool asked = false;
  {
    auto filter = NativeEffectFilter::Install(
        posts, [](NativeEffectFilter::Kind) { return false; },
        [&](const NativeEffectFilter::Cue&) -> uint64_t {
          asked = true;
          return ~uint64_t{0};
        });
    ASSERT_TRUE(filter) << filter.error();
    EXPECT_TRUE(posts.Routes(kParticleManagerMessageId));
    EXPECT_TRUE(posts.Routes(kSoundStartMessageId));
    for (const auto id : {kParticleManagerMessageId, kSoundStartMessageId}) {
      const FakePost message(id == kSoundStartMessageId ? Sound("status.stunned", 1)
                                                        : Particle(0, 4));
      posts.Dispatch(id, message);
      EXPECT_FALSE(message.serialized);
      EXPECT_EQ(message.Got(), 0b111U);
    }
  }
  EXPECT_FALSE(asked);
  EXPECT_FALSE(posts.Routes(kParticleManagerMessageId));
  EXPECT_FALSE(posts.Routes(kSoundStartMessageId));
}

}  // namespace
}  // namespace modlock::gameinterop
