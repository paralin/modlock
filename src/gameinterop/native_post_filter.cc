#include "modlock/gameinterop/native_post_filter.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "modlock/gameinterop/mapped_module_image.h"
#include "modlock/gameinterop/thunk_owner.h"
#include "modlock/gameinterop/virtual_slots.h"
#include "native_network_messages.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace modlock::gameinterop {

struct NativePostFilter::Impl {
  // routes holds each registered id's filter. The engine thread reads it on
  // every post, so a filter registers and unregisters on that thread, and
  // never from inside a post.
  std::unordered_map<int32_t, Filter> routes;
  // owner restores dispatch before routes is destroyed.
  std::optional<ThunkOwner> owner;

  // Find returns id's filter, or null when none is registered.
  const Filter* Find(int32_t id) const {
    const auto found = routes.find(id);
    return found == routes.end() ? nullptr : &found->second;
  }
};

namespace {

#if defined(_WIN32)
using PostEvent = void (*)(void*, int32_t, bool, int32_t, const uint64_t*, void*, const void*,
                           unsigned long, int32_t);
PostEvent g_original = nullptr;
void* g_messages = nullptr;
// g_serialize_slot and g_unserialize_slot are INetworkMessages'
// SerializeAbstract and UnserializeAbstract resolved indexes.
size_t g_serialize_slot = 0;
size_t g_unserialize_slot = 0;
const NativePostFilter::Impl* g_filter = nullptr;
DWORD g_thread = 0;

// Preserve call-through for a dispatch already entering the old thunk.
void ClearThunkState() {
  g_filter = nullptr;
  g_messages = nullptr;
  g_thread = 0;
}

// EnginePost is one post the engine is making, with its recipients' mask
// copied so a filter can split it.
class EnginePost final : public NativePostFilter::Post {
 public:
  EnginePost(void* self, int32_t split_screen, bool local_only, int32_t clients,
             const uint64_t* recipients, int32_t words, void* serializer, const void* message,
             unsigned long size, int32_t buffer)
      : self_(self),
        split_screen_(split_screen),
        local_only_(local_only),
        clients_(clients),
        serializer_(serializer),
        message_(message),
        size_(size),
        buffer_(buffer) {
    std::copy_n(recipients, words, recipients_.begin());
  }

  const NativePostFilter::Recipients& recipients() const override { return recipients_; }

  std::optional<std::span<const uint8_t>> Serialize(std::span<uint8_t> bytes) const override {
    const auto size = SerializeNetworkMessage(g_messages, g_serialize_slot, message_, bytes);
    if (!size) {
      return std::nullopt;
    }
    return std::span<const uint8_t>(bytes.data(), *size);
  }

  void Forward(const NativePostFilter::Recipients& to) const override { Post(to, message_); }

  bool ForwardCopy(const NativePostFilter::Recipients& to,
                   std::span<const uint8_t> wire) const override {
    const auto copy = ParseNetworkMessage(g_messages, g_unserialize_slot, serializer_, wire);
    if (!copy) {
      return false;
    }
    Post(to, copy.get());
    return true;
  }

 private:
  void Post(const NativePostFilter::Recipients& to, const void* message) const {
    g_original(self_, split_screen_, local_only_, clients_, to.data(), serializer_, message, size_,
               buffer_);
  }

  void* self_;
  int32_t split_screen_;
  bool local_only_;
  int32_t clients_;
  NativePostFilter::Recipients recipients_{};
  void* serializer_;
  const void* message_;
  unsigned long size_;
  int32_t buffer_;
};

// PostThunk routes a post on the installing thread whose id a filter wants,
// and sends every other post straight through: a post off that thread, one
// without a message, one whose mask the filter cannot copy, and one no filter
// is registered for, which costs one lookup.
void PostThunk(void* self, int32_t split_screen, bool local_only, int32_t clients,
               const uint64_t* recipients, void* serializer, const void* message,
               unsigned long size, int32_t buffer) {
  const int32_t words = clients > 0 ? (clients + 63) / 64 : 0;
  const auto through = [&] {
    g_original(self, split_screen, local_only, clients, recipients, serializer, message, size,
               buffer);
  };
  if (!g_filter || GetCurrentThreadId() != g_thread || !recipients || words == 0 ||
      words > static_cast<int32_t>(NativePostFilter::kMaxRecipientWords) || !serializer ||
      !message) {
    return through();
  }
  const auto id = NetworkMessageIdOf(serializer);
  const auto* filter = g_filter->Find(id);
  if (!filter) {
    return through();
  }
  const EnginePost post(self, split_screen, local_only, clients, recipients, words, serializer,
                        message, size, buffer);
  (*filter)(id, post);
}
#endif

}  // namespace

NativePostFilter::NativePostFilter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NativePostFilter::NativePostFilter(NativePostFilter&&) noexcept = default;
NativePostFilter& NativePostFilter::operator=(NativePostFilter&&) noexcept = default;
NativePostFilter::~NativePostFilter() = default;

NativePostFilter NativePostFilter::Create() {
  return NativePostFilter(std::make_unique<Impl>());
}

std::expected<NativePostFilter, std::string> NativePostFilter::Install() {
#if defined(_WIN32)
  // Install once, resolving the event system and the message serializer.
  if (g_filter) {
    return std::unexpected("native post filter already installed");
  }
  const auto events = ResolveEngineInterface(L"engine2.dll", "GameEventSystemServerV001");
  if (!events) {
    return std::unexpected(events.error());
  }
  const auto messages = ResolveEngineInterface(L"networksystem.dll", "NetworkMessagesVersion001");
  if (!messages) {
    return std::unexpected(messages.error());
  }
  const auto post_slot = ResolveVirtualSlot("game-event-system.post-event");
  if (!post_slot) {
    return std::unexpected(post_slot.error());
  }
  const auto serialize_slot = ResolveVirtualSlot("network-messages.serialize");
  if (!serialize_slot) {
    return std::unexpected(serialize_slot.error());
  }
  const auto unserialize_slot = ResolveVirtualSlot("network-messages.unserialize");
  if (!unserialize_slot) {
    return std::unexpected(unserialize_slot.error());
  }

  // Hook the post slot and keep what the thunk needs.
  auto impl = std::make_unique<Impl>();
  auto hook = VtableSlotHook::Install(*events, *post_slot, reinterpret_cast<void*>(&PostThunk));
  if (!hook) {
    return std::unexpected(hook.error());
  }
  g_original = reinterpret_cast<PostEvent>(hook->Original());
  g_messages = *messages;
  g_serialize_slot = *serialize_slot;
  g_unserialize_slot = *unserialize_slot;
  impl->owner.emplace(std::move(*hook), &ClearThunkState);

  // Route posts on this thread through the implementation, whose address a
  // move keeps.
  g_thread = GetCurrentThreadId();
  g_filter = impl.get();
  return NativePostFilter(std::move(impl));
#else
  return std::unexpected("the native post filter requires the Windows host");
#endif
}

std::expected<void, std::string> NativePostFilter::Register(int32_t id, Filter filter) {
  if (!filter) {
    return std::unexpected("a post filter is empty");
  }
  if (!impl_->routes.try_emplace(id, std::move(filter)).second) {
    return std::unexpected("message " + std::to_string(id) + " already has a post filter");
  }
  return {};
}

void NativePostFilter::Unregister(int32_t id) {
  impl_->routes.erase(id);
}

void NativePostFilter::Dispatch(int32_t id, const Post& post) const {
  const auto* filter = impl_->Find(id);
  if (!filter) {
    return post.Forward(post.recipients());
  }
  (*filter)(id, post);
}

bool NativePostFilter::Routes(int32_t id) const {
  return impl_->Find(id) != nullptr;
}

}  // namespace modlock::gameinterop
