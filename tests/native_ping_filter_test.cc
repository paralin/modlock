#include "modlock/gameinterop/native_ping_filter.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

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

}  // namespace
}  // namespace modlock::gameinterop
