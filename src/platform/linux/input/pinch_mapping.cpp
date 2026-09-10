/**
 * @file src/platform/linux/input/pinch_mapping.cpp
 * @brief Helpers for mapping pinch protocol values to libei parameters.
 */

// standard includes
#include <algorithm>
#include <cmath>

// local includes
#include "src/platform/linux/input/pinch_mapping.h"

namespace platf::pinch_mapping {

  float span_to_scale(float span, float base_span, float min_span, float max_span) {
    const auto clamped_span = std::clamp(span, min_span, max_span);
    if (base_span <= 0.0F) {
      return 1.0F;
    }

    return clamped_span / base_span;
  }

  std::pair<double, double> center_delta_pixels(
    float previous_x,
    float previous_y,
    float next_x,
    float next_y,
    const touch_port_t *touch_port
  ) {
    if (!touch_port || touch_port->width <= 0 || touch_port->height <= 0) {
      return {0.0, 0.0};
    }

    const auto width = static_cast<double>(touch_port->width);
    const auto height = static_cast<double>(touch_port->height);
    const auto delta_x = (static_cast<double>(next_x) - static_cast<double>(previous_x)) * width;
    const auto delta_y = (static_cast<double>(next_y) - static_cast<double>(previous_y)) * height;
    return {delta_x, delta_y};
  }

  finger_positions_t finger_positions(
    float center_x,
    float center_y,
    float span,
    const touch_port_t *touch_port
  ) {
    if (!touch_port || touch_port->width <= 0 || touch_port->height <= 0) {
      return {};
    }

    const auto center_desktop_x = static_cast<double>(touch_port->offset_x) + static_cast<double>(center_x) * touch_port->width;
    const auto center_desktop_y = static_cast<double>(touch_port->offset_y) + static_cast<double>(center_y) * touch_port->height;
    const auto half_separation = static_cast<double>(span) * std::min(touch_port->width, touch_port->height) / 2.0;

    return {
      {center_desktop_x - half_separation, center_desktop_y},
      {center_desktop_x + half_separation, center_desktop_y},
    };
  }

}  // namespace platf::pinch_mapping
