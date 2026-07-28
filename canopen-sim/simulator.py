#!/usr/bin/env python3

"""
Node ID = 1. Responds to SDO reads of the Identity Object (0x1018) and
cyclically emits TPDO1 (position), TPDO2 (velocity), TPDO3 (current).
Requires a SocketCAN interface (e.g. vcan0) already up:

    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0
"""

import argparse
import math
import threading
import time

import can

NODE_ID = 1

TPDO1_ID = 0x180 + NODE_ID  # position
TPDO2_ID = 0x280 + NODE_ID  # velocity
TPDO3_ID = 0x380 + NODE_ID  # current
SDO_REQUEST_ID = 0x600 + NODE_ID
SDO_RESPONSE_ID = 0x580 + NODE_ID

SCS_INITIATE_UPLOAD_REQUEST = 0x40
SCS_INITIATE_UPLOAD_RESPONSE_EXPEDITED_4BYTES = 0x43
SCS_ABORT = 0x80

IDENTITY_INDEX = 0x1018

# Hardcoded Identity Object values for V1 (placeholders, no real vendor).
IDENTITY_VALUES = {
    0x01: 0x0000A1A0,  # Vendor ID
    0x02: 0x00000001,  # Product Code
    0x03: 0x00010000,  # Revision Number
    0x04: 0x12345678,  # Serial Number
}


def u32_to_le_bytes(value: int) -> bytes:
    return value.to_bytes(4, byteorder="little", signed=False)


def i32_to_le_bytes(value: int) -> bytes:
    return value.to_bytes(4, byteorder="little", signed=True)


def i16_to_le_bytes(value: int) -> bytes:
    return value.to_bytes(2, byteorder="little", signed=True)


class MotorModel:
    """Toy motor: position ramps up, velocity ~constant, current noisy."""

    def __init__(self):
        self._start = time.monotonic()

    def position(self) -> int:
        t = time.monotonic() - self._start
        return int(1000 * t) % 1_000_000  # encoder counts, wraps for demo purposes

    def velocity(self) -> int:
        return 1000  # counts/s, constant in this toy model

    def current(self) -> int:
        t = time.monotonic() - self._start
        return int(500 + 100 * math.sin(t))  # mA, oscillating around 500mA


def sdo_responder(bus: can.Bus, stop: threading.Event) -> None:
    """Answers Identity Object SDO upload requests. Aborts anything else."""
    while not stop.is_set():
        msg = bus.recv(timeout=0.2)
        if msg is None or msg.arbitration_id != SDO_REQUEST_ID:
            continue

        cs = msg.data[0]
        index = msg.data[1] | (msg.data[2] << 8)
        subindex = msg.data[3]

        if cs != SCS_INITIATE_UPLOAD_REQUEST:
            continue  # only upload (read) requests are supported in V1

        if index == IDENTITY_INDEX and subindex in IDENTITY_VALUES:
            data = bytes([SCS_INITIATE_UPLOAD_RESPONSE_EXPEDITED_4BYTES]) + \
                msg.data[1:4] + u32_to_le_bytes(IDENTITY_VALUES[subindex])
        else:
            data = bytes([SCS_ABORT]) + msg.data[1:4] + b"\x00\x00\x00\x00"

        bus.send(can.Message(arbitration_id=SDO_RESPONSE_ID, data=data, is_extended_id=False))


def pdo_sender(bus: can.Bus, motor: MotorModel, stop: threading.Event) -> None:
    """Sends TPDO1/TPDO2 every 10ms, TPDO3 every 2ms (see spec, section 2)."""
    next_slow = time.monotonic()
    next_fast = time.monotonic()

    while not stop.is_set():
        now = time.monotonic()

        if now >= next_slow:
            bus.send(can.Message(arbitration_id=TPDO1_ID, data=i32_to_le_bytes(motor.position()),
                                  is_extended_id=False))
            bus.send(can.Message(arbitration_id=TPDO2_ID, data=i32_to_le_bytes(motor.velocity()),
                                  is_extended_id=False))
            next_slow += 0.010

        if now >= next_fast:
            bus.send(can.Message(arbitration_id=TPDO3_ID, data=i16_to_le_bytes(motor.current()),
                                  is_extended_id=False))
            next_fast += 0.002

        time.sleep(0.001)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iface", default="vcan0", help="SocketCAN interface (default: vcan0)")
    args = parser.parse_args()

    bus = can.Bus(channel=args.iface, interface="socketcan")
    motor = MotorModel()
    stop = threading.Event()

    sdo_thread = threading.Thread(target=sdo_responder, args=(bus, stop), daemon=True)
    pdo_thread = threading.Thread(target=pdo_sender, args=(bus, motor, stop), daemon=True)
    sdo_thread.start()
    pdo_thread.start()

    print(f"simulator running on {args.iface}, node id {NODE_ID}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        stop.set()
        sdo_thread.join()
        pdo_thread.join()
        bus.shutdown()


if __name__ == "__main__":
    main()