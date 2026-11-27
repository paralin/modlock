#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "modlock/export.h"

namespace modlock::gameinterop {

// NativePostFilter owns the server's one replacement of
// IGameEventSystem::PostEventAbstract, through which every user message and
// game event the server sends passes, and hands each post to the filter
// registered for its message id. A post no filter is registered for goes
// through after one lookup, as does every post off the installing engine
// thread and every post whose recipient mask it cannot copy. A filter decides
// which recipients get the message, or a rewritten copy of it; it never
// changes what the message did to the game. Destruction restores dispatch
// after the engine thread has quiesced.
class MODLOCK_API NativePostFilter {
 public:
  // kMaxRecipientWords bounds the recipient mask a post may carry, one bit a
  // client slot.
  static constexpr size_t kMaxRecipientWords = 4;
  using Recipients = std::array<uint64_t, kMaxRecipientWords>;

  // Post is one message the server is posting, borrowed for one filter call.
  class Post {
   public:
    virtual ~Post() = default;

    // recipients is the mask of client slots the server posts to.
    virtual const Recipients& recipients() const = 0;

    // Serialize writes the message's length-prefixed wire bytes into bytes
    // and returns them, or nothing when the engine cannot serialize it.
    virtual std::optional<std::span<const uint8_t>> Serialize(std::span<uint8_t> bytes) const = 0;

    // Forward posts the message to the recipients in to.
    virtual void Forward(const Recipients& to) const = 0;

    // ForwardCopy posts a message of the same type parsed from wire to the
    // recipients in to. It returns false, posting nothing, when the engine
    // cannot parse wire.
    virtual bool ForwardCopy(const Recipients& to, std::span<const uint8_t> wire) const = 0;
  };

  // Filter decides one post of its message id: it forwards what each
  // recipient gets, and a recipient it forwards nothing to gets nothing.
  using Filter = std::function<void(int32_t id, const Post& post)>;

  // Create returns a filter that hooks nothing, whose Dispatch a test drives.
  static NativePostFilter Create();

  // Install borrows GameEventSystemServerV001 after world initialization and
  // routes the posts made on the calling thread. One may be installed at a
  // time.
  static std::expected<NativePostFilter, std::string> Install();

  NativePostFilter(NativePostFilter&&) noexcept;
  NativePostFilter& operator=(NativePostFilter&&) noexcept;
  ~NativePostFilter();

  // Register routes posts of id to filter until Unregister. An id has at
  // most one filter. Register and Unregister run on the installing thread,
  // and never from inside a filter.
  std::expected<void, std::string> Register(int32_t id, Filter filter);

  // Unregister lets posts of id through again.
  void Unregister(int32_t id);

  // Dispatch hands post to the filter registered for id, or forwards it to
  // every recipient when none is.
  void Dispatch(int32_t id, const Post& post) const;

  // Routes reports whether a filter is registered for id: the one lookup a
  // post no filter wants costs.
  [[nodiscard]] bool Routes(int32_t id) const;

  // Impl holds the routes and the hook; the hook's thunk reads it.
  struct Impl;

 private:
  explicit NativePostFilter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlock::gameinterop
