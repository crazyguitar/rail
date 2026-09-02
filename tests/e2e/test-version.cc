#include "rail/version.h"

#include <gtest/gtest.h>

TEST(Version, IsNotEmpty) { EXPECT_STRNE(rail::version(), ""); }
