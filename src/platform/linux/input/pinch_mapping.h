/**
 * @file src/platform/linux/input/pinch_mapping.h
 * @brief Helpers for mapping pinch protocol values to libei parameters.
 */
#pragma once

// standard includes
#include <utility>

// local includes
#include "src/platform/common.h"

namespace platf::pinch_mapping {

  /**
   * @brief Desktop coordinates for two simulated pinch contacts.
   */
  struct finger_positions_t {
    std::pair<double, double> first;  ///< First contact position in logical pixels.
    std::pair<double, double> second;  ///< Second contact position in logical pixels.
  };

  /**
   * @brief Convert a normalized span to a libei pinch scale value.
   *
   * @param span Normalized finger separation.
   * @param base_span Span that maps to scale 1.0.
   * @param min_span Minimum allowed span.
   * @param max_span Maximum allowed span.
   * @return Absolute pinch scale for libei.
   */
  float span_to_scale(float span, float base_span, float min_span, float max_span);

  /**
   * @brief Compute the pixel delta of a normalized gesture center.
   *
   * @param previous_x Previous normalized center X.
   * @param previous_y Previous normalized center Y.
   * @param next_x Next normalized center X.
   * @param next_y Next normalized center Y.
   * @param touch_port Optional viewport used for scaling.
   * @return Relative center movement in logical pixels.
   */
  std::pair<double, double> center_delta_pixels(
    float previous_x,
    float previous_y,
    float next_x,
    float next_y,
    const touch_port_t *touch_port
  );

  /**
   * @brief Map a normalized pinch center and span to two desktop touch points.
   *
   * @param center_x Normalized center X within the touch port.
   * @param center_y Normalized center Y within the touch port.
   * @param span Normalized finger separation.
   * @param touch_port Monitor-local viewport used for scaling.
   * @return Two desktop coordinates placed symmetrically around the center.
   */
  finger_positions_t finger_positions(
    float center_x,
    float center_y,
    float span,
    const touch_port_t *touch_port
  );

}  // namespace platf::pinch_mapping
