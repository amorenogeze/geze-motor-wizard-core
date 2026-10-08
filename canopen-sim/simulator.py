#!/usr/bin/env python3
"""Simulated SOLO PICO on a SocketCAN bus (dev/test only).

Emulates the subset of the SOLO PICO CANopen behaviour that device-gateway
uses with device-gateway/canopen/devices/solopico.json:

  * SDO server on node 1: reads answer with 0x42 (expedited, size not
    indicated), writes (0x22 or size-indicated) answer 0x60, unknown objects
    answer an abort. Values are 32-bit little-endian; floats are Q15.17
    (value * 131072), like the real PICO.
  * TPDOs are synchronous: configured through 0x1814..0x1819 (sub 1 = COB-ID
    | 0x80000000 enable, sub 2 = every N SYNCs) and sent when a SYNC (0x080)
    arrives.
  * A toy brushed motor with the three SOLO control modes (control_mode
    0x3016): 1 torque (the torque reference becomes current, speed follows
    it), 0 speed (speed follows the speed reference, direction from 0x300C),
    2 position (moves to the position reference at up to speed_limit).
    Position integrates speed in encoder counts (4 x encoder_lines per rev);
    reset_position (0x301F) = 1 sets it to 0.

Starts like a factory PICO (motor type BLDC, analogue command mode), so the
gateway's configure step is exercised. Requires a SocketCAN interface:

    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0
"""

import argparse
import random
import threading
import time

import can

NODE_ID = 1
SYNC_ID = 0x080
SDO_REQUEST_ID = 0x600 + NODE_ID
SDO_RESPONSE_ID = 0x580 + NODE_ID

ABORT_NO_OBJECT = 0x06020000
ABORT_VALUE_RANGE = 0x06090030

Q17 = 131072.0


def to_q17(value: float) -> int:
    return int(round(value * Q17)) & 0xFFFFFFFF


def from_q17(raw: int) -> float:
    if raw & 0x80000000:
        raw -= 1 << 32
    return raw / Q17


def to_u32(value: int) -> int:
    return value & 0xFFFFFFFF


def le32(value: int) -> bytes:
    return to_u32(value).to_bytes(4, "little")


# Object dictionary: (index, sub) -> raw 32-bit value. Factory defaults.
FACTORY_OD = {
    (0x1001, 0): 0,                # error register
    (0x3002, 0): 0,                # command mode: analogue
    (0x3003, 0): to_q17(16.0),     # current limit A
    (0x3004, 0): 0,                # torque reference A (q17)
    (0x3005, 0): 0,                # speed reference rpm
    (0x3007, 0): 0,                # motor identification
    (0x3008, 0): 0,                # drive enable
    (0x3009, 0): 20,               # pwm kHz
    (0x300C, 0): 0,                # direction 0 CCW / 1 CW
    (0x3010, 0): 1024,             # encoder lines (factory default; the profile overwrites it)
    (0x3011, 0): 8000,             # speed limit rpm
    (0x3013, 0): 0,                # feedback mode
    (0x3015, 0): 1,                # motor type: BLDC/PMSM (factory)
    (0x3016, 0): 0,                # control mode: 0 speed, 1 torque, 2 position
    (0x301B, 0): 0,                # position reference counts
    (0x301F, 0): 0,                # reset position (write 1)
    (0x3031, 0): to_q17(24.0),     # bus voltage
    (0x3032, 0): 0,                # DC motor current Im (q17)
    (0x3034, 0): 0,                # Iq feedback (q17)
    (0x3036, 0): 0,                # speed feedback rpm
    (0x3037, 0): 0,                # position counts
    (0x3039, 0): to_q17(31.5),     # board temperature
    (0x303A, 0): 0x0000B020,       # firmware version
    (0x303B, 0): 0x00000123,       # hardware version
}
# TPDO parameter objects -> the object each one transmits.
TPDO_SOURCES = {
    0x1814: (0x3037, 0),  # position counts
    0x1815: (0x3036, 0),  # speed
    0x1816: (0x3034, 0),  # Iq
    0x1818: (0x1001, 0),  # error register
    0x1819: (0x3039, 0),  # board temperature
}
for pdo in TPDO_SOURCES:
    FACTORY_OD[(pdo, 1)] = 0  # disabled
    FACTORY_OD[(pdo, 2)] = 1


class Pico:
    """Thread-safe object dictionary + motor model."""

    def __init__(self):
        self.lock = threading.Lock()
        self.od = dict(FACTORY_OD)
        self.sync_counters = {pdo: 0 for pdo in TPDO_SOURCES}
        self._speed = 0.0
        self._position = 0.0

    def read(self, index: int, sub: int):
        with self.lock:
            return self.od.get((index, sub))

    def write(self, index: int, sub: int, value: int) -> int:
        """Returns 0 or an abort code."""
        with self.lock:
            if (index, sub) not in self.od:
                return ABORT_NO_OBJECT
            if index in TPDO_SOURCES and sub == 1 and value & 0x80000000:
                cob = value & 0x7FF
                if not 0x280 <= cob <= 0x2FF:
                    return ABORT_VALUE_RANGE
            if index in TPDO_SOURCES and sub == 2 and not (0 <= value < 12 or value == 0xFF):
                return ABORT_VALUE_RANGE
            self.od[(index, sub)] = value
            if index == 0x3007 and value == 1:
                print("motor identification requested (simulated, instant)")
            if index == 0x301F and value == 1:
                self._position = 0.0
                self.od[(0x3037, 0)] = 0
                self.od[(0x301F, 0)] = 0  # self-clearing, like a command
            return 0

    def tpdos_for_sync(self):
        """Frames to send for one SYNC."""
        frames = []
        with self.lock:
            for pdo, source in TPDO_SOURCES.items():
                cfg = self.od[(pdo, 1)]
                every = self.od[(pdo, 2)] or 1
                if not cfg & 0x80000000:
                    continue
                self.sync_counters[pdo] += 1
                if self.sync_counters[pdo] >= every:
                    self.sync_counters[pdo] = 0
                    frames.append((cfg & 0x7FF, le32(self.od[source])))
        return frames

    def step(self, dt: float):
        """Toy brushed motor in torque, speed or position control."""
        with self.lock:
            enabled = self.od[(0x3008, 0)] == 1
            mode = self.od[(0x3016, 0)]
            limit = from_q17(self.od[(0x3003, 0)])
            speed_limit = float(self.od[(0x3011, 0)])
            direction = 1.0 if self.od[(0x300C, 0)] == 1 else -1.0
            counts_per_rev = 4.0 * max(1, self.od[(0x3010, 0)])
            lag = 0.2  # s, speed follows its target with this time constant

            if not enabled:
                target = 0.0
                lag = 1.0  # freewheel: coasts down slowly
            elif mode == 1:  # torque: current = reference, speed follows current
                ref = max(-limit, min(limit, from_q17(self.od[(0x3004, 0)])))
                target = direction * 3000.0 * ref  # rpm per A
            elif mode == 0:  # speed: magnitude from the reference, sign from the direction
                ref = self.od[(0x3005, 0)]
                if ref & 0x80000000:
                    ref -= 1 << 32
                target = direction * abs(ref)
            else:  # position: P controller towards the reference
                ref = self.od[(0x301B, 0)]
                if ref & 0x80000000:
                    ref -= 1 << 32
                target = (ref - self._position) * 0.5  # rpm per count of error
                lag = 0.05
            target = max(-speed_limit, min(speed_limit, target))

            previous = self._speed
            self._speed += (target - self._speed) * min(1.0, dt / lag)
            self._position += self._speed / 60.0 * counts_per_rev * dt

            if not enabled:
                current = 0.0
            elif mode == 1:
                current = max(-limit, min(limit, from_q17(self.od[(0x3004, 0)])))
            else:  # what it takes to hold the speed plus to accelerate
                accel = (self._speed - previous) / dt if dt > 0 else 0.0
                current = min(limit, 0.1 + abs(self._speed) / 3000.0 + abs(accel) / 20000.0)
            if enabled and current:
                current += random.uniform(-0.02, 0.02)  # ripple

            self.od[(0x3032, 0)] = to_q17(current)
            self.od[(0x3034, 0)] = to_q17(current)
            self.od[(0x3036, 0)] = to_u32(int(round(self._speed)))
            self.od[(0x3037, 0)] = to_u32(int(self._position))


def can_loop(bus: can.Bus, pico: Pico, stop: threading.Event) -> None:
    """Answers SDOs and sends TPDOs on SYNC."""
    while not stop.is_set():
        msg = bus.recv(timeout=0.2)
        if msg is None:
            continue

        if msg.arbitration_id == SYNC_ID:
            for cob, data in pico.tpdos_for_sync():
                bus.send(can.Message(arbitration_id=cob, data=data, is_extended_id=False))
            continue

        if msg.arbitration_id != SDO_REQUEST_ID or len(msg.data) < 8:
            continue

        cs = msg.data[0]
        index = msg.data[1] | (msg.data[2] << 8)
        sub = msg.data[3]
        idx_sub = bytes(msg.data[1:4])
        value = int.from_bytes(msg.data[4:8], "little")

        if cs == 0x40:  # upload (read)
            v = pico.read(index, sub)
            data = (bytes([0x42]) + idx_sub + le32(v)) if v is not None else \
                   (bytes([0x80]) + idx_sub + le32(ABORT_NO_OBJECT))
        elif cs & 0xE0 == 0x20:  # download (write): 0x22 or size-indicated
            if cs & 0x01:  # size indicated: mask unused bytes
                unused = (cs >> 2) & 0x03
                value &= 0xFFFFFFFF >> (8 * unused)
            abort = pico.write(index, sub, value)
            data = (bytes([0x60]) + idx_sub + b"\x00\x00\x00\x00") if not abort else \
                   (bytes([0x80]) + idx_sub + le32(abort))
        else:
            continue

        bus.send(can.Message(arbitration_id=SDO_RESPONSE_ID, data=data, is_extended_id=False))


def motor_loop(pico: Pico, stop: threading.Event) -> None:
    last = time.monotonic()
    while not stop.is_set():
        time.sleep(0.005)
        now = time.monotonic()
        pico.step(now - last)
        last = now


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iface", default="vcan0", help="SocketCAN interface (default: vcan0)")
    args = parser.parse_args()

    bus = can.Bus(channel=args.iface, interface="socketcan")
    pico = Pico()
    stop = threading.Event()

    threads = [threading.Thread(target=can_loop, args=(bus, pico, stop), daemon=True),
               threading.Thread(target=motor_loop, args=(pico, stop), daemon=True)]
    for t in threads:
        t.start()

    print(f"SOLO PICO simulator on {args.iface}, node id {NODE_ID} (factory state: BLDC, analogue)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        stop.set()
        for t in threads:
            t.join()
        bus.shutdown()


if __name__ == "__main__":
    main()
