/**
 * @file tests/unit/platform/linux/test_pinch.cpp
 * @brief Tests for pinch gesture mapping helpers.
 */
#ifdef __linux__

// test includes
#include "../../../tests_common.h"

// local includes
#include "src/config.h"
#include "src/platform/common.h"
#include "src/platform/linux/input/pinch.h"
#include "src/platform/linux/input/pinch_mapping.h"
#include "src/platform/linux/input/portal_input.h"

namespace {

  constexpr float kBaseSpan = 0.10F;
  constexpr float kMinSpan = 0.04F;
  constexpr float kMaxSpan = 0.5F;

}  // namespace

TEST(PinchMappingTest, ConvertsSpanToScaleWithClamping) {
  EXPECT_FLOAT_EQ(platf::pinch_mapping::span_to_scale(0.10F, kBaseSpan, kMinSpan, kMaxSpan), 1.0F);
  EXPECT_FLOAT_EQ(platf::pinch_mapping::span_to_scale(0.20F, kBaseSpan, kMinSpan, kMaxSpan), 2.0F);
  EXPECT_FLOAT_EQ(platf::pinch_mapping::span_to_scale(0.01F, kBaseSpan, kMinSpan, kMaxSpan), 0.4F);
  EXPECT_FLOAT_EQ(platf::pinch_mapping::span_to_scale(1.0F, kBaseSpan, kMinSpan, kMaxSpan), 5.0F);
  EXPECT_FLOAT_EQ(platf::pinch_mapping::span_to_scale(0.10F, 0.0F, kMinSpan, kMaxSpan), 1.0F);
}

TEST(PinchMappingTest, ComputesCenterDeltaInPixels) {
  const platf::touch_port_t touch_port {
    .offset_x = 0,
    .offset_y = 0,
    .width = 1920,
    .height = 1080,
  };

  const auto [delta_x, delta_y] = platf::pinch_mapping::center_delta_pixels(
    0.25F,
    0.50F,
    0.75F,
    0.75F,
    &touch_port
  );
  EXPECT_DOUBLE_EQ(delta_x, 960.0);
  EXPECT_DOUBLE_EQ(delta_y, 270.0);
}

TEST(PinchMappingTest, ReturnsZeroCenterDeltaWithoutTouchPort) {
  const auto [delta_x, delta_y] = platf::pinch_mapping::center_delta_pixels(0.1F, 0.2F, 0.3F, 0.4F, nullptr);
  EXPECT_DOUBLE_EQ(delta_x, 0.0);
  EXPECT_DOUBLE_EQ(delta_y, 0.0);
}

TEST(PinchMappingTest, MapsFingerPositionsAroundCenter) {
  const platf::touch_port_t touch_port {
    .offset_x = 100,
    .offset_y = 50,
    .width = 1000,
    .height = 800,
  };

  const auto positions = platf::pinch_mapping::finger_positions(0.5F, 0.5F, 0.10F, &touch_port);
  EXPECT_NEAR(positions.first.first, 560.0, 0.001);
  EXPECT_NEAR(positions.first.second, 450.0, 0.001);
  EXPECT_NEAR(positions.second.first, 640.0, 0.001);
  EXPECT_NEAR(positions.second.second, 450.0, 0.001);
}

TEST(PinchAvailabilityTest, ReportsUnavailableWithoutPortalEisFd) {
  platf::pinch::clear_portal_eis_fd();
  EXPECT_FALSE(platf::pinch::available());
}

TEST(PinchCapabilityTest, AdvertisesWhenPortalPinchIsBuilt) {
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
  EXPECT_TRUE(platf::pinch::capable());
#else
  EXPECT_FALSE(platf::pinch::capable());
#endif
}

TEST(PortalInputTest, UsesParallelSessionOutsidePortalCapture) {
  const auto previous_capture = config::video.capture;

  config::video.capture = "kwin";
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
  EXPECT_TRUE(platf::portal_input::manages_pinch_eis());
#else
  EXPECT_FALSE(platf::portal_input::manages_pinch_eis());
#endif

  config::video.capture = "portal";
  EXPECT_FALSE(platf::portal_input::manages_pinch_eis());

  config::video.capture = previous_capture;
}

#endif  // __linux__
