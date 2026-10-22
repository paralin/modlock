#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>

#include "modlock/host_app/stdio_guard.h"

namespace modlock::host_app {
namespace {

const char* NullDevicePath() {
#ifdef _WIN32
  return "NUL";
#else
  return "/dev/null";
#endif
}

std::FILE* StreamFile(StdioStream stream) {
  switch (stream) {
    case StdioStream::kInput:
      return stdin;
    case StdioStream::kOutput:
      return stdout;
    case StdioStream::kError:
      return stderr;
  }
  return nullptr;
}

const char* StreamMode(StdioStream stream) {
  return stream == StdioStream::kInput ? "r" : "w";
}

void RepairStream(StdioStream stream, StdioHandleCheck check) {
  if (check(stream)) {
    return;
  }
  std::FILE* file = StreamFile(stream);
  if (file == nullptr) {
    return;
  }
  std::freopen(NullDevicePath(), StreamMode(stream), file);
}

#if defined(_WIN32)
// The handler must only keep the process alive: the UCRT already decided
// the operation failed and set errno, so there is nothing to report that
// the reopened null device has not made harmless.
void DiscardInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                             uintptr_t) {}
#endif

}  // namespace

bool DefaultStdioHandleCheck(const StdioStream stream) {
#if defined(_WIN32)
  // A stream is usable when the process has its standard handle.
  static constexpr unsigned kHandleIds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  const HANDLE handle = ::GetStdHandle(kHandleIds[static_cast<int>(stream)]);
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;

#else
  // Other platforms always have their streams.
  (void)stream;
  return true;
#endif
}

void InstallStdioGuard(const StdioHandleCheck check) {
  RepairStream(StdioStream::kInput, check);
  RepairStream(StdioStream::kOutput, check);
  RepairStream(StdioStream::kError, check);
#if defined(_WIN32)
  ::_set_invalid_parameter_handler(DiscardInvalidParameter);
#endif
}

}  // namespace modlock::host_app
