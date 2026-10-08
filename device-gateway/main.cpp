// device-gateway: the only process that talks to the motor controller.
//
//   device-gateway [iface] [device profile] [command set]
//
// Loads the device profile (what the device is) and the command set (what the
// UI commands do), opens the CAN interface once, then serves wizard-engine,
// reconnecting forever: an engine restart never reopens CAN.
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "canopen/canopen_translator.h"
#include "canopen/device_profile.h"
#include "commands/commands.h"
#include "engine_session.h"

using namespace wizard;

namespace {

constexpr const char* kEngineSocketPath = "/tmp/wizard-backend.sock";
constexpr const char* kDefaultCanInterface = "vcan0";

#ifndef WIZARD_DEFAULT_DEVICE_PROFILE
#define WIZARD_DEFAULT_DEVICE_PROFILE "/etc/wizard/devices/solopico.json"
#endif
#ifndef WIZARD_DEFAULT_COMMANDS
#define WIZARD_DEFAULT_COMMANDS "/etc/wizard/commands/solopico_commands.json"
#endif

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Directory of the running executable, e.g. /home/me/wizard/build/device-gateway.
std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    const std::string path(buf, static_cast<size_t>(n));
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

// Lookup of a config file, first match wins:
//   1. argv[arg]          explicit
//   2. $env               systemd override
//   3. installed          /etc/wizard/...
//   4. <exe dir>/relative native build tree (CMake copies the files there)
std::string resolve(int argc, char** argv, int arg, const char* env, const char* installed,
                    const std::string& relative) {
    if (argc > arg) return argv[arg];
    if (const char* e = std::getenv(env)) return e;
    if (file_exists(installed)) return installed;
    return exe_dir() + "/" + relative;
}

// SIGINT/SIGTERM (Ctrl+C, systemctl stop): stop the motor, then exit.
// The signals are blocked in every thread and received here with sigwait,
// so the SDO writes run in a normal thread, not inside a signal handler.
void signal_loop(sigset_t signals, DeviceTranslator* translator) {
    int sig = 0;
    if (sigwait(&signals, &sig) != 0) return;
    std::cout << "signal " << sig << " received, stopping the motor\n";
    translator->request_stop();  // a ramp in progress gives way
    if (!translator->stop()) std::cerr << "stop on shutdown failed\n";
    std::cout.flush();
    std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    // Block the shutdown signals before any thread exists so every thread
    // inherits the mask and only signal_loop receives them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    const std::string can_iface = argc > 1 ? argv[1] : kDefaultCanInterface;
    const std::string profile_path =
        resolve(argc, argv, 2, "WIZARD_DEVICE_PROFILE", WIZARD_DEFAULT_DEVICE_PROFILE, "devices/solopico.json");
    const std::string commands_path =
        resolve(argc, argv, 3, "WIZARD_COMMANDS", WIZARD_DEFAULT_COMMANDS, "commands/solopico_commands.json");

    DeviceProfile profile;
    CommandSet commands;
    try {
        std::cout << "loading device profile " << profile_path << "\n";
        profile = load_device_profile(profile_path);
        std::cout << "loading command set " << commands_path << "\n";
        commands = load_command_set(commands_path, profile);
    } catch (const ProfileError& e) {
        // Nothing sensible can run without valid files; exit so systemd shows
        // the reason instead of a gateway that silently does nothing.
        std::cerr << e.what() << "\n";
        return 1;
    }

    std::cout << "opening CAN interface " << can_iface << "\n";
    std::unique_ptr<DeviceTranslator> translator =
        std::make_unique<CanopenTranslator>(can_iface, profile, commands.stop);
    std::cout << "CAN interface ready\n";

    std::thread(signal_loop, signals, translator.get()).detach();

    while (true) run_engine_session(*translator, commands, kEngineSocketPath);
}
