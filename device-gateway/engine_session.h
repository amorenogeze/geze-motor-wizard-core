#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "commands/commands.h"
#include "device_translator.h"
#include "message.h"
#include "thread_safe_queue.h"
#include "unix_socket.h"

namespace wizard {

// One connection to wizard-engine, from connect until the engine goes away.
// Four threads share the socket (writes serialized by send_mutex_):
//
//   telemetry   (caller's thread)  device telemetry -> Position/Speed/CurrentEvent
//   reader                          only thread calling receive(); answers
//                                   DeviceInfoRequest, queues 0x40 / 0x43 for the
//                                   worker. A STOP also calls request_stop() at
//                                   once, so a ramp in progress gives way.
//   worker                          carries out the queued commands one at a time
//                                   (CommandManager), then always replies
//                                   RunStopStatusEvent (0x41)
//   heartbeat                       probe_alive() every 500 ms; McuStatusEvent on change
//
// When any of them sees the engine gone, the session ends and the motor is
// stopped: nobody could send STOP any more. The CAN side (translator) is the
// caller's and survives engine restarts.
class EngineSession {
public:
    EngineSession(DeviceTranslator& translator, const CommandSet& commands, UnixSocket& engine_sock);

    // Blocks until the session ends.
    void run();

private:
    void telemetry_loop();
    void reader_loop();
    void worker_loop();
    void heartbeat_loop();

    bool send(const Message& msg);
    void send_device_info();
    void send_run_stop_status();

    DeviceTranslator& translator_;
    const CommandSet& commands_;
    UnixSocket& sock_;
    CommandManager manager_;

    std::mutex send_mutex_;
    std::atomic<bool> active_{true};
    ThreadSafeQueue<SetMotionPayload> queue_;  // reader -> worker
};

// Waits for wizard-engine's socket, connects and runs one EngineSession.
// Returns when the session ends (or the connect fails) so the caller can retry.
void run_engine_session(DeviceTranslator& translator, const CommandSet& commands, const std::string& socket_path);

}  // namespace wizard
