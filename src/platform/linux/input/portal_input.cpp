/**
 * @file src/platform/linux/input/portal_input.cpp
 * @brief RemoteDesktop-only portal session for libei pinch while another backend captures video.
 */

// standard includes
#include <atomic>
#include <cstdint>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// platform includes
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/input/pinch.h"
#include "src/platform/linux/input/portal_input.h"

using namespace std::literals;

#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)

namespace {

  constexpr uint32_t PERSIST_UNTIL_REVOKED = 2;

  constexpr uint32_t TYPE_KEYBOARD = 1;
  constexpr uint32_t TYPE_POINTER = 2;
  constexpr uint32_t TYPE_TOUCHSCREEN = 4;

  constexpr const char *PORTAL_NAME = "org.freedesktop.portal.Desktop";
  constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
  constexpr const char *REMOTE_DESKTOP_IFACE = "org.freedesktop.portal.RemoteDesktop";
  constexpr const char *REQUEST_IFACE = "org.freedesktop.portal.Request";

  constexpr const char REQUEST_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
  constexpr const char SESSION_PREFIX[] = "/org/freedesktop/portal/desktop/session/";

  /**
   * @brief Persistent portal restore token for remote input device permissions.
   */
  class restore_token_t {
  public:
    /**
     * @brief Return the persisted restore token.
     *
     * @return Stored restore token string.
     */
    static std::string get() {
      return *token_;
    }

    /**
     * @brief Store a new restore token value.
     *
     * @param value Portal restore token from RemoteDesktop.Start.
     */
    static void set(std::string_view value) {
      *token_ = value;
    }

    /**
     * @brief Return whether a restore token has been persisted.
     *
     * @return True when the restore token is non-empty.
     */
    static bool empty() {
      return token_->empty();
    }

    /**
     * @brief Load the restore token from disk.
     */
    static void load() {
      std::ifstream file(get_file_path());
      if (file.is_open()) {
        std::getline(file, *token_);
        if (!token_->empty()) {
          BOOST_LOG(info) << "[portal_input] Loaded portal input restore token from disk"sv;
        }
      }
    }

    /**
     * @brief Persist the restore token to disk.
     */
    static void save() {
      if (token_->empty()) {
        return;
      }

      std::ofstream file(get_file_path());
      if (file.is_open()) {
        file << *token_;
        BOOST_LOG(info) << "[portal_input] Saved portal input restore token to disk"sv;
      } else {
        BOOST_LOG(warning) << "[portal_input] Failed to save portal input restore token"sv;
      }
    }

  private:
    static inline const std::unique_ptr<std::string> token_ = std::make_unique<std::string>();

    static std::string get_file_path() {
      return platf::appdata().string() + "/portal_input_token";
    }
  };

  /**
   * @brief DBus response loop state for portal request objects.
   */
  struct dbus_response_t {
    GMainLoop *loop;  ///< GLib main loop waiting for a portal response signal.
    GVariant *response;  ///< DBus response payload returned by the portal.
    guint subscription_id;  ///< Subscription ID.
  };

  /**
   * @brief RemoteDesktop portal session used only for ConnectToEIS pinch injection.
   */
  class input_session_t {
  public:
    input_session_t() = default;

    input_session_t(const input_session_t &) = delete;
    input_session_t &operator=(const input_session_t &) = delete;

    ~input_session_t() noexcept {
      try {
        platf::pinch::clear_portal_eis_fd();

        if (conn && !session_handle.empty()) {
          g_autoptr(GError) err = nullptr;
          g_dbus_connection_call_sync(
            conn,
            PORTAL_NAME,
            session_handle.c_str(),
            "org.freedesktop.portal.Session",
            "Close",
            nullptr,
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            &err
          );

          if (err) {
            BOOST_LOG(warning) << "[portal_input] Failed to close portal session: "sv << err->message;
          } else {
            BOOST_LOG(debug) << "[portal_input] Closed portal input session: "sv << session_handle;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "[portal_input] Exception in session teardown: "sv << e.what();
      } catch (...) {
        BOOST_LOG(error) << "[portal_input] Unknown exception in session teardown"sv;
      }

      if (remote_desktop_proxy) {
        g_clear_object(&remote_desktop_proxy);
      }
      if (conn) {
        g_clear_object(&conn);
      }
    }

    /**
     * @brief Create a RemoteDesktop session and connect libei over EIS.
     *
     * @return True when the EIS fd was handed to the pinch backend.
     */
    bool connect() {
      restore_token_t::load();

      conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      if (!conn) {
        BOOST_LOG(error) << "[portal_input] Failed to connect to session bus"sv;
        return false;
      }

      remote_desktop_proxy = g_dbus_proxy_new_sync(
        conn,
        G_DBUS_PROXY_FLAGS_NONE,
        nullptr,
        PORTAL_NAME,
        PORTAL_PATH,
        REMOTE_DESKTOP_IFACE,
        nullptr,
        nullptr
      );
      if (!remote_desktop_proxy) {
        BOOST_LOG(error) << "[portal_input] Failed to create RemoteDesktop proxy"sv;
        return false;
      }

      g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
      g_autofree gchar *session_path = nullptr;
      g_autofree gchar *session_token = nullptr;
      create_session_path(conn, nullptr, &session_token);

      if (create_portal_session(loop, &session_path, session_token) < 0) {
        return false;
      }

      if (select_remote_desktop_devices(loop, session_path) < 0) {
        return false;
      }

      if (start_remote_desktop_session(loop, session_path) < 0) {
        return false;
      }

      int eis_fd = -1;
      if (connect_to_eis(session_path, eis_fd) < 0) {
        return false;
      }

      platf::pinch::set_portal_eis_fd(eis_fd);
      if (eis_fd >= 0) {
        close(eis_fd);
      }

      // The libei handshake completes asynchronously once the compositor emits
      // EI events. Keep the portal session alive even if that has not happened yet.
      if (platf::pinch::available()) {
        BOOST_LOG(info) << "[portal_input] libei gesture device ready"sv;
      } else {
        BOOST_LOG(info) << "[portal_input] EIS connected; libei handshake will complete on first pinch event"sv;
      }

      return true;
    }

  private:
    GDBusConnection *conn = nullptr;
    GDBusProxy *remote_desktop_proxy = nullptr;
    std::string session_handle;

    static void on_response_received_cb(
      [[maybe_unused]] GDBusConnection *connection,
      [[maybe_unused]] const gchar *sender_name,
      [[maybe_unused]] const gchar *object_path,
      [[maybe_unused]] const gchar *interface_name,
      [[maybe_unused]] const gchar *signal_name,
      GVariant *parameters,
      gpointer user_data
    ) {
      auto *response = static_cast<dbus_response_t *>(user_data);
      response->response = g_variant_ref_sink(parameters);
      g_main_loop_quit(response->loop);
    }

    static gchar *get_sender_string(GDBusConnection *connection) {
      gchar *sender = g_strdup(g_dbus_connection_get_unique_name(connection) + 1);
      gchar *dot;
      while ((dot = strstr(sender, ".")) != nullptr) {
        *dot = '_';
      }
      return sender;
    }

    static void create_request_path(GDBusConnection *connection, gchar **out_path, gchar **out_token) {
      static uint32_t request_count = 0;

      request_count++;

      if (out_token) {
        *out_token = g_strdup_printf("SunshineInput%u", request_count);
      }
      if (out_path) {
        g_autofree gchar *sender = get_sender_string(connection);
        *out_path = g_strdup(std::format("{}{}{}{}", REQUEST_PREFIX, sender, "/SunshineInput", request_count).c_str());
      }
    }

    static void create_session_path(GDBusConnection *connection, gchar **out_path, gchar **out_token) {
      static uint32_t session_count = 0;

      session_count++;

      if (out_token) {
        *out_token = g_strdup_printf("SunshineInput%u", session_count);
      }

      if (out_path) {
        g_autofree gchar *sender = get_sender_string(connection);
        *out_path = g_strdup(std::format("{}{}{}{}", SESSION_PREFIX, sender, "/SunshineInput", session_count).c_str());
      }
    }

    static void dbus_response_init(dbus_response_t *response, GMainLoop *loop, GDBusConnection *connection, const char *request_path) {
      response->loop = loop;
      response->subscription_id = g_dbus_connection_signal_subscribe(
        connection,
        PORTAL_NAME,
        REQUEST_IFACE,
        "Response",
        request_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_response_received_cb,
        response,
        nullptr
      );
    }

    static GVariant *dbus_response_wait(dbus_response_t *response) {
      g_main_loop_run(response->loop);
      return response->response;
    }

    int create_portal_session(GMainLoop *loop, gchar **session_path_out, const gchar *session_token) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(a{sv})"));
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(
        remote_desktop_proxy,
        "CreateSession",
        g_variant_builder_end(&builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
      );

      if (err) {
        BOOST_LOG(error) << "[portal_input] Could not create RemoteDesktop session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) create_response = dbus_response_wait(&response);
      if (!create_response) {
        BOOST_LOG(error) << "[portal_input] CreateSession: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) results = nullptr;
      g_variant_get(create_response, "(u@a{sv})", &response_code, &results);
      if (response_code != 0) {
        BOOST_LOG(error) << "[portal_input] CreateSession failed with response code: "sv << response_code;
        return -1;
      }

      g_autoptr(GVariant) session_handle_v = g_variant_lookup_value(results, "session_handle", nullptr);
      if (!session_handle_v) {
        BOOST_LOG(error) << "[portal_input] CreateSession: session_handle missing"sv;
        return -1;
      }

      if (g_variant_is_of_type(session_handle_v, G_VARIANT_TYPE_VARIANT)) {
        g_autoptr(GVariant) inner = g_variant_get_variant(session_handle_v);
        *session_path_out = g_strdup(g_variant_get_string(inner, nullptr));
      } else {
        *session_path_out = g_strdup(g_variant_get_string(session_handle_v, nullptr));
      }

      session_handle = *session_path_out;
      BOOST_LOG(info) << "[portal_input] Created RemoteDesktop session: "sv << session_handle;
      return 0;
    }

    int select_remote_desktop_devices(GMainLoop *loop, const gchar *session_path) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(oa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(TYPE_KEYBOARD | TYPE_POINTER | TYPE_TOUCHSCREEN));
      g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(PERSIST_UNTIL_REVOKED));
      if (!restore_token_t::empty()) {
        g_variant_builder_add(&builder, "{sv}", "restore_token", g_variant_new_string(restore_token_t::get().c_str()));
      }
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(
        remote_desktop_proxy,
        "SelectDevices",
        g_variant_builder_end(&builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
      );

      if (err) {
        BOOST_LOG(error) << "[portal_input] Could not select remote desktop devices: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) devices_response = dbus_response_wait(&response);
      if (!devices_response) {
        BOOST_LOG(error) << "[portal_input] SelectDevices: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_variant_get(devices_response, "(u@a{sv})", &response_code, nullptr);
      if (response_code != 0) {
        BOOST_LOG(error) << "[portal_input] SelectDevices failed with response code: "sv << response_code;
        return -1;
      }

      BOOST_LOG(info) << "[portal_input] Remote desktop input devices selected"sv;
      return 0;
    }

    int start_remote_desktop_session(GMainLoop *loop, const gchar *session_path) {
      dbus_response_t response = {
        nullptr,
      };
      g_autofree gchar *request_token = nullptr;
      create_request_path(conn, nullptr, &request_token);

      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("(osa{sv})"));
      g_variant_builder_add(&builder, "o", session_path);
      g_variant_builder_add(&builder, "s", "");
      g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
      g_variant_builder_close(&builder);

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_sync(
        remote_desktop_proxy,
        "Start",
        g_variant_builder_end(&builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &err
      );
      if (err) {
        BOOST_LOG(error) << "[portal_input] Could not start RemoteDesktop session: "sv << err->message;
        return -1;
      }

      const gchar *request_path = nullptr;
      g_variant_get(reply, "(o)", &request_path);
      dbus_response_init(&response, loop, conn, request_path);

      g_autoptr(GVariant) start_response = dbus_response_wait(&response);
      if (!start_response) {
        BOOST_LOG(error) << "[portal_input] RemoteDesktop Start: no response received"sv;
        return -1;
      }

      guint32 response_code;
      g_autoptr(GVariant) dict = nullptr;
      g_variant_get(start_response, "(u@a{sv})", &response_code, &dict);
      if (response_code != 0) {
        BOOST_LOG(error) << "[portal_input] RemoteDesktop Start failed with response code: "sv << response_code;
        return -1;
      }

      if (const gchar *new_token = nullptr; g_variant_lookup(dict, "restore_token", "s", &new_token) && new_token && new_token[0] != '\0' && restore_token_t::get() != new_token) {
        restore_token_t::set(new_token);
        restore_token_t::save();
      }

      BOOST_LOG(info) << "[portal_input] RemoteDesktop session started for pinch injection"sv;
      return 0;
    }

    int connect_to_eis(const gchar *session_path, int &fd) {
      g_autoptr(GUnixFDList) fd_list = nullptr;
      g_autoptr(GVariant) msg = g_variant_ref_sink(g_variant_new("(oa{sv})", session_path, nullptr));

      g_autoptr(GError) err = nullptr;
      g_autoptr(GVariant) reply = g_dbus_proxy_call_with_unix_fd_list_sync(
        remote_desktop_proxy,
        "ConnectToEIS",
        msg,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        nullptr,
        &fd_list,
        nullptr,
        &err
      );
      if (err) {
        BOOST_LOG(error) << "[portal_input] Could not connect to EIS: "sv << err->message;
        return -1;
      }

      int fd_handle = 0;
      g_variant_get(reply, "(h)", &fd_handle);
      fd = g_unix_fd_list_get(fd_list, fd_handle, nullptr);
      if (fd < 0) {
        BOOST_LOG(error) << "[portal_input] ConnectToEIS returned an invalid fd"sv;
        return -1;
      }

      BOOST_LOG(info) << "[portal_input] Connected to portal EIS for pinch injection"sv;
      return 0;
    }
  };

  std::mutex session_mutex;
  std::unique_ptr<input_session_t> active_session;
  std::atomic<std::uint64_t> connect_generation {0};

}  // namespace

#endif  // HAVE_LIBEI_PINCH && SUNSHINE_BUILD_PORTAL

namespace platf::portal_input {

  bool manages_pinch_eis() {
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
    return config::video.capture != "portal";
#else
    return false;
#endif
  }

  void streaming_start() {
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
    if (!manages_pinch_eis()) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock {session_mutex};
      if (active_session) {
        return;
      }
    }

    const auto generation = ++connect_generation;
    BOOST_LOG(info) << "[portal_input] Opening RemoteDesktop session for pinch injection (async)"sv;

    std::thread([generation]() {
      auto session = std::make_unique<input_session_t>();
      if (!session->connect()) {
        BOOST_LOG(warning) << "[portal_input] Pinch injection unavailable; RemoteDesktop EIS setup failed"sv;
        return;
      }

      std::lock_guard<std::mutex> lock {session_mutex};
      if (generation != connect_generation) {
        BOOST_LOG(debug) << "[portal_input] Discarding stale portal input session"sv;
        return;
      }

      active_session = std::move(session);
      BOOST_LOG(info) << "[portal_input] Portal input session ready for pinch injection"sv;
    }).detach();
#endif
  }

  void streaming_stop() {
#if defined(HAVE_LIBEI_PINCH) && defined(SUNSHINE_BUILD_PORTAL)
    ++connect_generation;

    std::lock_guard<std::mutex> lock {session_mutex};
    if (!active_session) {
      return;
    }

    BOOST_LOG(info) << "[portal_input] Closing RemoteDesktop session for pinch injection"sv;
    active_session.reset();
#endif
  }

}  // namespace platf::portal_input
