#include "modlock/gameinterop/native_memory.h"

#include <windows.h>

#include <cstring>

namespace modlock::gameinterop {

namespace {

// Copy stays a call so the access violation is raised inside the __try range.
// Clang catches only faults raised by calls in the guarded body.
[[gnu::noinline]] void Copy(void* target, const void* source, size_t size) {
  std::memcpy(target, source, size);
}

int FilterAccessViolation(DWORD code) {
  return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

bool GuardedCopy(void* target, const void* source, size_t size) {
  __try {
    Copy(target, source, size);
  } __except (FilterAccessViolation(GetExceptionCode())) {
    return false;
  }
  return true;
}

}  // namespace

bool ReadNative(const void* source, void* out, size_t size) {
  return GuardedCopy(out, source, size);
}

bool WriteNative(void* target, const void* source, size_t size) {
  return GuardedCopy(target, source, size);
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
