#include "modlock/host_app/stdio_guard.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>

namespace {

using modlock::host_app::InstallStdioGuard;
using modlock::host_app::StdioHandleCheck;
using modlock::host_app::StdioStream;

// Records every stream the guard consults and answers from a per-stream
// table so a test can fail selected handles while keeping stdout usable for
// the test framework's own reporting.
struct RecordingCheck {
  std::set<StdioStream> probed;
  std::set<StdioStream> invalid_streams;

  static bool Entry(StdioStream stream) {
    auto& self = Instance();
    self.probed.insert(stream);
    return self.invalid_streams.count(stream) == 0;
  }

  static RecordingCheck& Instance() {
    static RecordingCheck check;
    return check;
  }
};

TEST(StdioGuard, ConsultsTheCheckForEveryStandardStream) {
  // The guard probes all three streams.
  auto& recorder = RecordingCheck::Instance();
  recorder.probed.clear();
  recorder.invalid_streams.clear();
  InstallStdioGuard(&RecordingCheck::Entry);
  EXPECT_EQ(recorder.probed, (std::set<StdioStream>{StdioStream::kInput, StdioStream::kOutput,
                                                    StdioStream::kError}));
}

TEST(StdioGuard, ValidHandlesLeaveTheStreamsUntouched) {
  // Valid streams keep their descriptors.
  auto& recorder = RecordingCheck::Instance();
  recorder.invalid_streams.clear();
  const int in_before = fileno(stdin);
  const int out_before = fileno(stdout);
  InstallStdioGuard(&RecordingCheck::Entry);
  EXPECT_EQ(fileno(stdin), in_before);
  EXPECT_EQ(fileno(stdout), out_before);
}

TEST(StdioGuard, InvalidHandlesReopenStreamsToAHarmlessDevice) {
  // Mark input and error invalid and install the guard.
  auto& recorder = RecordingCheck::Instance();
  recorder.probed.clear();
  recorder.invalid_streams = {StdioStream::kInput, StdioStream::kError};
  InstallStdioGuard(&RecordingCheck::Entry);

  // Each reopened stream stays usable: reads end the stream and writes are
  // discarded instead of raising the invalid-parameter condition.
  EXPECT_EQ(fgetc(stdin), EOF);
  EXPECT_GE(fileno(stdin), 0);
  EXPECT_EQ(fputc('x', stderr), 'x');
  EXPECT_GE(fileno(stderr), 0);
  EXPECT_EQ(recorder.probed, (std::set<StdioStream>{StdioStream::kInput, StdioStream::kOutput,
                                                    StdioStream::kError}));
}

}  // namespace
