#include "modlock/gameinterop/native_chat_hook.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>

#include "proto/modlock/chat.pb.h"

namespace modlock::gameinterop {
namespace {

// Recorded from CUserMessageSayText2::Serialize in the native Windows host.
// Controller 1 sent /ready through console all chat.
constexpr std::array<uint8_t, 40> kHumanReady{
    0x27, 0x08, 0x01, 0x10, 0x01, 0x1a, 0x10, 0x43, 0x73, 0x74, 0x72, 0x69, 0x6b, 0x65,
    0x5f, 0x43, 0x68, 0x61, 0x74, 0x5f, 0x41, 0x6c, 0x6c, 0x22, 0x03, 0x64, 0x64, 0x6b,
    0x2a, 0x06, 0x2f, 0x72, 0x65, 0x61, 0x64, 0x79, 0x32, 0x00, 0x3a, 0x00};

std::vector<uint8_t> EncodeChat(const google::protobuf::MessageLite& message) {
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

TEST(NativeChatHook, DecodesRecordedHumanAllChatToItsClientSlot) {
  // A recorded all chat names its sender's slot; another id refuses it.
  const auto chat = NativeChatHook::Decode(118, kHumanReady);
  ASSERT_TRUE(chat);
  EXPECT_EQ(chat->slot, 0);
  EXPECT_EQ(chat->text, "/ready");
  EXPECT_FALSE(NativeChatHook::Decode(314, kHumanReady));
}

TEST(NativeChatHook, DecodesGlobalAndTeamGameChatToTheSenderSlot) {
  engine::ChatMessage message;
  for (const bool all_chat : {true, false}) {
    message.set_all_chat(all_chat);
    for (const int slot : {0, 1}) {
      message.set_player_slot(slot);
      for (const auto text : {"/ready", "/again", "/souls 40000"}) {
        message.set_text(text);
        const auto chat = NativeChatHook::Decode(314, EncodeChat(message));
        ASSERT_TRUE(chat);
        EXPECT_EQ(chat->slot, slot);
        EXPECT_EQ(chat->text, text);
      }
    }
  }
}

TEST(NativeChatHook, DecodesConsoleTeamChatWhileAliveOrDead) {
  // Every team and spectator audience decodes, with or without its prefix.
  engine::PlayerChatMessage message;
  message.set_entity_index(2);
  message.set_chat(true);
  message.set_text("/again");
  for (const auto audience :
       {"Cstrike_Chat_CT", "Cstrike_Chat_CT_Dead", "Cstrike_Chat_CT_Loc", "Cstrike_Chat_T",
        "Cstrike_Chat_T_Dead", "Cstrike_Chat_T_Loc", "Cstrike_Chat_Spec"}) {
    for (const auto prefix : {"", "#"}) {
      message.set_message_name(std::string(prefix) + audience);
      const auto chat = NativeChatHook::Decode(118, EncodeChat(message));
      ASSERT_TRUE(chat);
      EXPECT_EQ(chat->slot, 1);
      EXPECT_EQ(chat->text, "/again");
    }
  }
}

TEST(NativeChatHook, RejectsInvalidGameChatSendersAndIncompleteMessages) {
  // A sender outside the slots is refused.
  engine::ChatMessage message;
  message.set_text("/ready");
  message.set_all_chat(false);
  for (const int slot : {-1, 64}) {
    message.set_player_slot(slot);
    EXPECT_FALSE(NativeChatHook::Decode(314, EncodeChat(message)));
  }

  // A truncated message is refused.
  message.set_player_slot(0);
  const auto bytes = EncodeChat(message);
  EXPECT_FALSE(NativeChatHook::Decode(314, std::span(bytes).first(bytes.size() - 1)));
}

TEST(NativeChatHook, RejectsServerChatOtherAudiencesAndIncompleteMessages) {
  // A server sender is refused.
  auto server = kHumanReady;
  server[2] = 0;
  EXPECT_FALSE(NativeChatHook::Decode(118, server));

  // An unknown audience or a truncated message is refused.
  auto audience = kHumanReady;
  audience[20] = 'X';
  EXPECT_FALSE(NativeChatHook::Decode(118, audience));
  EXPECT_FALSE(NativeChatHook::Decode(118, std::span(kHumanReady).first(30)));
}

}  // namespace
}  // namespace modlock::gameinterop
