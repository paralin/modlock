#include "modlock/gameinterop/native_memory.h"

namespace modlock::gameinterop {

bool ReadNative(const void*, void*, size_t) { return false; }

bool WriteNative(void*, const void*, size_t) { return false; }

}  // namespace modlock::gameinterop
