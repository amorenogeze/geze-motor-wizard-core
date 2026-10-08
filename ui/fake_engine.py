#!/usr/bin/env python3
"""Fake wizard-engine + device-gateway + SOLO PICO, for testing ui/motion_cli.py without hardware.

Listens on the UI socket like wizard-engine and behaves like the whole chain
behind it: it runs the same sequences as solopico.json (stop first on a mode
change, ramps at the profile rates, live update for the same mode, HOME
refused while running), simulates a motor with an encoder, streams
PositionEvent / SpeedEvent / CurrentEvent every 100 ms while running, and
answers every command with RunStopStatusEvent (0x41) once the sequence is done.

    python3 ui/fake_engine.py                    # /tmp/wizard-ui.sock, real ramp times
    python3 ui/fake_engine.py --fast             # ramps 10x faster
    python3 ui/fake_engine.py --no-reply         # never send 0x41 (test the UI timeout)
    python3 ui/fake_engine.py --sock /tmp/x.sock

Stop the real wizard-engine first, it uses the same socket path.
"""
import argparse
import os
import queue
import socket
import threading
import time

from motion_cli import (
    CURRENT_EVENT, DEVICE_INFO_REQUEST, DEVICE_INFO_RESPONSE, DIR_CCW, LIMITS, MCU_STATUS,
    MODE_HOME, MODE_NAMES, MODE_POSITION, MODE_SPEED, MODE_STOP, MODE_TORQUE, POSITION_EVENT,
    RUN_STOP_STATUS, SET_MOTION, SET_RUN_STOP, SPEED_EVENT, FrameReader, pack, unpack,
)

COUNTS_PER_REV = 4 * 500         # quadrature x encoder_lines (solopico.json placeholder)
SPEED_LIMIT_RPM = 4000
TORQUE_RAMP_A_S = 0.5            # motion.modes.torque.ramp_rate, also stop
SPEED_RAMP_RPM_S = 500           # motion.modes.speed.ramp_rate
STOP_SPEED_RAMP_RPM_S = 1000     # stop: speed_reference ramp
LEGACY_RUN = (MODE_TORQUE, 5750, 0)   # profile "run": torque 5.75 A, CW
CONTROL_MODE = {MODE_TORQUE: 1, MODE_SPEED: 0, MODE_POSITION: 2}


def now_us():
    return int(time.time() * 1e6)


def log(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


class Motor:
    """Brushed DC motor + SOLO control loops, crude but plausible."""

    def __init__(self):
        self.lock = threading.Lock()
        self.enabled = False
        self.mode = MODE_TORQUE
        self.sign = 1               # +1 CW, -1 CCW
        self.torque_ref = 0.0       # A
        self.speed_ref = 0.0        # rpm, magnitude
        self.pos_ref = 0            # counts
        self.speed = 0.0            # rpm, signed
        self.pos = 0.0              # counts
        self.current = 0.0          # A

    def step(self, dt):
        with self.lock:
            if not self.enabled:
                accel = -self.speed / 1.0                    # freewheel, coasts down
                self.current = 0.0
            elif self.mode == MODE_TORQUE:
                accel = 2000.0 * self.torque_ref * self.sign - 5.0 * self.speed   # ~400 rpm/A no-load
                self.current = self.torque_ref
            else:
                if self.mode == MODE_SPEED:
                    target = self.speed_ref * self.sign
                    tau = 0.15
                else:
                    target = max(-SPEED_LIMIT_RPM, min(SPEED_LIMIT_RPM, 2.0 * (self.pos_ref - self.pos)))
                    tau = 0.08
                accel = (target - self.speed) / tau
                self.current = min(10.0, 0.15 + 0.0004 * abs(self.speed) + abs(accel) / 4000.0)
            self.speed += accel * dt
            self.pos += self.speed / 60.0 * COUNTS_PER_REV * dt


class FakeChain:
    def __init__(self, sock_path, fast, no_reply):
        self.sock_path = sock_path
        self.time_scale = 0.1 if fast else 1.0
        self.no_reply = no_reply
        self.motor = Motor()
        self.running = False
        self.active = None              # (mode, direction) while running
        self.stop_requested = threading.Event()
        self.commands = queue.Queue()
        self.client = None
        self.client_lock = threading.Lock()

    # ------------------------------------------------ socket
    def send(self, data):
        with self.client_lock:
            if self.client:
                try:
                    self.client.sendall(data)
                except OSError:
                    pass

    def serve(self):
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(self.sock_path)
        srv.listen(1)
        log(f"fake engine listening on {self.sock_path} ({'fast' if self.time_scale < 1 else 'real'} ramps"
            f"{', NO 0x41 replies' if self.no_reply else ''})")
        threading.Thread(target=self.sim_loop, daemon=True).start()
        threading.Thread(target=self.telemetry_loop, daemon=True).start()
        threading.Thread(target=self.command_loop, daemon=True).start()
        try:
            while True:
                conn, _ = srv.accept()
                with self.client_lock:
                    if self.client:
                        self.client.close()
                    self.client = conn
                log("UI connected")
                self.send(pack(MCU_STATUS, 1))
                threading.Thread(target=self.reader, args=(conn,), daemon=True).start()
        except KeyboardInterrupt:
            pass
        finally:
            srv.close()
            os.unlink(self.sock_path)

    def reader(self, conn):
        frames = FrameReader()
        while True:
            try:
                data = conn.recv(4096)
            except OSError:
                data = b""
            if not data:
                log("UI disconnected")
                return
            for msg_type, payload in frames.feed(data):
                fields = unpack(msg_type, payload)
                if fields is None:
                    log(f"ignored malformed/unknown message 0x{msg_type:02x} ({len(payload)} bytes)")
                    continue
                if msg_type == DEVICE_INFO_REQUEST:
                    self.send(pack(DEVICE_INFO_RESPONSE, 0, 0x1, 0x0000B040, 0))
                    log("DeviceInfoRequest -> DeviceInfoResponse")
                    continue
                if msg_type == SET_RUN_STOP:
                    cmd = LEGACY_RUN if fields[0] else (MODE_STOP, 0, 0)
                elif msg_type == SET_MOTION:
                    cmd = fields
                else:
                    log(f"ignored message 0x{msg_type:02x}")
                    continue
                if cmd[0] == MODE_STOP:
                    self.stop_requested.set()   # like stop_requested_: a ramp up gives way
                self.commands.put(cmd)

    # ------------------------------------------------ simulation + telemetry
    def sim_loop(self):
        dt = 0.01
        while True:
            time.sleep(dt)
            self.motor.step(dt)

    def telemetry_loop(self):
        while True:
            time.sleep(0.1)
            if not self.running:            # telemetry_only_while_running
                continue
            m = self.motor
            with m.lock:
                pos, speed, cur = int(round(m.pos)), int(round(m.speed)), int(round(m.current * 1000))
            ts = now_us()
            self.send(pack(POSITION_EVENT, ts, pos))
            self.send(pack(SPEED_EVENT, ts, speed))
            self.send(pack(CURRENT_EVENT, ts, max(-32768, min(32767, cur))))

    # ------------------------------------------------ command handling (= device-gateway)
    def ramp(self, attr, target, rate, interruptible):
        """Ramps motor.<attr> to target at rate units/s (20 ms steps). False if a STOP interrupted it."""
        m = self.motor
        with m.lock:
            start = getattr(m, attr)
        duration = abs(target - start) / rate * self.time_scale if rate else 0
        t0 = time.monotonic()
        while True:
            done = (time.monotonic() - t0) / duration if duration else 1.0
            if done >= 1.0:
                break
            if interruptible and self.stop_requested.is_set():
                log(f"  ramp {attr} interrupted by STOP")
                return False
            with m.lock:
                setattr(m, attr, start + (target - start) * done)
            time.sleep(0.02)
        with m.lock:
            setattr(m, attr, target)
        return True

    def stop_sequence(self, why):
        m = self.motor
        log(f"  {why}: speed_reference -> 0, torque_reference -> 0 (ramped), drive_enable = 0")
        self.running = False
        with m.lock:
            if m.mode == MODE_SPEED:            # ramp_start_object: speed_feedback
                m.speed_ref = min(m.speed_ref, abs(m.speed))
            m.torque_ref = min(m.torque_ref, m.current) if m.mode == MODE_TORQUE else 0.0
        self.ramp("speed_ref", 0.0, STOP_SPEED_RAMP_RPM_S, False)
        self.ramp("torque_ref", 0.0, TORQUE_RAMP_A_S, False)
        with m.lock:
            m.enabled = False
        self.active = None

    def handle(self, mode, setpoint, direction):
        m = self.motor
        name = MODE_NAMES.get(mode, f"mode {mode}")
        if mode in LIMITS:
            lo, hi, unit = LIMITS[mode]
            clamped = max(lo, min(hi, setpoint))
            if clamped != setpoint:
                log(f"  setpoint {setpoint} {unit} clamped to {clamped}")
            setpoint = clamped
            name += f" {setpoint} {unit}"
            if mode != MODE_POSITION:
                name += " CCW" if direction == DIR_CCW else " CW"
        log(f"command {name}")

        if mode == MODE_STOP:
            self.stop_requested.clear()
            self.stop_sequence("stop")
            return
        if mode == MODE_HOME:
            if self.running:
                log("  HOME refused while running")
            else:
                with m.lock:
                    m.pos = 0.0
                    m.pos_ref = 0
                log("  reset_position = 1 -> position 0")
            return
        if mode not in LIMITS:
            log(f"  unknown mode {mode}, ignored")
            return

        key = (mode, direction if mode != MODE_POSITION else 0)
        if self.running and self.active == key:
            log("  live update: only the reference changes")
            ok = self.apply_reference(mode, setpoint)
        else:
            if self.running:
                self.stop_sequence("mode/direction change, stop first")
            log(f"  control_mode = {CONTROL_MODE[mode]}, direction, drive_enable = 1, reference")
            with m.lock:
                m.mode = mode
                m.sign = -1 if (direction == DIR_CCW and mode != MODE_POSITION) else 1
                m.torque_ref = 0.0
                m.speed_ref = 0.0
                if mode == MODE_POSITION:
                    m.pos_ref = setpoint    # target first, then enable
                m.enabled = True
            self.running = True
            self.active = key
            ok = self.apply_reference(mode, setpoint)
        if not ok:   # half-applied run -> stop, like CanopenTranslator::set_run_stop
            self.stop_sequence("stop (after interrupted run)")

    def apply_reference(self, mode, setpoint):
        if mode == MODE_TORQUE:
            return self.ramp("torque_ref", setpoint / 1000.0, TORQUE_RAMP_A_S, True)
        if mode == MODE_SPEED:
            return self.ramp("speed_ref", float(setpoint), SPEED_RAMP_RPM_S, True)
        with self.motor.lock:
            self.motor.pos_ref = setpoint
        return True

    def command_loop(self):
        while True:
            cmd = self.commands.get()
            self.handle(*cmd)
            with self.motor.lock:
                running = self.motor.enabled     # status read-back: drive_enable
            if self.no_reply:
                log("  (--no-reply: RunStopStatusEvent NOT sent)")
                continue
            self.send(pack(RUN_STOP_STATUS, now_us(), 1 if running else 0))
            log(f"  -> RunStopStatusEvent {'RUNNING' if running else 'STOPPED'}")


def main():
    ap = argparse.ArgumentParser(description="fake wizard-engine for ui.py testing")
    ap.add_argument("--sock", default="/tmp/wizard-ui.sock")
    ap.add_argument("--fast", action="store_true", help="ramps 10x faster")
    ap.add_argument("--no-reply", action="store_true", help="never answer with 0x41")
    args = ap.parse_args()
    FakeChain(args.sock, args.fast, args.no_reply).serve()


if __name__ == "__main__":
    main()
