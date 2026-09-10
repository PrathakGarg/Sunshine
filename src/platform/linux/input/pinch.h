/**
 * @file src/platform/linux/input/pinch.h
 * @brief Linux libei pinch gesture injection API.
 */
#pragma once

namespace platf::pinch {

  /**
   * @brief Return whether pinch should be advertised to clients.
   *
   * Advertisement happens during RTSP negotiation, before the portal EIS fd is
   * available, so this checks compile-time support and capture configuration.
   *
   * @return True when libei pinch injection is built with portal support.
   */
  bool capable();

  /**
   * @brief Return whether libei pinch injection is ready.
   *
   * @return True when an EIS connection and gesture device are available.
   */
  bool available();

  /**
   * @brief Provide the portal EIS file descriptor for pinch injection.
   *
   * @param fd EIS socket file descriptor from RemoteDesktop.ConnectToEIS.
   */
  void set_portal_eis_fd(int fd);

  /**
   * @brief Release the active portal EIS connection.
   */
  void clear_portal_eis_fd();

}  // namespace platf::pinch
