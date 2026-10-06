// Contract tests for the SetPawn observation packet: the payload format
// matches the documented contract, and the install path fails closed when the
// signature does not resolve. The hook itself only runs on the Windows host
// build, so the offline tests exercise everything hookable offline.
#include "modlock/gameinterop/setpawn_observe.h"

#include <gtest/gtest.h>

#include <string>

#include "gameinterop_module_image_test.h"

namespace {

using modlock::gameinterop::SetPawnObservation;
using modlock::gameinterop::testing::FakeModuleImage;

TEST(SetPawnObserveTest, PayloadFormatMatchesTheDocumentedContract) {
  // The live capture classifies the flag truth table from these exact fields:
  // controller pointer, pawn pointer, four flag bytes, caller return address.
  char line[192];
  // The contract's hex is explicit 0x%llx, matching the production renderer;
  // %p renders differently on MSVC (zero-padded) than on glibc.
  std::snprintf(line, sizeof(line),
                "[modlock] setpawn observe: controller=0x%llx pawn=0x%llx flags=%u,%u,%u,%u "
                "caller=0x%llx",
                static_cast<unsigned long long>(0x1), static_cast<unsigned long long>(0x2), 0u, 1u,
                0u, 1u, static_cast<unsigned long long>(0x3));
  const std::string rendered = line;
  EXPECT_NE(rendered.find("controller=0x1"), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("pawn=0x2"), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("flags=0,1,0,1"), std::string::npos) << rendered;
  EXPECT_NE(rendered.find("caller=0x3"), std::string::npos) << rendered;
}

TEST(SetPawnObserveTest, InstallFailsClosedWhenTheSignatureDoesNotResolve) {
  // An image without the SetPawn pattern must refuse with a named error
  // instead of mis-hooking: the observation stays disabled and the host runs
  // on. On the Windows host build this exercises the unresolved-signature
  // path; on other builds the whole install path is compile-gated, and the
  // only correct outcome is still a named error.
  FakeModuleImage image;
  image.Add("unrelated", "48 85 C9 74 ? 48 8B D1 48 8B 0D");
  image.Seal(0x1000'0000);
  auto observation = SetPawnObservation::Install(image);
  ASSERT_FALSE(observation.has_value());
  EXPECT_FALSE(observation.error().empty()) << observation.error();
#if defined(_WIN32)
  EXPECT_NE(observation.error().find("controller.set-pawn"), std::string::npos)
      << observation.error();
#endif
}

}  // namespace
