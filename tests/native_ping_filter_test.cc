#include "modlock/gameinterop/native_ping_filter.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include "src/gameinterop/engine/ping.pb.h"
#include "src/gameinterop/native_network_messages.h"

namespace modlock::gameinterop {
namespace {

std::vector<uint8_t> EncodePing(const google::protobuf::MessageLite& message) {
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

engine::PingMessage WheelPing(int32_t slot, uint32_t option) {
  engine::PingMessage message;
  message.mutable_ping_data()->set_sender_player_slot(slot);
  message.mutable_ping_data()->set_speech_concept(7);
  message.set_ping_wheel_option_id(option);
  return message;
}

TEST(NativePingFilter, DecodesAWheelPingToItsSenderAndOption) {
  const auto bytes = EncodePing(WheelPing(3, 12));
  const auto ping = NativePingFilter::Decode(kPingWheelMessageId, bytes);
  ASSERT_TRUE(ping);
  EXPECT_EQ(ping->slot, 3);
  EXPECT_EQ(ping->kind, NativePingFilter::Kind::kWheel);
  EXPECT_EQ(ping->option, 12U);
  EXPECT_EQ(ping->concept_value, 7);
}

TEST(NativePingFilter, ReadsTheOptionOnlyFromAWheelPing) {
  // Field 2 means something else in a map ping.
  const auto bytes = EncodePing(WheelPing(3, 12));
  const auto ping = NativePingFilter::Decode(kMapPingMessageId, bytes);
  ASSERT_TRUE(ping);
  EXPECT_EQ(ping->kind, NativePingFilter::Kind::kMap);
  EXPECT_EQ(ping->option, 0U);
}

TEST(NativePingFilter, RefusesOtherMessagesAndBadSenders) {
  const auto bytes = EncodePing(WheelPing(3, 12));
  EXPECT_FALSE(NativePingFilter::Decode(118, bytes));
  EXPECT_FALSE(NativePingFilter::Decode(kPingWheelMessageId, EncodePing(WheelPing(-1, 12))));
  EXPECT_FALSE(NativePingFilter::Decode(kPingWheelMessageId, EncodePing(engine::PingMessage{})));
  EXPECT_FALSE(
      NativePingFilter::Decode(kPingWheelMessageId, std::span(bytes.data(), bytes.size() - 1)));
}

engine::PingMessage ParseWire(std::span<const uint8_t> wire) {
  // Strip the length the filter frames each message with.
  google::protobuf::io::CodedInputStream input(wire.data(), static_cast<int>(wire.size()));
  uint32_t size = 0;
  EXPECT_TRUE(input.ReadVarint32(&size));
  EXPECT_EQ(size, wire.size() - input.CurrentPosition());
  engine::PingMessage message;
  EXPECT_TRUE(
      message.ParseFromArray(wire.data() + input.CurrentPosition(), static_cast<int>(size)));
  return message;
}

TEST(NativePingFilter, MutesTheVoiceLineAndKeepsTheMarker) {
  // The game's location (2) and animation (9) are fields this build does not
  // name; the marker needs them.
  auto message = WheelPing(3, 12);
  auto* common = message.mutable_ping_data();
  common->set_response_chosen("hero_ping_enemy_here_03");
  common->mutable_unknown_fields()->AddLengthDelimited(2, "location");
  common->mutable_unknown_fields()->AddVarint(9, 4);

  const auto voiceless = NativePingFilter::WithoutVoice(kPingWheelMessageId, EncodePing(message));
  ASSERT_TRUE(voiceless);
  const auto result = ParseWire(*voiceless);
  EXPECT_FALSE(result.ping_data().has_speech_concept());
  EXPECT_FALSE(result.ping_data().has_response_chosen());
  EXPECT_EQ(result.ping_data().sender_player_slot(), 3);
  EXPECT_EQ(result.ping_wheel_option_id(), 12U);
  const auto& kept = result.ping_data().unknown_fields();
  ASSERT_EQ(kept.field_count(), 2);
  EXPECT_EQ(kept.field(0).number(), 2);
  EXPECT_EQ(kept.field(0).length_delimited(), "location");
  EXPECT_EQ(kept.field(1).number(), 9);
  EXPECT_EQ(kept.field(1).varint(), 4U);

  // The rewrite still reads as the same player's ping, now with no line.
  const auto ping = NativePingFilter::Decode(kPingWheelMessageId, *voiceless);
  ASSERT_TRUE(ping);
  EXPECT_EQ(ping->slot, 3);
  EXPECT_EQ(ping->concept_value, 0);
}

TEST(NativePingFilter, RefusesToRewriteWhatItCannotRead) {
  const auto bytes = EncodePing(WheelPing(3, 12));
  EXPECT_FALSE(NativePingFilter::WithoutVoice(118, bytes));
  EXPECT_FALSE(
      NativePingFilter::WithoutVoice(kMapPingMessageId, std::span(bytes.data(), bytes.size() - 1)));
}

// FakePost records what a filter forwards of one post.
class FakePost final : public NativePostFilter::Post {
 public:
  explicit FakePost(std::vector<uint8_t> wire) : wire_(std::move(wire)) { all_[0] = 0b111; }

  const NativePostFilter::Recipients& recipients() const override { return all_; }

  std::optional<std::span<const uint8_t>> Serialize(std::span<uint8_t> bytes) const override {
    std::copy(wire_.begin(), wire_.end(), bytes.begin());
    return std::span<const uint8_t>(bytes.data(), wire_.size());
  }

  void Forward(const NativePostFilter::Recipients& to) const override { forwards.push_back(to); }

  bool ForwardCopy(const NativePostFilter::Recipients& to,
                   std::span<const uint8_t>) const override {
    copies.push_back(to);
    return true;
  }

  mutable std::vector<NativePostFilter::Recipients> forwards;
  mutable std::vector<NativePostFilter::Recipients> copies;

 private:
  std::vector<uint8_t> wire_;
  NativePostFilter::Recipients all_{};
};

TEST(NativePostFilter, RoutesOnlyTheIdsItsFiltersWant) {
  // A post no filter wants goes to every recipient after one lookup.
  auto posts = NativePostFilter::Create();
  const FakePost chat({});
  EXPECT_FALSE(posts.Routes(kPlayerChatMessageId));
  posts.Dispatch(kPlayerChatMessageId, chat);
  ASSERT_EQ(chat.forwards.size(), 1U);
  EXPECT_EQ(chat.forwards[0], chat.recipients());

  // A registered id reaches its filter, which decides who gets the post;
  // one id holds one filter.
  int seen = 0;
  ASSERT_TRUE(posts.Register(kSoundStartMessageId,
                             [&](int32_t, const NativePostFilter::Post&) { seen++; }));
  EXPECT_FALSE(posts.Register(kSoundStartMessageId, [](int32_t, const NativePostFilter::Post&) {}));
  const FakePost sound({});
  posts.Dispatch(kSoundStartMessageId, sound);
  EXPECT_EQ(seen, 1);
  EXPECT_TRUE(sound.forwards.empty());
  posts.Unregister(kSoundStartMessageId);
  EXPECT_FALSE(posts.Routes(kSoundStartMessageId));

  // The ping filter holds its four ids while it lives. A ping from a slot
  // that is no connected human reaches everyone whole, without the handler.
  bool asked = false;
  {
    auto pings = NativePingFilter::Install(posts, [&](const NativePingFilter::Ping&, uint64_t&) {
      asked = true;
      return true;
    });
    ASSERT_TRUE(pings) << pings.error();
    for (const auto id :
         {kMapPingMessageId, kPingWheelMessageId, kQuickResponseMessageId, kAbilityPingMessageId}) {
      EXPECT_TRUE(posts.Routes(id));
    }
    EXPECT_FALSE(NativePingFilter::Install(
        posts, [](const NativePingFilter::Ping&, uint64_t&) { return false; }));
    const FakePost ping(EncodePing(WheelPing(3, 12)));
    posts.Dispatch(kPingWheelMessageId, ping);
    EXPECT_FALSE(asked);
    ASSERT_EQ(ping.forwards.size(), 1U);
    EXPECT_EQ(ping.forwards[0], ping.recipients());
    EXPECT_TRUE(ping.copies.empty());
  }
  EXPECT_FALSE(posts.Routes(kPingWheelMessageId));
}

}  // namespace
}  // namespace modlock::gameinterop
