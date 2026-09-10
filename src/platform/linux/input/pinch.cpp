/**
 * @file src/platform/linux/input/pinch.cpp
 * @brief Linux pinch gesture injection via libei.
 */

// standard includes
#include <array>
#include <mutex>
#include <utility>

// platform includes
#include <moonlight-common-c/src/Limelight.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/input/pinch.h"
#include "src/platform/linux/input/pinch_mapping.h"

using namespace std::literals;

namespace {

  constexpr float kBaseSpan = 0.10F;
  constexpr float kMinSpan = 0.04F;
  constexpr float kMaxSpan = 1.0F;
  constexpr std::uint32_t kPinchFingers = 2;

#if defined(HAVE_LIBEI_PINCH)
  #include <libei.h>
  #include <unistd.h>

  /**
   * @brief Active libei injection strategy for pinch gestures.
   */
  enum class injection_mode_t {
    None,  ///< No device selected yet.
    Gesture,  ///< Native libei pinch events.
    Touch,  ///< Two-finger touch fallback for compositors without gesture EIS.
  };

  /**
   * @brief State for one simulated touch contact.
   */
  struct touch_contact_t {
    struct ei_touch *touch = nullptr;  ///< Active libei touch object.
  };

  struct libei_state_t {
    std::mutex mutex;
    int portal_fd = -1;
    struct ei *context = nullptr;
    struct ei_seat *seat = nullptr;
    struct ei_device *device = nullptr;
    injection_mode_t mode = injection_mode_t::None;
    bool handshake_complete = false;
    bool pinch_active = false;
    bool touch_active = false;
    float previous_center_x = 0.5F;
    float previous_center_y = 0.5F;
    std::uint32_t emulate_sequence = 1;
    std::array<touch_contact_t, 2> touch_contacts {};
  };

  libei_state_t &libei_state() {
    static libei_state_t state;
    return state;
  }

  /**
   * @brief Return true when the device advertises native pinch injection.
   *
   * @param device Candidate libei device.
   * @return True when the device supports gesture pinch events.
   */
  bool device_supports_gestures(struct ei_device *device) {
    return ei_device_has_capability(device, EI_DEVICE_CAP_GESTURES);
  }

  /**
   * @brief Return true when the device can be used for touch-based pinch simulation.
   *
   * @param device Candidate libei device.
   * @return True when the device supports absolute touch injection.
   */
  bool device_supports_touch(struct ei_device *device) {
    return ei_device_has_capability(device, EI_DEVICE_CAP_TOUCH) ||
           ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE);
  }

  /**
   * @brief Process pending libei events until the queue is empty.
   *
   * @param state libei connection state.
   */
  void dispatch_events(libei_state_t &state) {
    if (!state.context) {
      return;
    }

    ei_dispatch(state.context);

    struct ei_event *event = nullptr;
    while ((event = ei_get_event(state.context)) != nullptr) {
      switch (ei_event_get_type(event)) {
        case EI_EVENT_CONNECT:
          BOOST_LOG(debug) << "[pinch] libei connected"sv;
          break;
        case EI_EVENT_SEAT_ADDED:
          if (!state.seat) {
            state.seat = ei_seat_ref(ei_event_get_seat(event));
            ei_seat_bind_capabilities(
              state.seat,
              EI_DEVICE_CAP_GESTURES,
              EI_DEVICE_CAP_TOUCH,
              EI_DEVICE_CAP_POINTER_ABSOLUTE,
              nullptr
            );
            ei_seat_request_device_with_capabilities(state.seat, EI_DEVICE_CAP_GESTURES, nullptr);
            BOOST_LOG(debug) << "[pinch] libei seat bound for gestures and touch"sv;
          }
          break;
        case EI_EVENT_SEAT_REMOVED:
          if (ei_event_get_seat(event) == state.seat) {
            state.seat = ei_seat_unref(state.seat);
          }
          break;
        case EI_EVENT_DEVICE_ADDED:
          {
            auto *device = ei_event_get_device(event);
            if (state.device) {
              break;
            }

            if (device_supports_gestures(device)) {
              state.device = ei_device_ref(device);
              state.mode = injection_mode_t::Gesture;
              BOOST_LOG(info) << "[pinch] libei gesture device added"sv;
            } else if (device_supports_touch(device)) {
              state.device = ei_device_ref(device);
              state.mode = injection_mode_t::Touch;
              BOOST_LOG(info) << "[pinch] libei touch device added; using two-finger fallback"sv;
            }
          }
          break;
        case EI_EVENT_DEVICE_RESUMED:
          if (auto *device = ei_event_get_device(event); device == state.device) {
            ei_device_start_emulating(device, state.emulate_sequence++);
            state.handshake_complete = true;
            if (state.mode == injection_mode_t::Gesture) {
              BOOST_LOG(info) << "[pinch] libei gesture device ready"sv;
            } else if (state.mode == injection_mode_t::Touch) {
              BOOST_LOG(info) << "[pinch] libei touch device ready for pinch simulation"sv;
            }
          }
          break;
        case EI_EVENT_DEVICE_REMOVED:
          if (auto *device = ei_event_get_device(event); device == state.device) {
            state.device = ei_device_unref(state.device);
            state.mode = injection_mode_t::None;
            state.handshake_complete = false;
            state.pinch_active = false;
            state.touch_active = false;
            BOOST_LOG(warning) << "[pinch] libei device removed"sv;
          }
          break;
        case EI_EVENT_DISCONNECT:
          state.device = ei_device_unref(state.device);
          state.seat = ei_seat_unref(state.seat);
          state.mode = injection_mode_t::None;
          state.handshake_complete = false;
          state.pinch_active = false;
          state.touch_active = false;
          BOOST_LOG(warning) << "[pinch] libei disconnected"sv;
          break;
        default:
          break;
      }

      ei_event_unref(event);
    }
  }

  /**
   * @brief Pump libei events until the device handshake completes or the queue stalls.
   *
   * @param state libei connection state.
   * @param max_rounds Maximum number of dispatch rounds.
   * @return True when a device is ready for injection.
   */
  bool pump_until_ready(libei_state_t &state, int max_rounds = 64) {
    for (int round = 0; round < max_rounds && !state.handshake_complete; ++round) {
      dispatch_events(state);
    }

    return state.handshake_complete;
  }

  /**
   * @brief Release any active simulated touch contacts.
   *
   * @param state libei connection state.
   */
  void release_touch_contacts(libei_state_t &state) {
    if (!state.device || state.mode != injection_mode_t::Touch) {
      return;
    }

    if (state.touch_active) {
      if (state.touch_contacts[0].touch) {
        ei_touch_up(state.touch_contacts[0].touch);
      }
      if (state.touch_contacts[1].touch) {
        ei_touch_up(state.touch_contacts[1].touch);
      }
      ei_device_frame(state.device, ei_now(state.context));
      state.touch_active = false;
    }

    for (auto &contact : state.touch_contacts) {
      if (contact.touch) {
        contact.touch = ei_touch_unref(contact.touch);
      }
    }
  }

  /**
   * @brief Tear down the active libei sender context.
   *
   * @param state libei connection state.
   */
  void teardown(libei_state_t &state) {
    if (state.device && state.mode == injection_mode_t::Gesture && state.pinch_active) {
      ei_device_pinch_end(state.device);
      state.pinch_active = false;
    }

    release_touch_contacts(state);

    if (state.device) {
      state.device = ei_device_unref(state.device);
    }

    if (state.seat) {
      state.seat = ei_seat_unref(state.seat);
    }

    if (state.context) {
      ei_unref(state.context);
      state.context = nullptr;
    }

    if (state.portal_fd >= 0) {
      close(state.portal_fd);
      state.portal_fd = -1;
    }

    state.device = nullptr;
    state.seat = nullptr;
    state.mode = injection_mode_t::None;
    state.handshake_complete = false;
  }

  /**
   * @brief Initialize the libei sender from the stored portal fd.
   *
   * @param state libei connection state.
   * @return True when initialization succeeds.
   */
  bool ensure_context(libei_state_t &state) {
    if (state.context) {
      return pump_until_ready(state);
    }

    if (state.portal_fd < 0) {
      return false;
    }

    state.context = ei_new_sender(nullptr);
    if (!state.context) {
      BOOST_LOG(error) << "[pinch] Failed to create libei sender context"sv;
      return false;
    }

    if (ei_setup_backend_fd(state.context, state.portal_fd) < 0) {
      BOOST_LOG(error) << "[pinch] Failed to set up libei backend fd"sv;
      teardown(state);
      return false;
    }

    return pump_until_ready(state);
  }

  /**
   * @brief Inject a pinch gesture through native libei gesture events.
   *
   * @param state libei connection state.
   * @param touch_port Monitor-local viewport used for center deltas.
   * @param pinch Pinch packet mapped to libei parameters.
   */
  void update_gesture_pinch(libei_state_t &state, const platf::touch_port_t *touch_port, const platf::pinch_input_t &pinch) {
    const auto scale = platf::pinch_mapping::span_to_scale(pinch.span, kBaseSpan, kMinSpan, kMaxSpan);

    switch (pinch.eventType) {
      case LI_PINCH_EVENT_BEGIN:
        if (state.pinch_active) {
          ei_device_pinch_end(state.device);
        }
        ei_device_pinch_begin(state.device, kPinchFingers);
        state.pinch_active = true;
        state.previous_center_x = pinch.centerX;
        state.previous_center_y = pinch.centerY;
        ei_device_pinch_update(state.device, 0.0, 0.0, scale, 0.0);
        break;
      case LI_PINCH_EVENT_UPDATE:
        if (!state.pinch_active) {
          return;
        }
        {
          const auto [delta_x, delta_y] = platf::pinch_mapping::center_delta_pixels(
            state.previous_center_x,
            state.previous_center_y,
            pinch.centerX,
            pinch.centerY,
            touch_port
          );
          state.previous_center_x = pinch.centerX;
          state.previous_center_y = pinch.centerY;
          ei_device_pinch_update(state.device, delta_x, delta_y, scale, 0.0);
        }
        break;
      case LI_PINCH_EVENT_END:
        if (!state.pinch_active) {
          return;
        }
        ei_device_pinch_end(state.device);
        state.pinch_active = false;
        break;
      default:
        break;
    }
  }

  /**
   * @brief Place two touch contacts for the touch-based pinch fallback.
   *
   * @param state libei connection state.
   * @param positions Desktop coordinates for both contacts.
   * @return True when both contacts were placed successfully.
   */
  bool begin_touch_contacts(libei_state_t &state, const platf::pinch_mapping::finger_positions_t &positions) {
    release_touch_contacts(state);
    state.touch_contacts[0].touch = ei_device_touch_new(state.device);
    state.touch_contacts[1].touch = ei_device_touch_new(state.device);
    if (!state.touch_contacts[0].touch || !state.touch_contacts[1].touch) {
      BOOST_LOG(warning) << "[pinch] Failed to allocate libei touch contacts"sv;
      release_touch_contacts(state);
      return false;
    }

    ei_touch_down(state.touch_contacts[0].touch, positions.first.first, positions.first.second);
    ei_touch_down(state.touch_contacts[1].touch, positions.second.first, positions.second.second);
    ei_device_frame(state.device, ei_now(state.context));
    state.touch_active = true;
    BOOST_LOG(debug)
      << "[pinch] touch fallback began at ("sv << positions.first.first << ',' << positions.first.second << ") and ("
      << positions.second.first << ',' << positions.second.second << ')' << std::endl;
    return true;
  }

  /**
   * @brief Inject a pinch gesture by simulating two touch contacts.
   *
   * @param state libei connection state.
   * @param touch_port Monitor-local viewport used for coordinate mapping.
   * @param pinch Pinch packet mapped to touch positions.
   */
  void update_touch_pinch(libei_state_t &state, const platf::touch_port_t *touch_port, const platf::pinch_input_t &pinch) {
    if (!touch_port) {
      BOOST_LOG(warning) << "[pinch] Touch fallback requires a touch port"sv;
      return;
    }

    const auto positions = platf::pinch_mapping::finger_positions(pinch.centerX, pinch.centerY, pinch.span, touch_port);

    switch (pinch.eventType) {
      case LI_PINCH_EVENT_BEGIN:
        begin_touch_contacts(state, positions);
        break;
      case LI_PINCH_EVENT_UPDATE:
        if (!state.touch_active || !state.touch_contacts[0].touch || !state.touch_contacts[1].touch) {
          // The libei device may become ready after the first BEGIN packet was dropped.
          if (!begin_touch_contacts(state, positions)) {
            return;
          }
          BOOST_LOG(info) << "[pinch] touch fallback recovered after late libei handshake"sv;
          break;
        }

        ei_touch_motion(state.touch_contacts[0].touch, positions.first.first, positions.first.second);
        ei_touch_motion(state.touch_contacts[1].touch, positions.second.first, positions.second.second);
        ei_device_frame(state.device, ei_now(state.context));
        break;
      case LI_PINCH_EVENT_END:
        if (!state.touch_active) {
          return;
        }
        release_touch_contacts(state);
        break;
      default:
        break;
    }
  }

#endif  // HAVE_LIBEI_PINCH

}  // namespace

namespace platf::pinch {

  bool capable() {
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
    return true;
#else
    return false;
#endif
  }

  bool available() {
#if defined(HAVE_LIBEI_PINCH)
    auto &state = libei_state();
    std::lock_guard<std::mutex> lock {state.mutex};
    return ensure_context(state);
#else
    return false;
#endif
  }

  void set_portal_eis_fd(int fd) {
#if defined(HAVE_LIBEI_PINCH)
    if (fd < 0) {
      return;
    }

    auto &state = libei_state();
    std::lock_guard<std::mutex> lock {state.mutex};
    teardown(state);

    state.portal_fd = dup(fd);
    if (state.portal_fd < 0) {
      BOOST_LOG(error) << "[pinch] Failed to duplicate portal EIS fd"sv;
      return;
    }

    ensure_context(state);
#else
    (void) fd;
#endif
  }

  void clear_portal_eis_fd() {
#if defined(HAVE_LIBEI_PINCH)
    auto &state = libei_state();
    std::lock_guard<std::mutex> lock {state.mutex};
    teardown(state);
#endif
  }

}  // namespace platf::pinch

namespace platf {

  void pinch_update(client_input_t *input, const touch_port_t *touch_port, const pinch_input_t &pinch) {
    (void) input;

#if defined(HAVE_LIBEI_PINCH)
    auto &state = libei_state();
    std::lock_guard<std::mutex> lock {state.mutex};

    if (!ensure_context(state) || !state.device) {
      BOOST_LOG(warning) << "[pinch] Dropping pinch packet; libei not ready (handshake incomplete or no device)"sv;
      return;
    }

    if (state.mode == injection_mode_t::Gesture) {
      update_gesture_pinch(state, touch_port, pinch);
    } else if (state.mode == injection_mode_t::Touch) {
      update_touch_pinch(state, touch_port, pinch);
    } else {
      BOOST_LOG(warning) << "[pinch] Dropping pinch packet; no supported libei injection mode"sv;
      return;
    }

    dispatch_events(state);
#else
    (void) touch_port;
    (void) pinch;
#endif
  }

}  // namespace platf
