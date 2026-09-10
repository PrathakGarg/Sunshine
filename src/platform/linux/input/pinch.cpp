/**
 * @file src/platform/linux/input/pinch.cpp
 * @brief Linux pinch gesture injection.
 */

// local includes
#include "src/platform/common.h"

namespace platf {

  /**
   * @brief Forward pinch gestures to the OS.
   *
   * libei injection will be implemented here.
   *
   * @param input The client-specific input context.
   * @param pinch The pinch gesture event.
   */
  void pinch_update(client_input_t *input, const pinch_input_t &pinch) {
    (void) input;
    (void) pinch;
  }

}  // namespace platf
