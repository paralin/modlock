#include "modlock/gameinterop/native_memory.h"

namespace modlock::gameinterop {

bool ReadNative(const void*, void*, size_t) {
  return false;
}

bool WriteNative(void*, const void*, size_t) {
  return false;
}

void* AllocateNative(size_t) {
  return nullptr;
}

const char* InternGlobalSymbol(const char*) {
  return nullptr;
}

}  // namespace modlock::gameinterop
