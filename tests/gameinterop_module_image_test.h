#pragma once

// Test-only ModuleImage over a synthetic blob: every recorded pattern lands
// once at a known offset, mirroring how a live session scans server.dll.
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "modlock/gameinterop/game_symbols.h"
#include "modlock/gameinterop/signature.h"

namespace modlock::gameinterop::testing {

class FakeModuleImage : public ModuleImage {
 public:
  std::uintptr_t base() const override { return base_; }
  std::span<const uint8_t> image_bytes() const override { return bytes_; }

  // Add lays the pattern down at the next free slot (32-byte spacing) and
  // records its offset under id.
  void Add(std::string_view id, std::string_view pattern) {
    auto parsed = ParseSignature(std::string(id), pattern).value();
    while (bytes_.size() % 32 != 0) {
      bytes_.push_back(0xCC);
    }
    offsets_[std::string(id)] = bytes_.size();
    bytes_.insert(bytes_.end(), parsed.bytes.begin(), parsed.bytes.end());
  }

  // AddSignature lays the recorded GameSignatures() pattern for id.
  void AddSignature(std::string_view id) { Add(id, FindGameSignature(id)->pattern); }

  // PatchRelativeCall writes an E8 rel32 at id's offset + call_delta that
  // decodes to exactly `target` for this image's base.
  void PatchRelativeCall(std::string_view id, size_t call_delta, void* target) {
    const size_t site = offsets_[std::string(id)] + call_delta;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(target) -
                                               (base_ + static_cast<std::uintptr_t>(site) + 5));
    bytes_[site] = 0xE8;
    std::memcpy(bytes_.data() + site + 1, &rel, sizeof(rel));
  }

  // PatchRelativeLea writes a seven-byte REX.W LEA at id's offset + lea_delta
  // whose disp32 decodes to exactly `target` for this image's base.
  void PatchRelativeLea(std::string_view id, size_t lea_delta, void* target) {
    // Encode a RIP-relative LEA from the site to the target.
    const size_t site = offsets_[std::string(id)] + lea_delta;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(target) -
                                               (base_ + static_cast<std::uintptr_t>(site) + 7));
    bytes_[site] = 0x48;
    bytes_[site + 1] = 0x8D;
    bytes_[site + 2] = 0x05;
    std::memcpy(bytes_.data() + site + 3, &rel, sizeof(rel));
  }

  // OffsetOf returns the offset the probe's pattern was laid at, so a test
  // can assert a production-resolved pointer equals base + offset.
  size_t OffsetOf(std::string_view id) const { return offsets_.at(std::string(id)); }

  // Pad grows the image with int3 filler so relative sites past a short
  // pattern stay inside the image.
  void pad(size_t bytes = 4096) { bytes_.insert(bytes_.end(), bytes, 0xCC); }

  size_t size() const { return bytes_.size(); }

  // Seal fixes the base so `anchor` is the image's last byte, then freezes
  // the image. A relative call patched to a real test-binary function at the
  // anchor stays inside int32 reach and inside the image, as the resolver
  // requires of every relative target.
  void Seal(std::uintptr_t anchor) {
    base_ = anchor - static_cast<std::uintptr_t>(bytes_.size() - 1);
    sealed_ = true;
  }

 private:
  std::vector<uint8_t> bytes_;
  std::map<std::string, size_t> offsets_;
  std::uintptr_t base_ = 0;
  bool sealed_ = false;
};

}  // namespace modlock::gameinterop::testing
