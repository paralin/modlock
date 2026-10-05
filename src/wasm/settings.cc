#include "wasm/settings.h"

#include <array>
#include <charconv>
#include <cmath>

namespace modlock::wasm {
namespace {

// NumberText writes the shortest text that reads back as number.
std::string NumberText(double number) {
  std::array<char, 32> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), number);
  return std::string(text.data(), written.ptr);
}

}  // namespace

const Setting* FindSetting(const Manifest& manifest, std::string_view key) {
  for (const auto& setting : manifest.settings()) {
    if (setting.key() == key) return &setting;
  }
  return nullptr;
}

std::optional<std::string> Canonical(const Setting& setting, std::string_view value) {
  switch (setting.kind()) {
    case Setting::KIND_CHOICE:
      for (const auto& choice : setting.choices()) {
        if (choice.value() == value) return std::string(value);
      }
      return std::nullopt;
    case Setting::KIND_SWITCH:
      if (value == "true" || value == "false") return std::string(value);
      return std::nullopt;
    case Setting::KIND_NUMBER: {
      // Accept a number within the bounds and on a step.
      double number = 0;
      const auto* end = value.data() + value.size();
      if (auto parsed = std::from_chars(value.data(), end, number);
          parsed.ec != std::errc() || parsed.ptr != end || std::isnan(number) ||
          number < setting.min() || number > setting.max()) {
        return std::nullopt;
      }
      if (const double step = setting.step(); step > 0) {
        const double steps = (number - setting.min()) / step;
        if (std::abs(steps - std::round(steps)) > 1e-9) return std::nullopt;
      }
      return NumberText(number);
    }
    default:
      return std::nullopt;
  }
}

std::string DefaultValue(const Setting& setting) {
  if (auto value = Canonical(setting, setting.default_value())) return *value;
  switch (setting.kind()) {
    case Setting::KIND_CHOICE:
      return setting.choices().empty() ? std::string() : setting.choices(0).value();
    case Setting::KIND_SWITCH:
      return "false";
    case Setting::KIND_NUMBER:
      return NumberText(setting.min());
    default:
      return {};
  }
}

}  // namespace modlock::wasm
