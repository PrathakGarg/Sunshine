/**
 * @file tests/unit/platform/linux/test_pinch.cpp
 * @brief Tests for Linux pinch gesture forwarding.
 */

// test includes
#include "../../tests_common.h"

// local includes
#include "src/platform/common.h"

TEST(PinchInputTest, PinchUpdateAcceptsAllPhases) {
  platf::pinch_input_t begin {LI_PINCH_EVENT_BEGIN, 0.10F, 0.5F, 0.5F};
  platf::pinch_input_t update {LI_PINCH_EVENT_UPDATE, 0.14F, 0.5F, 0.5F};
  platf::pinch_input_t end {LI_PINCH_EVENT_END, 0.14F, 0.5F, 0.5F};

  platf::pinch_update(nullptr, begin);
  platf::pinch_update(nullptr, update);
  platf::pinch_update(nullptr, end);
}
