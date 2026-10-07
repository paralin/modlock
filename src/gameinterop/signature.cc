#include "modlock/gameinterop/signature.h"

#include <array>
#include <cctype>
#include <cstring>
#include <optional>

namespace modlock::gameinterop {
namespace {

// NibbleValue decodes one hex digit, or nullopt for anything else.
std::optional<int> NibbleValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return std::nullopt;
}

// kSampleStride spaces the bytes RarestByte reads; a prime keeps the sample
// from falling into step with aligned structures.
constexpr size_t kSampleStride = 61;

// RarestByte returns the index of sig's literal byte that occurs least often
// in a sample of data, or nullopt when every byte is a wildcard.
std::optional<size_t> RarestByte(std::span<const uint8_t> data, const Signature& sig) {
  std::array<uint32_t, 256> counts{};
  for (size_t i = 0; i < data.size(); i += kSampleStride) counts[data[i]]++;
  std::optional<size_t> rarest;
  for (size_t k = 0; k < sig.bytes.size(); k++) {
    if (sig.wildcard[k]) continue;
    if (!rarest || counts[sig.bytes[k]] < counts[sig.bytes[*rarest]]) rarest = k;
  }
  return rarest;
}

// Matches reports whether sig matches data at start, which leaves room for it.
bool Matches(std::span<const uint8_t> data, size_t start, const Signature& sig) {
  for (size_t k = 0; k < sig.bytes.size(); k++) {
    if (!sig.wildcard[k] && data[start + k] != sig.bytes[k]) return false;
  }
  return true;
}

}  // namespace

std::expected<Signature, std::string> ParseSignature(std::string id, std::string_view pattern) {
  Signature sig{.id = std::move(id)};
  for (size_t i = 0; i < pattern.size();) {
    unsigned char c = static_cast<unsigned char>(pattern[i]);
    if (std::isspace(c)) {
      i++;
      continue;
    }
    if (pattern[i] == '?') {
      // One wildcard byte per token; the signature database writes it as
      // '?' or '??'. Consume the whole run of question marks.
      while (i < pattern.size() && pattern[i] == '?') {
        i++;
      }
      sig.bytes.push_back(0);
      sig.wildcard.push_back(true);
      continue;
    }
    auto high = NibbleValue(pattern[i]);
    auto low = i + 1 < pattern.size() ? NibbleValue(pattern[i + 1]) : std::nullopt;
    if (!high || !low) {
      return std::unexpected("signature '" + sig.id + "': expected hex byte at offset " +
                             std::to_string(i));
    }
    sig.bytes.push_back(static_cast<uint8_t>(*high * 16 + *low));
    sig.wildcard.push_back(false);
    i += 2;
  }
  if (sig.bytes.empty()) {
    return std::unexpected("signature '" + sig.id + "': empty pattern");
  }
  return sig;
}

std::vector<size_t> SignatureScan(std::span<const uint8_t> data, const Signature& sig) {
  std::vector<size_t> hits;
  if (sig.bytes.empty() || sig.bytes.size() > data.size()) return hits;
  const size_t last_start = data.size() - sig.bytes.size();

  const auto pivot = RarestByte(data, sig);
  if (!pivot) {
    // A pattern of wildcards alone matches everywhere it fits.
    hits.resize(last_start + 1);
    for (size_t start = 0; start <= last_start; start++) hits[start] = start;
    return hits;
  }

  // memchr finds each place the pattern's rarest byte occurs far faster than
  // a byte-by-byte scan; the whole pattern is checked only there.
  const uint8_t* first = data.data() + *pivot;
  const uint8_t* end = first + last_start + 1;
  for (const uint8_t* p = first; p < end; p++) {
    p = static_cast<const uint8_t*>(std::memchr(p, sig.bytes[*pivot], end - p));
    if (p == nullptr) break;
    const auto start = static_cast<size_t>(p - first);
    if (Matches(data, start, sig)) hits.push_back(start);
  }
  return hits;
}

}  // namespace modlock::gameinterop
