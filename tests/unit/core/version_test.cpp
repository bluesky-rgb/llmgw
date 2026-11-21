#include "llmgw/core/version.h"

#include <gtest/gtest.h>

#include <string>

namespace llmgw::core {
namespace {

TEST(Version, MatchesProjectVersionComponents) {
  const std::string expected = std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) +
                               "." + std::to_string(kVersionPatch);
  EXPECT_EQ(kVersion, expected);
}

}  // namespace
}  // namespace llmgw::core
