#!/usr/bin/env python3
"""UI for Versión A (see docs/v1-spec.md, 8.1).

Menu-driven terminal UI over wizard-engine's UI socket:
  - Commands: send RUN/STOP.
  - Telemetry: live view of position/velocity/current with an ASCII graph.
  - Device Info: request it on demand (engine -> gateway -> MCU simulator).
"""

import argparse
import curses
import socket
import threading
import time
from collections import deque

# Must match shared/message.h exactly.
MSG_POSITION_EVENT = 0x21
MSG_VELOCITY_EVENT = 0x22
MSG_CURRENT_EVENT = 0x23
MSG_DEVICE_INFO_REQUEST = 0x30
MSG_DEVICE_INFO_RESPONSE = 0x31
MSG_SET_RUN_STOP_COMMAND = 0x40
MSG_RUN_STOP_STATUS_EVENT = 0x41
MSG_GATEWAY_STATUS_EVENT = 0x50
MSG_MCU_STATUS_EVENT = 0x51

HEADER_SIZE = 3
HISTORY_LEN = 60  # samples kept per parameter for the ASCII graph


def encode_frame(msg_type: int, payload: bytes) -> bytes:
    return bytes([msg_type]) + len(payload).to_bytes(2, "little") + payload


def encode_set_run_stop(run: bool) -> bytes:
    return encode_frame(MSG_SET_RUN_STOP_COMMAND, bytes([1 if run else 0]))


def encode_device_info_request() -> bytes:
    return encode_frame(MSG_DEVICE_INFO_REQUEST, b"")


class MessageParser:
    """Mirrors shared/message.h's MessageParser: buffers partial reads."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        self._buf.extend(data)

    def try_parse(self):
        if len(self._buf) < HEADER_SIZE:
            return None
        msg_type = self._buf[0]
        length = int.from_bytes(self._buf[1:3], "little")
        if len(self._buf) < HEADER_SIZE + length:
            return None
        payload = bytes(self._buf[HEADER_SIZE:HEADER_SIZE + length])
        del self._buf[:HEADER_SIZE + length]
        return msg_type, payload


class SharedState:
    """Everything the receiver thread updates and the UI screens read."""

    def __init__(self):
        self.lock = threading.Lock()
        self.position = deque(maxlen=HISTORY_LEN)
        self.velocity = deque(maxlen=HISTORY_LEN)
        self.current = deque(maxlen=HISTORY_LEN)
        self.run_stop_status = None  # None = unknown, True = RUNNING, False = STOPPED
        self.device_info = None  # dict or None
        self.gateway_connected = None  # None = unknown, True/False
        self.mcu_responding = None  # None = unknown, True/False
        self.connected = True


def parse_i32_event(payload: bytes):
    if len(payload) != 12:
        return None
    ts = int.from_bytes(payload[0:8], "little")
    val = int.from_bytes(payload[8:12], "little", signed=True)
    return ts, val


def parse_current_event(payload: bytes):
    if len(payload) != 10:
        return None
    ts = int.from_bytes(payload[0:8], "little")
    val = int.from_bytes(payload[8:10], "little", signed=True)
    return ts, val


def parse_run_stop_status(payload: bytes):
    if len(payload) != 9:
        return None
    ts = int.from_bytes(payload[0:8], "little")
    running = payload[8] != 0
    return ts, running


def parse_device_info(payload: bytes):
    if len(payload) != 16:
        return None
    vendor = int.from_bytes(payload[0:4], "little")
    product = int.from_bytes(payload[4:8], "little")
    revision = int.from_bytes(payload[8:12], "little")
    serial = int.from_bytes(payload[12:16], "little")
    return {"vendor_id": vendor, "product_code": product, "revision": revision, "serial": serial}


def receiver_thread(sock: socket.socket, state: SharedState):
    parser = MessageParser()
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            break
        parser.feed(data)
        while True:
            msg = parser.try_parse()
            if msg is None:
                break
            msg_type, payload = msg
            with state.lock:
                if msg_type == MSG_POSITION_EVENT:
                    parsed = parse_i32_event(payload)
                    if parsed:
                        state.position.append(parsed[1])
                elif msg_type == MSG_VELOCITY_EVENT:
                    parsed = parse_i32_event(payload)
                    if parsed:
                        state.velocity.append(parsed[1])
                elif msg_type == MSG_CURRENT_EVENT:
                    parsed = parse_current_event(payload)
                    if parsed:
                        state.current.append(parsed[1])
                elif msg_type == MSG_RUN_STOP_STATUS_EVENT:
                    parsed = parse_run_stop_status(payload)
                    if parsed:
                        state.run_stop_status = parsed[1]
                elif msg_type == MSG_DEVICE_INFO_RESPONSE:
                    state.device_info = parse_device_info(payload)
                elif msg_type == MSG_GATEWAY_STATUS_EVENT:
                    if len(payload) == 1:
                        state.gateway_connected = payload[0] != 0
                elif msg_type == MSG_MCU_STATUS_EVENT:
                    if len(payload) == 1:
                        state.mcu_responding = payload[0] != 0
    with state.lock:
        state.connected = False


def ascii_sparkline(values, width: int) -> str:
    """Renders a list of numbers as a single-line ASCII bar graph."""
    blocks = " .:-=+*#%@"
    if not values:
        return "(no data yet)"
    vals = list(values)[-width:]
    lo, hi = min(vals), max(vals)
    if hi == lo:
        return blocks[-1] * len(vals)
    span = hi - lo
    return "".join(blocks[int((v - lo) / span * (len(blocks) - 1))] for v in vals)


def draw_commands_screen(stdscr, sock: socket.socket, state: SharedState):
    stdscr.nodelay(False)
    while True:
        stdscr.clear()
        with state.lock:
            status = state.run_stop_status
        status_str = "UNKNOWN" if status is None else ("RUNNING" if status else "STOPPED")
        stdscr.addstr(0, 0, "== Commands ==")
        stdscr.addstr(2, 0, f"Current MCU status: {status_str}")
        stdscr.addstr(4, 0, "[r] RUN   [s] STOP   [b] Back")
        stdscr.refresh()

        key = stdscr.getkey()
        if key == "r":
            sock.sendall(encode_set_run_stop(True))
        elif key == "s":
            sock.sendall(encode_set_run_stop(False))
        elif key == "b":
            return


def draw_telemetry_screen(stdscr, state: SharedState):
    stdscr.nodelay(True)
    while True:
        stdscr.clear()
        height, width = stdscr.getmaxyx()
        graph_width = max(10, width - 20)
        with state.lock:
            position = list(state.position)
            velocity = list(state.velocity)
            current = list(state.current)

        stdscr.addstr(0, 0, "== Telemetry (live) ==  [b] Back")
        stdscr.addstr(2, 0, f"Position: {position[-1] if position else '-':>8}  ")
        stdscr.addstr(3, 0, ascii_sparkline(position, graph_width))
        stdscr.addstr(5, 0, f"Velocity: {velocity[-1] if velocity else '-':>8}  ")
        stdscr.addstr(6, 0, ascii_sparkline(velocity, graph_width))
        stdscr.addstr(8, 0, f"Current:  {current[-1] if current else '-':>8}  ")
        stdscr.addstr(9, 0, ascii_sparkline(current, graph_width))
        stdscr.refresh()

        try:
            key = stdscr.getkey()
            if key == "b":
                return
        except curses.error:
            pass  # no key pressed, nodelay mode
        time.sleep(0.1)


def draw_device_info_screen(stdscr, sock: socket.socket, state: SharedState):
    stdscr.nodelay(False)
    while True:
        stdscr.clear()
        with state.lock:
            info = state.device_info
        stdscr.addstr(0, 0, "== Device Info ==")
        if info:
            stdscr.addstr(2, 0, f"Vendor ID:    {info['vendor_id']:#010x}")
            stdscr.addstr(3, 0, f"Product Code: {info['product_code']:#010x}")
            stdscr.addstr(4, 0, f"Revision:     {info['revision']:#010x}")
            stdscr.addstr(5, 0, f"Serial:       {info['serial']:#010x}")
        else:
            stdscr.addstr(2, 0, "(no data yet - press 'g' to request it)")
        stdscr.addstr(7, 0, "[g] Get device info   [b] Back")
        stdscr.refresh()

        key = stdscr.getkey()
        if key == "g":
            sock.sendall(encode_device_info_request())
        elif key == "b":
            return


def status_str(value):
    if value is None:
        return "UNKNOWN"
    return "UP" if value else "DOWN"


def main_menu(stdscr, sock: socket.socket, state: SharedState):
    curses.curs_set(0)
    options = ["Commands", "Telemetry", "Device Info", "Exit"]
    selected = 0

    while True:
        stdscr.nodelay(False)
        stdscr.clear()
        stdscr.addstr(0, 0, "== wizard-core UI ==")
        with state.lock:
            gw = state.gateway_connected
            mcu = state.mcu_responding
        stdscr.addstr(1, 0, f"engine: UP   gateway: {status_str(gw)}   mcu: {status_str(mcu)}")
        for i, opt in enumerate(options):
            prefix = "> " if i == selected else "  "
            stdscr.addstr(3 + i, 0, prefix + opt)
        stdscr.addstr(3 + len(options) + 1, 0, "(arrows to move, Enter to select)")
        stdscr.refresh()

        key = stdscr.getkey()
        if key in ("KEY_UP", "k"):
            selected = (selected - 1) % len(options)
        elif key in ("KEY_DOWN", "j"):
            selected = (selected + 1) % len(options)
        elif key in ("\n", "\r"):
            choice = options[selected]
            if choice == "Commands":
                draw_commands_screen(stdscr, sock, state)
            elif choice == "Telemetry":
                draw_telemetry_screen(stdscr, state)
            elif choice == "Device Info":
                draw_device_info_screen(stdscr, sock, state)
            elif choice == "Exit":
                return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default="/tmp/wizard-ui.sock")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(args.socket)

    state = SharedState()
    thread = threading.Thread(target=receiver_thread, args=(sock, state), daemon=True)
    thread.start()

    try:
        curses.wrapper(main_menu, sock, state)
    finally:
        sock.close()


if __name__ == "__main__":
    main()