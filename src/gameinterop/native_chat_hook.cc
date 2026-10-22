#include "modlock/gameinterop/native_chat_hook.h"

#include <google/protobuf/io/coded_stream.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <utility>

#include "modlock/gameinterop/connection_tracker.h"
#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"
#include "proto/modlock/chat.pb.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {
namespace {

constexpr int16_t kPlayerChatMessageId = 118;
constexpr int16_t kChatMessageId = 314;

constexpr std::array<std::string_view, 10> kPlayerChatAudiences{
    "Cstrike_Chat_All",     "Cstrike_Chat_AllDead", "Cstrike_Chat_AllSpec", "Cstrike_Chat_CT",
    "Cstrike_Chat_CT_Dead", "Cstrike_Chat_CT_Loc",  "Cstrike_Chat_T",       "Cstrike_Chat_T_Dead",
    "Cstrike_Chat_T_Loc",   "Cstrike_Chat_Spec"};

#if defined(_WIN32)
// Source SDK networksystem/inetworkmessages.h and tier1/bitbuf.h define the
// serialize slot and this bf_write. Installed message IDs are 32-bit.
// The engine serializes its own protobuf; only bytes cross the protobuf ABI.
struct BitWriter {
  uint8_t* data;
  int32_t bytes;
  int32_t bits;
  int32_t cursor = 0;
  const char* debug_name = "modlock chat";
  bool overflow = false;
  bool assert_overflow = false;
};
static_assert(offsetof(BitWriter, cursor) == 16);
static_assert(offsetof(BitWriter, debug_name) == 24);
static_assert(sizeof(BitWriter) == 40);

using Post = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                      unsigned long, int32_t);
Post g_original = nullptr;
void* g_messages = nullptr;
// g_serialize_slot is INetworkMessages::SerializeAbstract's resolved index.
size_t g_serialize_slot = 0;
NativeChatHook::Handler* g_handler = nullptr;
DWORD g_thread = 0;

// Preserve call-through for a dispatch already entering the old thunk.
void ClearThunkState() {
  g_handler = nullptr;
  g_messages = nullptr;
  g_thread = 0;
}

std::optional<NativeChatHook::Message> ReadChat(void* serializer, const void* native_message) {
  // Read the message id from the serializer; only chat is read.
  if (!serializer || !native_message || !g_messages) {
    return {};
  }
  int32_t id = 0;
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(),
                         static_cast<const uint8_t*>(serializer) + kSerializerMessageIdOffset, &id,
                         sizeof(id), &copied) ||
      copied != sizeof(id) || (id != kPlayerChatMessageId && id != kChatMessageId)) {
    return {};
  }

  // Serialize the message and decode it.
  std::array<uint8_t, 4096> bytes{};
  BitWriter writer{bytes.data(), static_cast<int32_t>(bytes.size()),
                   static_cast<int32_t>(bytes.size() * 8)};
  using Serialize = bool (*)(void*, BitWriter&, const void*);
  if (!NetworkMessagesMethod<Serialize>(g_messages, g_serialize_slot)(g_messages, writer,
                                                                      native_message) ||
      writer.overflow || writer.cursor <= 0 || writer.cursor > writer.bits || writer.cursor % 8) {
    std::fprintf(stderr, "[modlock] native chat serialization failed\n");
    return {};
  }
  return NativeChatHook::Decode(id, std::span(bytes.data(), writer.cursor / 8));
}

void PostThunk(void* self, int32_t split_screen, bool local_only, int32_t clients,
               const uint64_t* recipients, void* serializer, const void* message,
               unsigned long size, int32_t buffer) {
  // Decode while the native message remains borrowed. Native dispatch may
  // consume temporary storage; command handling happens after forwarding chat.
  const auto chat =
      g_handler && GetCurrentThreadId() == g_thread ? ReadChat(serializer, message) : std::nullopt;
  g_original(self, split_screen, local_only, clients, recipients, serializer, message, size,
             buffer);
  if (!chat) {
    return;
  }
  const auto sender = ConnectionTracker::StateForSlot(chat->slot);
  if (sender.occupied && sender.fully_connected && !sender.is_bot && sender.xuid && g_handler) {
    (*g_handler)(chat->slot, chat->text);
  }
}
#endif

}  // namespace

std::optional<NativeChatHook::Message> NativeChatHook::Decode(int16_t message_id,
                                                              std::span<const uint8_t> serialized) {
  // Read the length prefix of one chat message.
  if ((message_id != kPlayerChatMessageId && message_id != kChatMessageId) || serialized.empty() ||
      serialized.size() > 4096) {
    return std::nullopt;
  }
  google::protobuf::io::CodedInputStream input(serialized.data(),
                                               static_cast<int>(serialized.size()));
  uint32_t size = 0;
  if (!input.ReadVarint32(&size) || size > serialized.size() ||
      size != serialized.size() - input.CurrentPosition()) {
    return std::nullopt;
  }
  const auto* payload = serialized.data() + input.CurrentPosition();

  // A server chat message names its sender's slot.
  if (message_id == kChatMessageId) {
    engine::ChatMessage message;
    if (!message.ParseFromArray(payload, static_cast<int>(size)) || !message.has_text() ||
        message.player_slot() < 0 || message.player_slot() >= 64) {
      return std::nullopt;
    }
    return Message{message.player_slot(), message.text()};
  }

  // A player chat message must be chat to a known audience.
  engine::PlayerChatMessage message;
  if (!message.ParseFromArray(payload, static_cast<int>(size))) {
    return std::nullopt;
  }
  if (!message.chat() || message.entity_index() < 1 || message.entity_index() > 64) {
    return std::nullopt;
  }
  std::string_view audience = message.message_name();
  if (audience.starts_with('#')) {
    audience.remove_prefix(1);
  }
  if (std::ranges::find(kPlayerChatAudiences, audience) == kPlayerChatAudiences.end()) {
    return std::nullopt;
  }
  return Message{message.entity_index() - 1, message.text()};
}

struct NativeChatHook::Impl {
  Handler handler;
  // Restore dispatch and release the borrowed handler before destroying it.
  std::optional<ThunkOwner> owner;
};

NativeChatHook::NativeChatHook(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeChatHook::NativeChatHook(NativeChatHook&&) noexcept = default;
NativeChatHook& NativeChatHook::operator=(NativeChatHook&&) noexcept = default;
NativeChatHook::~NativeChatHook() = default;

std::expected<NativeChatHook, std::string> NativeChatHook::Install(Handler handler) {
#if defined(_WIN32)
  // Install once, resolving the event system and the message serializer.
  if (g_handler || !handler) {
    return std::unexpected("native chat hook already installed or empty");
  }
  const auto resolved = ResolveEngineInterface(L"engine2.dll", "GameEventSystemServerV001");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  void* events = *resolved;
  const auto messages = ResolveEngineInterface(L"networksystem.dll", "NetworkMessagesVersion001");
  if (!messages) {
    return std::unexpected(messages.error());
  }

  // Resolve the post and serialize slots.
  const auto post_slot = ResolveVirtualSlot("game-event-system.post-event");
  const auto serialize_slot = ResolveVirtualSlot("network-messages.serialize");
  if (!post_slot) {
    return std::unexpected(post_slot.error());
  }
  if (!serialize_slot) {
    return std::unexpected(serialize_slot.error());
  }

  // Hook the post slot.
  auto impl = std::make_unique<Impl>();
  impl->handler = std::move(handler);
  auto hook = VtableSlotHook::Install(events, *post_slot, reinterpret_cast<void*>(&PostThunk));
  if (!hook) {
    return std::unexpected(hook.error());
  }

  // Keep what the thunk needs.
  g_original = reinterpret_cast<Post>(hook->Original());
  g_messages = *messages;
  g_serialize_slot = *serialize_slot;
  impl->owner.emplace(std::move(*hook), &ClearThunkState);

  // Route chat on this thread to the handler.
  g_thread = GetCurrentThreadId();
  g_handler = &impl->handler;
  return NativeChatHook(std::move(impl));
#else
  (void)handler;
  return std::unexpected("native chat requires the Windows host");
#endif
}

}  // namespace modlock::gameinterop
