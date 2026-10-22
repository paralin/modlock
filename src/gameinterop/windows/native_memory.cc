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

void* AllocateNative(size_t size) {
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  if (tier0 == nullptr) {
    return nullptr;
  }
  auto alloc = reinterpret_cast<void* (*)(size_t)>(GetProcAddress(tier0, "MemAlloc_AllocFunc"));
  return alloc == nullptr ? nullptr : alloc(size);
}

}  // namespace modlock::gameinterop
