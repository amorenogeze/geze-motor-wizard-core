# geze-motor-wizard-core: project structure (context file)

> Paste this file at the start of a new conversation to restore context.
> Keep it up to date when the structure changes. Last updated: 2026-09-25.

## 1. What this is
A motor tuning wizard ("door tuning wizard") running on a **DH Electronics DHSOM
(STM32MP13, Linux, DHCOR/DHSBC carrier)**. V1 features: CANopen telemetry
(position / speed / current), RUN/STOP command, Device Info, MCU alive status,
telemetry stored in SQLite, terminal UI.

- Motor controller: **SOLO PICO** (brushed DC motor), reached over **CANopen**, node 1.
  Everything PICO-specific is in `device-gateway/canopen/devices/solopico.json` (device profile).
- Without hardware: `canopen-sim/simulator.py` emulates the PICO on `vcan0`.
- Language: C++17 (CMake ≥ 3.16), Python 3 for sim + UI. Namespace `wizard`.
- Side experiment (separate from this repo): NUCLEO-F302R8 + X-NUCLEO-IHM08M1 driving a brushed motor with TIM1 PWM.

## 2. Processes and data flow
```
[SOLO PICO | canopen-sim] --CAN (SocketCAN can0/vcan0)--> [device-gateway]
        --/tmp/wizard-backend.sock--> [wizard-engine] --/tmp/wizard-ui.sock--> [ui/ui.py]
                                            |
                                            +--> SQLite /tmp/door_tuning_wizard.db
```
| Process | Role | Owns |
|---|---|---|
| `device-gateway` | Only component that speaks CANopen (Modbus planned). Translates CAN <-> socket messages. Client of the engine socket; reconnects forever. | CAN socket (kept open across engine restarts) |
| `wizard-engine` | Listens for gateway and UI (one client each), forwards commands UI->gateway and replies gateway->UI, writes telemetry to DB while a run is open. | Both listening sockets, DB connection |
| `ui/ui.py` | Reference curses UI: Commands (RUN/STOP), Telemetry (sparklines), Device Info. Not the final UI. | - |
| `canopen-sim` | Dev-only simulated SOLO PICO, node ID 1 (SDO 0x42/0x22, SYNC TPDOs, toy motor). | - |

Deployed as systemd services (`wizard-engine`, gateway). Engine logging: always
connection events/errors; per-telemetry logs only with `WIZARD_VERBOSE=1`.

## 3. Directory tree (source only)
```
.
├── CMakeLists.txt            # lib `shared` + subdirs; gtest via FetchContent (native only)
├── README.md  TODO.md  structure.md
├── shared/                   # static lib `shared` used by both C++ processes
│   ├── message.h/.cpp        # frame format + typed make_/parse_ per message
│   ├── unix_socket.h/.cpp    # UnixSocket (send/receive), listen_and_accept, connect_to
│   ├── thread_safe_queue.h   # ThreadSafeQueue<T>: push/pop/pop_for/clear/close
│   └── db.h/.cpp             # Database (SQLite), socket + db path constants
├── wizard-engine/
│   ├── CMakeLists.txt
│   └── main.cpp              # relay + DB writer
├── device-gateway/
│   ├── CMakeLists.txt        # libs `canopen` (addresses) and `gateway` (commands + session, links canopen) + exe;
│   │                         # copies devices/ and the commands JSON next to the exe; installs to /etc/wizard/{devices,commands}
│   ├── main.cpp              # file lookup (argv -> env -> /etc/wizard -> exe dir), load, open CAN, signals, reconnect loop
│   ├── engine_session.h/.cpp # EngineSession: one wizard-engine connection, 4 threads (telemetry / reader / worker / heartbeat)
│   ├── device_translator.h   # abstract DeviceTranslator (start/execute/stop/request_stop/is_running) + WriteStep, TelemetrySample, DeviceInfo
│   ├── commands/
│   │   ├── commands.h/.cpp   # CommandSet (load + check JSON, start / live-update steps, 0x40 -> 0x43) + CommandManager
│   │   └── solopico_commands.json  # what each command does (object names only)
│   └── canopen/
│       ├── canopen_client.h/.cpp      # SocketCAN (or adopted fd), generic expedited SDO read/write, SYNC, TPDO routing by COB-ID, reader thread
│       ├── device_profile.h/.cpp      # DeviceProfile: parse+validate JSON (nlohmann_json), ValueType encode/decode (q17 = x*131072)
│       ├── canopen_translator.h/.cpp  # CanopenTranslator : DeviceTranslator, runs steps by object name; own stop steps; pdo/sync/poll threads
│       ├── profile_json.h             # JSON checking helpers shared by the profile and command set parsers
│       └── devices/solopico.json      # SOLO PICO profile
├── canopen-sim/simulator.py  # simulated SOLO PICO (python-can)
├── ui/ui.py                  # curses UI
├── ui/motion_cli.py          # line-based test UI for SetMotionCommand (t/s/p/stop/home)
├── ui/fake_engine.py         # fake engine + gateway + PICO for motion_cli.py without hardware
└── tests/                    # GoogleTest: message, unix_socket, thread_safe_queue, device_profile, commands,
                              # canopen_translator; fake PICO over a socketpair in tests/fake_pico.h
```
`docs/` (v1-spec.md etc.) is referenced by code comments/README but is not in the repo.

## 4. Socket wire protocol (`shared/message.h`)
Frame: `[type u8][length u16 LE][payload]`. All integers little-endian. `ts` = u64 µs (gettimeofday).

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 / 0x02 | Ping / Pong | - | empty (defined, unused) |
| 0x21 | PositionEvent | gw -> engine | ts u64, position i32 (12 B) |
| 0x22 | SpeedEvent | gw -> engine | ts u64, speed i32 (12 B) |
| 0x23 | CurrentEvent | gw -> engine | ts u64, current **i16** (10 B) |
| 0x30 | DeviceInfoRequest | UI/engine -> gw | empty |
| 0x31 | DeviceInfoResponse | gw -> engine -> UI | vendor, product, revision, serial: u32 each (16 B) |
| 0x40 | SetRunStopCommand | UI -> engine -> gw | u8 run (1 B) |
| 0x41 | RunStopStatusEvent | gw -> engine -> UI | ts u64, u8 running (9 B); answers 0x40 and 0x43, always |
| 0x43 | SetMotionCommand | UI -> engine -> gw | u8 mode (0 STOP, 1 TORQUE, 2 SPEED, 3 POSITION, 4 HOME), i32 setpoint (mA / rpm / counts), u8 direction (0 CW, 1 CCW) (6 B) |
| 0x51 | McuStatusEvent | gw -> engine -> UI | u8 alive (1 B), sent only on change |

`ui/ui.py` duplicates these constants (it calls 0x22 `MSG_VELOCITY_EVENT`); keep both in sync.

## 5. CANopen mapping (from `devices/solopico.json`)
Node 1. SDO timeout 500 ms, one SDO in flight (mutex). SOLO style: read cs 0x40 -> reply 0x42
(no size), write cs 0x22 -> reply 0x60. All values 32-bit LE; floats `q17` (value * 131072).
Object indexes from SOLO's official library (`SOLOMotorControllersCanopen.h`).

| Name in profile | Object | Type | Role |
|---|---|---|---|
| command_mode | 0x3002 | u32 | 1 = digital (configure, verified) |
| current_limit | 0x3003 | q17 A | 2.0 (configure, verified) |
| torque_reference | 0x3004 | q17 A | run 0.5 / stop 0 |
| speed_reference | 0x3005 | i32 rpm | 0 at configure |
| motor_identification | 0x3007 | u32 | step present but `skip` |
| drive_enable | 0x3008 | u32 | run 1 / stop 0; also the **status** object |
| pwm_frequency_khz | 0x3009 | u32 | 20 |
| motor_direction | 0x300C | u32 | run 1 (CW) |
| speed_limit | 0x3011 | i32 | 4000 |
| feedback_mode | 0x3013 | u32 | 0 sensorless |
| motor_type | 0x3015 | u32 | 0 DC (factory is 1 BLDC) |
| control_mode | 0x3016 | u32 | 1 torque |
| dc_motor_current | 0x3032 | q17 A | telemetry current, **SDO-polled** every 20 ms, x1000 -> mA |
| firmware / hardware version | 0x303A / 0x303B | u32 | alive probe; device_info revision / product_code |
| TPDO params | 0x1814..0x1819 sub1 = COB-ID\|0x80000000 enable, sub2 = every N SYNC | u32 | 0x1814 position -> 0x281, 0x1815 speed -> 0x282 (0x1816 Iq -> 0x283, skipped) |

SYNC (0x080) every 10 ms, **only while RUNNING** (`telemetry_only_while_running`), so no telemetry when stopped.
Unverified on real hardware: TPDO enable bit meaning, whether reads need a special subindex
(`sdo.read_subindex`), 0x1018 identity support.

## 6. Threads
**device-gateway** translator threads (process lifetime): `pdo_thread_` (TPDO -> TelemetrySample),
`sync_thread_` (SYNC while running), `poll_thread_` (sdo_poll channels). `configure` runs on the first
successful heartbeat probe and again after the node disappears and returns; `start` / `execute`
configure first if needed; a failed `start` runs the stop steps.

**device-gateway** per engine session (`EngineSession`, engine_session.cpp):
- caller's thread `telemetry_loop`: `wait_next_telemetry()` -> Position/Speed/CurrentEvent.
- `reader_loop`: only reader of the engine socket; answers DeviceInfoRequest itself, queues 0x40 / 0x43 for the worker. A STOP also calls `request_stop()` at once, so a ramp in progress gives way instead of finishing first.
- `worker_loop`: carries out the queued commands one at a time through `CommandManager`, then replies RunStopStatusEvent (read-back of the status object, or the gateway's own state if that read fails).
- `heartbeat_loop`: every 500 ms `probe_alive()`; sends McuStatusEvent on change.
- When one of them sees the engine gone, the session ends, queued commands are dropped and the motor is stopped.
- `send_mutex_` serializes writes to the engine socket; `active_` ends all four threads.
- Plus `CanopenClient::reader_thread_` (lifetime of process): routes SDO responses and TPDOs into two queues.

**wizard-engine**: main thread `gateway_accept_loop` -> `gateway_loop`; `ui_command_loop` thread.
Engine sends DeviceInfoRequest to the gateway right after it connects.

## 7. Database (`shared/db.*`)
- Path default `/tmp/door_tuning_wizard.db` (argv[1] overrides). **Owned/provisioned by the UI backend**: engine opens READWRITE without CREATE, never creates tables.
- Tables used: `Data_Type(id, name)` with names `position`, `speed`, `current`; `Runs(Id, End_Time)`; `Data(Run_Id, Data_Type_Id, Value, Timestamp ISO-8601 UTC seconds)`.
- Engine idles (5 s retry) until tables + the 3 Data_Type rows exist.
- Run tracking: on RunStopStatusEvent running=1, attach to latest `Runs` row with `End_Time IS NULL`; telemetry is inserted only while attached; running=0 detaches.

## 8. Build / run
```bash
mkdir build && cd build && cmake .. && cmake --build . -j4 && ./wizard_tests
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
python3 canopen-sim/simulator.py        # 1
./build/wizard-engine [db_path]         # 2
./build/device-gateway [vcan0|can0]     # 3
python3 ui/ui.py                        # 4
```
`device-gateway [iface] [profile] [commands]`; profile lookup: argv[2] -> $WIZARD_DEVICE_PROFILE ->
/etc/wizard/devices/solopico.json -> <exe dir>/devices/solopico.json. Command set lookup: argv[3] ->
$WIZARD_COMMANDS -> /etc/wizard/commands/solopico_commands.json -> <exe dir>/commands/solopico_commands.json.
Deps: Threads, SQLite3, nlohmann_json (find_package, else FetchContent; Yocto: DEPENDS nlohmann-json),
python-can. Tests skipped when cross-compiling (48 tests, all passing).

## 9. Status (TODO.md)
- Engine: sockets done. Open: CONFIGURATOR (compare UI config vs real MCU), ErrorLog, EventLog.
- Gateway: SOLO PICO profile + simulator done (branch feature/solo-pico). Open: test on real PICO;
  decide telemetry units with UI/DB owner.
- Planned engine modules (discussed): DataManager, Controller, telemetry, MovementMonitor
  (blockade, deviation, non-safety blockades). Decision so far: keep them as modules in
  one engine binary; real safety stops belong in hardware/PICO (current limit, STO), not Linux.

## 10. Observations to keep in mind
- Engine forwards only command replies to the UI (`is_command_reply`), **not telemetry**, so the UI Telemetry screen receives nothing through the engine today.
- README says the engine is a "pure relay, no state", but it keeps run state and writes the DB.
- Telemetry units changed with the PICO: current in **mA** (CurrentEvent i16), speed rpm, position encoder
  counts (meaningless without encoder). DB `Data_Type` limits / UI graphs were tuned for the old sim (4-16 A, 10-90 %).
- No segmented SDO (only expedited, max 4 bytes).
- `canopen-sim/requirements.txt` is referenced by README but missing from the repo.
