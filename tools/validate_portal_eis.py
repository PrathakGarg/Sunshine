#!/usr/bin/env python3
"""
Validate xdg-desktop-portal RemoteDesktop EIS without ScreenCast capture.

Tests whether ConnectToEIS works after RemoteDesktop-only session setup
(CreateSession -> SelectDevices -> Start), which is required for pinch
injection while capture=kwin handles video.

Run on the Nobara desktop session (needs portal UI approval on first run):

  WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \\
    python3 tools/validate_portal_eis.py
"""

from __future__ import annotations

import os
import socket
import sys
import uuid
from typing import Any

import gi

gi.require_version("GLib", "2.0")
gi.require_version("Gio", "2.0")
from gi.repository import GLib, Gio

PORTAL_NAME = "org.freedesktop.portal.Desktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
REMOTE_DESKTOP_IFACE = "org.freedesktop.portal.RemoteDesktop"
SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast"
REQUEST_IFACE = "org.freedesktop.portal.Request"
SESSION_IFACE = "org.freedesktop.portal.Session"

TYPE_KEYBOARD = 1
TYPE_POINTER = 2
TYPE_TOUCHSCREEN = 4
PERSIST_UNTIL_REVOKED = 2
SOURCE_TYPE_MONITOR = 1
CURSOR_MODE_EMBEDDED = 2

REQUEST_PREFIX = "/org/freedesktop/portal/desktop/request/"
SESSION_PREFIX = "/org/freedesktop/portal/desktop/session/"


class PortalTestError(Exception):
    pass


def sender_token(conn: Gio.DBusConnection) -> str:
    name = conn.get_unique_name()[1:]
    return name.replace(".", "_")


def make_request_path(conn: Gio.DBusConnection, token: str) -> str:
    return f"{REQUEST_PREFIX}{sender_token(conn)}/{token}"


def make_session_path(conn: Gio.DBusConnection, token: str) -> str:
    return f"{SESSION_PREFIX}{sender_token(conn)}/{token}"


def wait_for_response(
    conn: Gio.DBusConnection, request_path: str, timeout_ms: int = 120_000
) -> tuple[int, dict[str, Any]]:
    loop = GLib.MainLoop()
    result: dict[str, Any] = {}

    def on_response(
        _connection: Gio.DBusConnection,
        _sender: str,
        _path: str,
        _iface: str,
        _signal: str,
        params: GLib.Variant,
    ) -> None:
        code, dict_variant = params.unpack()
        result["code"] = code
        result["results"] = dict(dict_variant) if dict_variant else {}
        loop.quit()

    sub_id = conn.signal_subscribe(
        PORTAL_NAME,
        REQUEST_IFACE,
        "Response",
        request_path,
        None,
        Gio.DBusSignalFlags.NONE,
        on_response,
    )

    timed_out = False

    def on_timeout() -> bool:
        nonlocal timed_out
        timed_out = True
        loop.quit()
        return False

    timeout_id = GLib.timeout_add(timeout_ms, on_timeout)

    loop.run()
    conn.signal_unsubscribe(sub_id)
    if not timed_out:
        GLib.source_remove(timeout_id)

    if "code" not in result:
        raise PortalTestError(f"Timed out waiting for portal response on {request_path}")

    return result["code"], result["results"]


def call_request(
    conn: Gio.DBusConnection,
    proxy: Gio.DBusProxy,
    method: str,
    parameters: GLib.Variant,
    timeout_ms: int = 120_000,
) -> tuple[int, dict[str, Any]]:
    token = f"eis_validate_{uuid.uuid4().hex[:8]}"
    request_path = make_request_path(conn, token)

    reply = proxy.call_sync(
        method,
        parameters,
        Gio.DBusCallFlags.NONE,
        timeout_ms,
        None,
    )
    returned_request_path = reply.unpack()[0]
    if returned_request_path != request_path:
        print(f"  note: portal returned request path {returned_request_path}")

    return wait_for_response(conn, returned_request_path, timeout_ms)


def unwrap_handle(value: Any) -> str:
    if isinstance(value, GLib.Variant):
        value = value.unpack()
    if isinstance(value, tuple) and len(value) == 1:
        value = value[0]
    return str(value)


def create_proxy(conn: Gio.DBusConnection, iface: str) -> Gio.DBusProxy:
    return Gio.DBusProxy.new_sync(
        conn,
        Gio.DBusProxyFlags.NONE,
        None,
        PORTAL_NAME,
        PORTAL_PATH,
        iface,
        None,
    )


def create_remote_desktop_session(conn: Gio.DBusConnection, rd_proxy: Gio.DBusProxy) -> str:
    session_token = f"eis_session_{uuid.uuid4().hex[:8]}"
    request_token = f"eis_req_{uuid.uuid4().hex[:8]}"

    params = GLib.Variant(
        "(a{sv})",
        [
            {
                "handle_token": GLib.Variant("s", request_token),
                "session_handle_token": GLib.Variant("s", session_token),
            }
        ],
    )

    print("-> RemoteDesktop.CreateSession")
    code, results = call_request(conn, rd_proxy, "CreateSession", params)
    if code != 0:
        raise PortalTestError(f"CreateSession failed with response code {code}")

    session_handle = unwrap_handle(results.get("session_handle"))
    print(f"   session: {session_handle}")
    return session_handle


def select_devices(conn: Gio.DBusConnection, rd_proxy: Gio.DBusProxy, session_handle: str) -> None:
    request_token = f"eis_req_{uuid.uuid4().hex[:8]}"
    device_types = TYPE_KEYBOARD | TYPE_POINTER | TYPE_TOUCHSCREEN

    params = GLib.Variant(
        "(oa{sv})",
        [
            session_handle,
            {
                "handle_token": GLib.Variant("s", request_token),
                "types": GLib.Variant("u", device_types),
                "persist_mode": GLib.Variant("u", PERSIST_UNTIL_REVOKED),
            },
        ],
    )

    print("-> RemoteDesktop.SelectDevices (keyboard|pointer|touchscreen)")
    print("   (approve the input-device portal dialog on the desktop if prompted)")
    code, _results = call_request(conn, rd_proxy, "SelectDevices", params)
    if code != 0:
        raise PortalTestError(f"SelectDevices failed with response code {code}")
    print("   SelectDevices OK")


def select_screencast_sources(
    conn: Gio.DBusConnection, sc_proxy: Gio.DBusProxy, session_handle: str
) -> None:
    request_token = f"eis_req_{uuid.uuid4().hex[:8]}"

    params = GLib.Variant(
        "(oa{sv})",
        [
            session_handle,
            {
                "handle_token": GLib.Variant("s", request_token),
                "types": GLib.Variant("u", SOURCE_TYPE_MONITOR),
                "cursor_mode": GLib.Variant("u", CURSOR_MODE_EMBEDDED),
                "multiple": GLib.Variant("b", True),
            },
        ],
    )

    print("-> ScreenCast.SelectSources (monitor)")
    print("   (approve the screen-share portal dialog on the desktop if prompted)")
    code, _results = call_request(conn, sc_proxy, "SelectSources", params)
    if code != 0:
        raise PortalTestError(f"SelectSources failed with response code {code}")
    print("   SelectSources OK")


def start_session(
    conn: Gio.DBusConnection, proxy: Gio.DBusProxy, session_handle: str, label: str
) -> dict[str, Any]:
    request_token = f"eis_req_{uuid.uuid4().hex[:8]}"
    params = GLib.Variant(
        "(osa{sv})",
        [
            session_handle,
            "",
            {"handle_token": GLib.Variant("s", request_token)},
        ],
    )

    print(f"-> {label}.Start")
    code, results = call_request(conn, proxy, "Start", params)
    if code != 0:
        raise PortalTestError(f"{label} Start failed with response code {code}")

    streams = results.get("streams")
    if streams is None:
        print("   Start OK (no streams in response)")
    else:
        print(f"   Start OK (streams={streams})")

    restore_token = results.get("restore_token")
    if restore_token:
        print(f"   restore_token={restore_token}")

    return results


def connect_to_eis(conn: Gio.DBusConnection, rd_proxy: Gio.DBusProxy, session_handle: str) -> int:
    params = GLib.Variant("(oa{sv})", [session_handle, {}])

    print("-> RemoteDesktop.ConnectToEIS")
    reply, fd_list = rd_proxy.call_with_unix_fd_list_sync(
        "ConnectToEIS",
        params,
        Gio.DBusCallFlags.NONE,
        120_000,
        None,
    )

    fd_index, = reply.unpack()
    try:
        eis_fd = fd_list.get(fd_index)
    except GLib.Error as exc:
        raise PortalTestError(f"ConnectToEIS fd_list.get failed: {exc.message}") from exc
    if eis_fd < 0:
        raise PortalTestError("ConnectToEIS returned an invalid fd")
    print(f"   ConnectToEIS OK (fd={eis_fd})")
    return eis_fd


def probe_eis_fd(fd: int) -> None:
    """Best-effort check that the fd is a usable socket."""
    flags = fcntl_getfl(fd)
    print(f"   fd flags=0x{flags:x}")

    sock = socket.fromfd(fd, socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    try:
        peek = sock.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT)
        print(f"   fd peek: {len(peek)} byte(s) available (socket is live)")
    except BlockingIOError:
        print("   fd peek: no data yet (socket is live)")
    except OSError as exc:
        print(f"   fd peek: {exc}")


def fcntl_getfl(fd: int) -> int:
    import fcntl

    return fcntl.fcntl(fd, fcntl.F_GETFL)


def close_session(conn: Gio.DBusConnection, session_handle: str) -> None:
    try:
        conn.call_sync(
            PORTAL_NAME,
            session_handle,
            SESSION_IFACE,
            "Close",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            5_000,
            None,
        )
        print(f"-> Session.Close ({session_handle})")
    except GLib.Error as exc:
        print(f"   Session.Close failed: {exc.message}")


def run_test(name: str, include_screencast: bool) -> bool:
    print()
    print("=" * 72)
    print(name)
    print("=" * 72)

    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    rd_proxy = create_proxy(conn, REMOTE_DESKTOP_IFACE)
    sc_proxy = create_proxy(conn, SCREENCAST_IFACE) if include_screencast else None

    session_handle = None
    eis_fd = None

    try:
        session_handle = create_remote_desktop_session(conn, rd_proxy)
        select_devices(conn, rd_proxy, session_handle)

        if include_screencast:
            assert sc_proxy is not None
            select_screencast_sources(conn, sc_proxy, session_handle)
            start_session(conn, rd_proxy, session_handle, "RemoteDesktop")
        else:
            start_session(conn, rd_proxy, session_handle, "RemoteDesktop")

        eis_fd = connect_to_eis(conn, rd_proxy, session_handle)
        probe_eis_fd(eis_fd)
        print()
        print(f"PASS: {name}")
        return True
    except PortalTestError as exc:
        print()
        print(f"FAIL: {name}: {exc}")
        return False
    finally:
        if eis_fd is not None and eis_fd >= 0:
            os.close(eis_fd)
        if session_handle:
            close_session(conn, session_handle)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description="Validate portal RemoteDesktop EIS paths")
    parser.add_argument(
        "--test",
        choices=("a", "b", "all"),
        default="all",
        help="a=input-only RemoteDesktop, b=combined with ScreenCast, all=both",
    )
    args = parser.parse_args()

    required = ["DBUS_SESSION_BUS_ADDRESS"]
    missing = [key for key in required if not os.environ.get(key)]
    if missing:
        print("Missing environment variables:", ", ".join(missing), file=sys.stderr)
        print(
            "Run from the graphical session, e.g.\n"
            "  WAYLAND_DISPLAY=wayland-0 DISPLAY=:0 "
            "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus "
            "python3 tools/validate_portal_eis.py",
            file=sys.stderr,
        )
        return 2

    print("Portal EIS validation")
    print(f"  WAYLAND_DISPLAY={os.environ.get('WAYLAND_DISPLAY', '(unset)')}")
    print(f"  DISPLAY={os.environ.get('DISPLAY', '(unset)')}")
    print(f"  DBUS_SESSION_BUS_ADDRESS={os.environ['DBUS_SESSION_BUS_ADDRESS']}")
    print()
    print("Watch the Nobara desktop for portal permission dialogs.")

    try:
        subprocess = __import__("subprocess")
        subprocess.run(
            [
                "notify-send",
                "-u",
                "critical",
                "-t",
                "15000",
                "Portal EIS validation",
                "Approve the KDE portal dialog(s) on this machine now.",
            ],
            check=False,
        )
    except Exception:
        pass

    results: dict[str, bool] = {}

    if args.test in ("a", "all"):
        results["a"] = run_test(
            "Test A: RemoteDesktop-only (no ScreenCast.SelectSources)",
            include_screencast=False,
        )

    if args.test in ("b", "all"):
        results["b"] = run_test(
            "Test B: RemoteDesktop + ScreenCast.SelectSources (Sunshine combined path)",
            include_screencast=True,
        )

    print()
    print("=" * 72)
    print("Summary")
    print("=" * 72)
    if "a" in results:
        print(f"  Test A (input-only, no screencast): {'PASS' if results['a'] else 'FAIL'}")
    if "b" in results:
        print(f"  Test B (combined portal session):     {'PASS' if results['b'] else 'FAIL'}")
    print()
    if results.get("a"):
        print("Option 1 is viable: EIS works without tying to portal video capture.")
    elif results.get("b"):
        print(
            "Option 1 needs combined portal session at stream start (Test B path), "
            "but can still ignore portal PipeWire while kwin captures video."
        )
    elif results:
        print("Neither path worked — pinch via portal EIS may not be viable on this setup.")

    return 0 if any(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
