#include "modlock/gameinterop/native_sound.h"

#include <cmath>

#include "modlock/gameinterop/game_symbols.h"

namespace modlock::gameinterop {

std::expected<NativeSound, std::string> NativeSound::TryCreate(const ModuleImage& server) {
#if defined(_WIN32)
  // Resolve the native CBaseEntity::EmitSoundParams entry point.
  auto target = ResolveSignature(server, "entity.emit-sound");
  if (!target) {
    return std::unexpected(target.error());
  }
  return NativeSound(reinterpret_cast<EmitFunction>(*target));
#else
  (void)server;
  return std::unexpected("native sound requires the Windows host build");
#endif
}

bool NativeSound::Emit(void* entity, const char* sound_name, int pitch, float volume,
                       float delay) const {
  if (!entity || !sound_name || !*sound_name || pitch <= 0 || !std::isfinite(volume) ||
      volume < 0 || !std::isfinite(delay) || delay < 0) {
    return false;
  }
  emit_(entity, sound_name, pitch, volume, delay);
  return true;
}

}  // namespace modlock::gameinterop
