#!/usr/bin/env python3
"""wizard-core motion command line (test UI for SetMotionCommand 0x43).

Talks to wizard-engine on /tmp/wizard-ui.sock (or to fake_engine.py when there
is no SOLO PICO). Sends SetMotionCommand (0x43): torque / speed / position /
stop / home, plus the legacy SetRunStopCommand (0x40), and prints every event
that comes back.

    python3 ui/motion_cli.py                         # interactive
    python3 ui/motion_cli.py --sock /tmp/x.sock      # other socket
    python3 ui/motion_cli.py --script "t 2000; wait 3; s 1500 ccw; wait 3; stop"

Commands (sending the same mode + direction again while running = live update):
    t <mA> [cw|ccw]     torque          e.g. t 5750
    s <rpm> [cw|ccw]    speed           e.g. s 1500 ccw
    p <counts>          position        absolute, 0 = home, e.g. p 4000
    stop                stop (0x43 mode 0)
    home                position := 0 (refused while running)
    run / halt          legacy 0x40 RUN (fixed torque) / STOP
    info                DeviceInfoRequest
    tel                 telemetry printing on/off (1 line per second)
    wait <s>            pause (useful in --script)
    help, quit
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

# ---------------------------------------------------------------- protocol
# Frame: [type u8][payload length u16 LE][payload]. Must match shared/message.h
# and protocol_interactive.html.

PING = 0x01
PONG = 0x02
POSITION_EVENT = 0x21        # <Qi  timestamp_us, position (counts)
SPEED_EVENT = 0x22           # <Qi  timestamp_us, speed (rpm)
CURRENT_EVENT = 0x23         # <Qh  timestamp_us, current (mA)
DEVICE_INFO_REQUEST = 0x30   # empty
DEVICE_INFO_RESPONSE = 0x31  # <IIII vendor, product, revision, serial
SET_RUN_STOP = 0x40          # <B   run
RUN_STOP_STATUS = 0x41       # <QB  timestamp_us, running
SET_MOTION = 0x43            # <BiB mode, setpoint, direction
MCU_STATUS = 0x51            # <B   responding

MODE_STOP, MODE_TORQUE, MODE_SPEED, MODE_POSITION, MODE_HOME = 0, 1, 2, 3, 4
MODE_NAMES = {0: "STOP", 1: "TORQUE", 2: "SPEED", 3: "POSITION", 4: "HOME"}
DIR_CW, DIR_CCW = 0, 1

# Same limits as solopico.json -> motion.modes (device-gateway clamps to these).
LIMITS = {
    MODE_TORQUE: (0, 10000, "mA"),
    MODE_SPEED: (0, 4000, "rpm"),
    MODE_POSITION: (-2000000, 2000000, "counts"),
}

PAYLOAD_FORMAT = {
    PING: "", PONG: "",
    POSITION_EVENT: "<Qi", SPEED_EVENT: "<Qi", CURRENT_EVENT: "<Qh",
    DEVICE_INFO_REQUEST: "", DEVICE_INFO_RESPONSE: "<IIII",
    SET_RUN_STOP: "<B", RUN_STOP_STATUS: "<QB", SET_MOTION: "<BiB", MCU_STATUS: "<B",
}

# The real gateway answers 0x41 only after the whole sequence, including the
# ramps (worst case: stop from 10 A + ramp up to 10 A at 0.5 A/s = 40 s).
REPLY_TIMEOUT_S = 45.0


def pack(msg_type, *values):
    payload = struct.pack(PAYLOAD_FORMAT[msg_type], *values) if values else b""
    return struct.pack("<BH", msg_type, len(payload)) + payload


def unpack(msg_type, payload):
    """Payload fields as a tuple, or None if the type is unknown or the size is wrong."""
    fmt = PAYLOAD_FORMAT.get(msg_type)
    if fmt is None or len(payload) != struct.calcsize(fmt):
        return None
    return struct.unpack(fmt, payload) if fmt else ()


class FrameReader:
    """Stream reassembly: feed() raw bytes, get back the complete (type, payload) frames."""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        out = []
        while len(self.buf) >= 3:
            msg_type = self.buf[0]
            length = struct.unpack("<H", self.buf[1:3])[0]
            if len(self.buf) < 3 + length:
                break
            out.append((msg_type, self.buf[3:3 + length]))
            self.buf = self.buf[3 + length:]
        return out


# ---------------------------------------------------------------- UI

class TestUI:
    def __init__(self, sock_path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(sock_path)
        self.send_lock = threading.Lock()
        self.print_lock = threading.Lock()
        self.pending = []            # [(description, sent_at)] waiting for 0x41, FIFO
        self.pending_lock = threading.Lock()
        self.tel = {"pos": None, "speed": None, "current": None}
        self.show_tel = True
        self.running = None
        self.closed = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._ticker, daemon=True).start()

    # -------- output
    def out(self, text):
        with self.print_lock:
            sys.stdout.write("\r" + text + "\n")
            sys.stdout.flush()

    # -------- sending
    def _send(self, data):
        with self.send_lock:
            self.sock.sendall(data)

    def _expect_reply(self, what):
        with self.pending_lock:
            self.pending.append((what, time.monotonic()))

    def motion(self, mode, setpoint=0, direction=DIR_CW):
        if mode in LIMITS:
            lo, hi, unit = LIMITS[mode]
            if not lo <= setpoint <= hi:
                self.out(f"  note: {setpoint} {unit} is outside {lo}..{hi}, the gateway will clamp it")
        what = MODE_NAMES[mode]
        if mode in LIMITS:
            what += f" {setpoint} {LIMITS[mode][2]}"
        if mode in (MODE_TORQUE, MODE_SPEED):
            what += " CCW" if direction == DIR_CCW else " CW"
        self._expect_reply(what)
        self._send(pack(SET_MOTION, mode, setpoint, direction))
        self.out(f"> 0x43 SetMotionCommand {what}")

    def legacy_run(self, run):
        what = "legacy RUN" if run else "legacy STOP"
        self._expect_reply(what)
        self._send(pack(SET_RUN_STOP, 1 if run else 0))
        self.out(f"> 0x40 SetRunStopCommand {what}")

    def device_info(self):
        self._send(pack(DEVICE_INFO_REQUEST))
        self.out("> 0x30 DeviceInfoRequest")

    # -------- receiving
    def _reader(self):
        reader = FrameReader()
        while True:
            try:
                data = self.sock.recv(4096)
            except OSError:
                data = b""
            if not data:
                self.out("! connection closed by wizard-engine")
                self.closed.set()
                return
            for msg_type, payload in reader.feed(data):
                self._handle(msg_type, payload)

    def _handle(self, msg_type, payload):
        fields = unpack(msg_type, payload)
        if fields is None:
            self.out(f"! unknown or malformed message 0x{msg_type:02x} ({len(payload)} bytes): {payload.hex(' ')}")
            return
        if msg_type == POSITION_EVENT:
            self.tel["pos"] = fields[1]
        elif msg_type == SPEED_EVENT:
            self.tel["speed"] = fields[1]
        elif msg_type == CURRENT_EVENT:
            self.tel["current"] = fields[1]
        elif msg_type == RUN_STOP_STATUS:
            self.running = bool(fields[1])
            with self.pending_lock:
                what, t0 = self.pending.pop(0) if self.pending else (None, None)
            state = "RUNNING" if self.running else "STOPPED"
            if what:
                self.out(f"< 0x41 RunStopStatusEvent {state}  (reply to {what}, after {time.monotonic() - t0:.1f} s)")
            else:
                self.out(f"< 0x41 RunStopStatusEvent {state}")
        elif msg_type == MCU_STATUS:
            self.out(f"< 0x51 McuStatusEvent {'responding' if fields[0] else 'NOT responding'}")
        elif msg_type == DEVICE_INFO_RESPONSE:
            v, p, r, s = fields
            self.out(f"< 0x31 DeviceInfoResponse vendor=0x{v:08x} product=0x{p:08x} revision=0x{r:08x} serial=0x{s:08x}")
        else:
            self.out(f"< 0x{msg_type:02x} {fields}")

    def _ticker(self):
        # Telemetry summary once per second; reply timeout check.
        last = None
        while not self.closed.is_set():
            time.sleep(1.0)
            with self.pending_lock:
                late = [p for p in self.pending if time.monotonic() - p[1] > REPLY_TIMEOUT_S]
                self.pending = [p for p in self.pending if p not in late]
            for what, _ in late:
                self.out(f"! TIMEOUT: no RunStopStatusEvent (0x41) for {what} within {REPLY_TIMEOUT_S:.0f} s")
            snap = (self.tel["pos"], self.tel["speed"], self.tel["current"])
            if self.show_tel and snap != last and any(v is not None for v in snap):
                pos, spd, cur = snap
                self.out(f"  telemetry  pos={pos} counts  speed={spd} rpm  current={cur} mA")
                last = snap


# ---------------------------------------------------------------- command line

def parse_dir(args, i):
    if len(args) > i:
        d = args[i].lower()
        if d in ("cw", "0"):
            return DIR_CW
        if d in ("ccw", "1"):
            return DIR_CCW
        raise ValueError(f"direction must be cw or ccw, not '{args[i]}'")
    return DIR_CW


def execute(ui, line):
    """Runs one command. Returns False to quit."""
    args = line.split()
    if not args:
        return True
    cmd = args[0].lower()
    try:
        if cmd in ("q", "quit", "exit"):
            return False
        if cmd in ("h", "help", "?"):
            print(__doc__.split("Commands", 1)[1].split("\n", 1)[1])
        elif cmd in ("t", "torque"):
            ui.motion(MODE_TORQUE, int(args[1]), parse_dir(args, 2))
        elif cmd in ("s", "speed"):
            ui.motion(MODE_SPEED, int(args[1]), parse_dir(args, 2))
        elif cmd in ("p", "pos", "position"):
            ui.motion(MODE_POSITION, int(args[1]))
        elif cmd == "stop":
            ui.motion(MODE_STOP)
        elif cmd == "home":
            ui.motion(MODE_HOME)
        elif cmd == "run":
            ui.legacy_run(True)
        elif cmd == "halt":
            ui.legacy_run(False)
        elif cmd == "info":
            ui.device_info()
        elif cmd == "tel":
            ui.show_tel = not ui.show_tel
            ui.out(f"  telemetry printing {'on' if ui.show_tel else 'off'}")
        elif cmd == "wait":
            time.sleep(float(args[1]))
        else:
            ui.out(f"? unknown command '{cmd}', type help")
    except IndexError:
        ui.out(f"? '{cmd}' needs a value, e.g. t 2000 / s 1500 ccw / p 4000 (type help)")
    except ValueError as e:
        ui.out(f"? {e} (type help)")
    except OSError as e:
        ui.out(f"! send failed: {e}")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="wizard-core test UI")
    ap.add_argument("--sock", default="/tmp/wizard-ui.sock")
    ap.add_argument("--script", help='commands separated by ";", e.g. "t 2000; wait 3; stop; wait 5"')
    args = ap.parse_args()

    if not os.path.exists(args.sock):
        sys.exit(f"{args.sock} does not exist: start wizard-engine (or fake_engine.py) first")
    try:
        ui = TestUI(args.sock)
    except OSError as e:
        sys.exit(f"cannot connect to {args.sock}: {e}")
    ui.out(f"connected to {args.sock}. Type help for commands.")

    if args.script:
        for line in args.script.split(";"):
            if not execute(ui, line.strip()):
                break
        time.sleep(0.3)  # let the last events print
        return

    while not ui.closed.is_set():
        try:
            line = input("wizard> ")
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not execute(ui, line):
            break


if __name__ == "__main__":
    main()
