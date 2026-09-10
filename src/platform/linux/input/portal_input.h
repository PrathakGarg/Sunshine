/**
 * @file src/platform/linux/input/portal_input.h
 * @brief xdg-desktop-portal RemoteDesktop session used for libei pinch injection.
 */
#pragma once

namespace platf::portal_input {

  /**
   * @brief Return whether a parallel portal input session is used for the active capture backend.
   *
   * Portal video capture already negotiates EIS during screencast setup.
   *
   * @return True when pinch EIS should be acquired through this module.
   */
  bool manages_pinch_eis();

  /**
   * @brief Open a RemoteDesktop portal session and connect libei pinch injection.
   *
   * Called when the first streaming session starts. The portal handshake runs
   * asynchronously so RTSP PLAY can complete without waiting on portal dialogs.
   */
  void streaming_start();

  /**
   * @brief Close the active portal input session.
   *
   * Called when the last streaming session stops.
   */
  void streaming_stop();

}  // namespace platf::portal_input
