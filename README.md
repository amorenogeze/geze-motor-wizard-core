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
its own. The motor controller is a **SOLO PICO**; everything specific to it
lives in a JSON device profile (`device-gateway/canopen/devices/solopico.json`).
A Python simulated SOLO PICO (`canopen-sim`) stands in for the real
hardware during development, so the whole stack can be exercised without a
physical motor.

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
│   └── canopen/            # SDO/PDO over SocketCAN (CanopenClient, CanopenTranslator, DeviceProfile)
│       └── devices/          # device profiles: solopico.json
├── canopen-sim/          # simulated SOLO PICO (Python + python-can), dev/test only
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
`CanopenTranslator` has no object indexes of its own: it executes the
device profile (`canopen/device_profile.h`/`.cpp`, see *Device profiles*
below).

**`canopen-sim/`** — a Python simulated SOLO PICO (`python-can`), node
ID 1: answers SDOs the way the PICO does (reads `0x42`, writes `0x22`/`0x60`,
Q15.17 floats), sends synchronous TPDOs when it receives SYNC, and runs a
toy brushed motor. Starts in the PICO's factory state (BLDC, analogue) so
the gateway's configuration step is exercised. Requires a `vcan0`
interface; never installed on production images with real CAN hardware.

**`ui/`** — `ui.py`, a `curses`-based terminal UI with three screens
(Commands, Telemetry, Device Info), included as a working reference
implementation of the wire protocol — not the final production UI. 

**`tests/`** — GoogleTest unit tests for `shared/` (message
encode/decode round-trips, socket framing, `ThreadSafeQueue` behavior),
device profile parsing, and `CanopenClient`/`CanopenTranslator` against a
fake SOLO PICO over a socketpair (no vcan needed). Native builds only;
skipped automatically when cross-compiling.

### Usage

**Requirements**
- CMake ≥ 3.16, a C++17 compiler
- SQLite3, nlohmann_json ≥ 3.2 (downloaded by CMake if not installed;
  on Yocto add `nlohmann-json` to `DEPENDS`)
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

# Terminal 3 — CANopen client  [iface] [device profile] [command set]
./build/device-gateway vcan0

# Terminal 4 — UI (only once you want to interact)
python3 ui/ui.py
# or, for torque / speed / position commands (SetMotionCommand 0x43):
python3 ui/motion_cli.py          # t 2000 | s 1500 ccw | p 4000 | stop | home | help
```
Without hardware or vcan, `python3 ui/fake_engine.py` stands in for the engine,
gateway and PICO so `ui/motion_cli.py` can be tried on its own.
The simulator starts in **STOP** — no telemetry flows until you send
`run` from the UI's Commands screen (the gateway only sends SYNC while
running).

### Device profiles

`device-gateway` loads one JSON profile at startup and exits with a list of
every problem if it is invalid. Lookup order: `argv[2]`,
`$WIZARD_DEVICE_PROFILE`, `/etc/wizard/devices/solopico.json` (installed),
then `devices/solopico.json` next to the executable (copied there by the
build). The profile defines:

| Section | Meaning |
|---|---|
| `node_id`, `sdo`, `sync_period_ms` | CANopen node, SDO style/timeout, SYNC period |
| `objects` | name → `{index, sub, type}`; `type` is `u8/u16/u32/i16/i32/q17` (`q17` = SOLO float, value × 131072) |
| `configure` | SDO writes applied when the node first answers, and again after it comes back from a power cycle; `"verify": true` reads back, `"skip": true` keeps a step without running it |
| `status`, `alive_object` | object read for run/stop status and by the 500 ms heartbeat |
| `device_info` | each field from an object or a fixed value |
| `telemetry` | `tpdo` (COB-ID + type) or `sdo_poll` (object + period); `scale` converts to the integer sent on the socket (e.g. A → mA) |

Keys starting with `_` are comments; any other unknown key is an error.

### Command set

What the commands do lives in a second file, `commands/solopico_commands.json`,
which names objects only (addresses stay in the device profile). Lookup order:
`argv[3]`, `$WIZARD_COMMANDS`, `/etc/wizard/commands/solopico_commands.json`
(installed), then `commands/solopico_commands.json` next to the executable.
It is checked against the device profile at startup.

| Section | Meaning |
|---|---|
| `device` | name of the device profile it belongs to (must match) |
| `direction` | object + values written for CW / CCW |
| `enable` | steps that start the drive |
| `stop` | STOP; every step is attempted even if one fails; a failed start also runs it |
| `home` | position becomes 0 (optional) |
| `modes` | `torque` / `speed` / `position`: `enter` steps, `reference` object, `scale` (socket unit → object unit), `min`/`max` (clamped), `ramp_rate`, `uses_direction`, `reference_first` |
| `legacy_run` | what `SetRunStopCommand` RUN means: mode + setpoint + direction |

How the gateway handles a command (`CommandManager`): a mode while stopped
starts it (enter steps, direction, enable, reference); the same mode and
direction while running only moves the reference (live update, ramped);
another mode or direction stops first, then starts; STOP runs `stop`; HOME runs
`home` and is refused while running. Setpoints are clamped to `min`/`max`.
Every command is answered with `RunStopStatusEvent` (0x41).


### Debugging

`wizard-engine` logs connection events (listen/connect/disconnect) and
errors unconditionally — that output is always on, in both native runs
and via `journalctl -u wizard-engine` on the board.

Per-message telemetry logging (`PositionEvent`/`VelocityEvent`/`CurrentEvent`
— up to ~500 lines/sec combined while RUNNING) is **off by default** to
keep normal output readable, but can be turned on when actually needed
via the `WIZARD_VERBOSE` environment variable. It's read once at
process startup, so toggling it requires a restart, not a live signal.

**Native / manual run:**
```bash
WIZARD_VERBOSE=1 ./build/wizard-engine
```

**On the board, via a systemd override** (keeps the default service
definition quiet; this is an opt-in, temporary override):
```bash
sudo systemctl edit wizard-engine
```
Add:
```ini
[Service]
Environment=WIZARD_VERBOSE=1
```
Then:
```bash
sudo systemctl daemon-reload
sudo systemctl restart wizard-engine
```

To turn it back off:
```bash
sudo systemctl revert wizard-engine
sudo systemctl restart wizard-engine
```
