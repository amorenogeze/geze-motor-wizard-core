# geze-motor-wizard-core

#### Table of Contents
- [Description](#description)
- [Architecture](#architecture)
- [Usage](#usage)

### Description

Motor tuning wizard-core(ENGINE+GATEWAY) for the DH Electronics DHSOM (STM32MP13,
DHCOR/DHSBC carrier board). Version 1: CANopen telemetry, run/stop
commands, and Device Info, driven through a terminal UI.

The core is split into two small C++ processes connected by Unix
sockets — `device-gateway`, which is the only component that speaks
CANopen/Modbus, and `wizard-engine`, a pure relay with no protocol logic of
its own. A Python-based simulated CANopen node (`canopen-sim`) stands
in for real MCU hardware during development, so the whole stack can be
exercised without a physical motor.

### Actual Architecture

```
[canopen-sim]  --CAN/vcan-->  [device-gateway]  --Unix socket-->  [wizard-engine]  --Unix socket-->  [ui.py]
 (dev only,                    CANopen client                     pure relay,                        curses UI
  no real HW                   (SocketCAN)                        no logic
  needed)
```

```
.
├── shared/               # message framing, Unix socket transport, ThreadSafeQueue
├── wizard-engine/        # relay server: UI socket + gateway socket
├── device-gateway/       # CANopen client, DeviceTranslator abstraction
│   └── canopen/            # SDO/PDO over SocketCAN (CanopenClient, CanopenTranslator)
├── canopen-sim/          # simulated CANopen node (Python + python-can), dev/test only
├── ui/                   # reference terminal UI (curses)
├── tests/                # GoogleTest unit tests (shared/ + ThreadSafeQueue)
├── docs/                 # spec, interactive protocol reference, flashing guide, TODO
└── CMakeLists.txt
```

**`shared/`** — everything reused by both C++ processes: the socket
framing (`message.h`/`.cpp`, `[type][length][payload]` + typed
encode/decode per message), the Unix socket wrapper
(`unix_socket.h`/`.cpp`), and `thread_safe_queue.h`, a generic blocking
queue that keeps every mutex/condition_variable in one small, tested
file instead of scattered across callers.

**`wizard-engine/`**  Listens on `/tmp/wizard-backend.sock`
(for `device-gateway`) and `/tmp/wizard-ui.sock` (for the UI), forwards
messages between the two, and reconnects either side independently if
it drops — no shared state, no protocol interpretation, no inference.

**`device-gateway/`** — the only component that talks CANopen/Modbus. Depends
on the `DeviceTranslator` abstract interface (`device_translator.h`),
not directly on any protocol client, so a future `ModbusTranslator`
could be added without touching `main.cpp`. Its current implementation,
`canopen/canopen_translator.h`/`.cpp` (`CanopenTranslator`), wraps
`canopen/canopen_client.h`/`.cpp` (`CanopenClient`), which owns a
single background reader thread over SocketCAN — SDO responses and TPDO
telemetry are dispatched into two `ThreadSafeQueue`s, avoiding a race
between the command path and the telemetry path on the same socket fd.

**`canopen-sim/`** — a Python CANopen node (`python-can`) simulating
node ID 1: responds to SDO reads/writes (Identity Object, plus the
manufacturer-specific `Control`/`Status` run/stop objects) and emits
TPDO telemetry while running. Requires a `vcan0` interface; never
installed on production images with real CAN hardware.

**`ui/`** — `ui.py`, a `curses`-based terminal UI with three screens
(Commands, Telemetry, Device Info), included as a working reference
implementation of the wire protocol — not the final production UI. 

**`tests/`** — GoogleTest unit tests for `shared/` (message
encode/decode round-trips, socket framing, `ThreadSafeQueue` behavior).
Native builds only; skipped automatically when cross-compiling.

**`docs/`** — start at `docs/index.html`. Includes the full spec
(`v1-spec.md`/`spec.html`), the interactive protocol reference
(`protocol.html`), the DHSBC eMMC flashing guide (`flashing-guide.html`),
and the current TODO/roadmap (`todo.html`).

### Usage

**Requirements**
- CMake ≥ 3.16, a C++17 compiler
- Linux with SocketCAN (`vcan` for development without real hardware)
- Python 3 + `python-can` (`pip install -r canopen-sim/requirements.txt`)

**Build (native)**
```bash
mkdir build && cd build
cmake ..
cmake --build . -j4
```
Builds `wizard-engine`, `device-gateway`, and (native builds only, not
when cross-compiling) `wizard_tests`.

**Tests**
```bash
cd build
./wizard_tests
```

**Set up a virtual CAN interface** (development, no real hardware)
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

**Running the full stack locally** — four processes, in this order:
```bash
# Terminal 1 — simulated MCU
pip install -r canopen-sim/requirements.txt
python3 canopen-sim/simulator.py

# Terminal 2 — relay
./build/wizard-engine

# Terminal 3 — CANopen client
./build/device-gateway vcan0

# Terminal 4 — UI (only once you want to interact)
python3 ui/ui.py
```
The simulator starts in **STOP** — no telemetry flows until you send
`run` from the UI's Commands screen.

**Cross-compiling for the DHSOM target** is done via `product-bsp`'s
`kas`/Yocto build (separate repo), which fetches this repo as a recipe
source — not part of this repository's own build. See
`docs/flashing-guide.html` for writing a built image to the DHSBC's
eMMC.
