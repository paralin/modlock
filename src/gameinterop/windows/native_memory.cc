#include "modlock/gameinterop/native_memory.h"

#include <windows.h>

namespace modlock::gameinterop {

bool ReadNative(const void* source, void* out, size_t size) {
  SIZE_T count = 0;
  return ReadProcessMemory(GetCurrentProcess(), source, out, size, &count) && count == size;
}

bool WriteNative(void* target, const void* source, size_t size) {
  SIZE_T count = 0;
  return WriteProcessMemory(GetCurrentProcess(), target, source, size, &count) && count == size;
}

}  // namespace modlock::gameinterop
