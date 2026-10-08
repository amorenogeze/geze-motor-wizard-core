// CanopenClient + CanopenTranslator against a fake SOLO PICO (tests/fake_pico.h).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "fake_pico.h"

using namespace wizard;
using namespace wizard::test;
using namespace std::chrono_literals;

TEST(CanopenClient, AcceptsSoloAndStandardUploadResponses) {
    FakePico pico;
    CanopenClient client(pico.gateway_fd, 1);
    auto r = client.sdo_read(0x303A, 0);  // PICO style 0x42
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value, 0x0000B020u);

    pico.size_indicated_replies = true;  // standard 0x43
    EXPECT_EQ(client.sdo_read(0x303B, 0).value, 0x123u);

    auto missing = client.sdo_read(0x2000, 0);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.abort_code, 0x06020000u);
}

TEST(CanopenClient, WriteCommandSpecifiers) {
    FakePico pico;
    CanopenClient client(pico.gateway_fd, 1);
    EXPECT_TRUE(client.sdo_write(0x3008, 0, 1, 4, false).ok);
    EXPECT_TRUE(client.sdo_write(0x3009, 0, 20, 1, true).ok);
    auto w = pico.writes();
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[0].cs, 0x22);  // SOLO: size not indicated
    EXPECT_EQ(w[1].cs, 0x2F);  // standard 1-byte
}

TEST(CanopenTranslator, ConfigureAppliesProfileInOrder) {
    FakePico pico;
    Config s = pico_setup();
    auto t = make_translator(pico, s);
    ASSERT_TRUE(t->configure());

    auto w = pico.writes();
    ASSERT_FALSE(w.empty());
    EXPECT_EQ(w.front().index, 0x3002);  // command_mode first
    for (const auto& x : w) EXPECT_EQ(x.cs, 0x22);
    EXPECT_EQ(pico.get(0x3015), 0u);                                        // motor type DC
    EXPECT_EQ(pico.get(0x3003),                                             // current limit from profile
              encode_value(ValueType::Q17, find_step(s.p.configure, "current_limit").value));
    EXPECT_EQ(pico.get(0x3016), 1u);                                        // torque mode
    EXPECT_EQ(pico.get(0x1814, 1), 0x80000281u);                            // position TPDO
    EXPECT_EQ(pico.get(0x1815, 2), 1u);                                     // speed TPDO every SYNC
    EXPECT_FALSE(find_write(w, 0x3007));                                    // identification skipped
    EXPECT_TRUE(t->is_configured());
}

TEST(CanopenTranslator, VerifyMismatchStopsConfigure) {
    FakePico pico;
    pico.set(0x3015, 0, 1);            // PICO still BLDC...
    pico.ignore_writes.insert(0x3015);  // ...and silently keeps it
    auto t = make_translator(pico);
    EXPECT_FALSE(t->configure());
    EXPECT_FALSE(t->is_configured());
    EXPECT_FALSE(find_write(pico.writes(), 0x3016));  // later steps not executed
}

TEST(CanopenTranslator, RunStopAndStatus) {
    FakePico pico;
    Config s = pico_setup();
    auto t = make_translator(pico, s);
    ASSERT_TRUE(t->run_stop(true));  // legacy RUN; configures on first use
    EXPECT_EQ(pico.get(0x3008), 1u);
    EXPECT_EQ(pico.get(0x3016), 1u);     // torque mode entered
    EXPECT_EQ(pico.get(0x3004), encode_value(ValueType::Q17, s.run_torque()));
    EXPECT_EQ(t->read_run_stop_status().value_or(false), true);

    ASSERT_TRUE(t->run_stop(false));
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
    EXPECT_EQ(t->read_run_stop_status().value_or(true), false);
}

TEST(CanopenTranslator, FailedRunFallsBackToStop) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());
    pico.reject_writes.insert(0x300C);  // motor_direction rejected mid-run
    EXPECT_FALSE(t->run_stop(true));
    EXPECT_FALSE(t->is_running());
    EXPECT_EQ(pico.get(0x3008), 0u);  // drive disabled again by the stop sequence
}

TEST(CanopenTranslator, TorqueRampsUpAtRateOnRun) {
    FakePico pico;
    Config s = ramp_setup();                                    // 0 -> 2.0 A at the torque mode's ramp_rate
    const double rate = s.torque().ramp_rate;
    ASSERT_GT(rate, 0.0);
    auto t = make_translator(pico, s);
    ASSERT_TRUE(t->configure());

    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->run_stop(true));
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double expected = kTestTorque / rate;                // 2.0 A / 0.5 A/s = 4 s
    EXPECT_GE(took, expected * 0.85);
    EXPECT_LT(took, expected * 1.5);
    auto up = q17_writes(pico, 0x3004);
    ASSERT_GT(up.size(), static_cast<size_t>(expected / 0.02 / 2));  // at least half of the 20 ms steps
    for (size_t i = 1; i < up.size(); ++i) EXPECT_GE(up[i], up[i - 1]);
    for (size_t i = 1; i < up.size(); ++i) EXPECT_LE(up[i] - up[i - 1], rate * 0.04 + 1e-6);  // <= 2 steps, no jumps
    EXPECT_DOUBLE_EQ(up.back(), kTestTorque);
    t->run_stop(false);
}

TEST(CanopenTranslator, RampDownStartsFromMeasuredCurrent) {
    FakePico pico;  // dc_motor_current (0x3032) reads 0.75 A
    auto t = make_translator(pico, shipped());
    ASSERT_TRUE(t->configure());
    pico.set(0x3004, 0, encode_value(ValueType::Q17, 2.0));  // reference 2.0 A, only 0.75 A flows
    pico.set(0x3008, 0, 1);

    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->run_stop(false));
    const auto took = std::chrono::steady_clock::now() - t0;
    auto down = q17_writes(pico, 0x3004);
    ASSERT_GT(down.size(), 20u);
    EXPECT_LE(down.front(), 0.75);                             // starts at the measured 0.75 A, not 2.0
    EXPECT_GT(down.front(), 0.70);
    for (size_t i = 1; i < down.size(); ++i) EXPECT_LE(down[i], down[i - 1]);
    EXPECT_DOUBLE_EQ(down.back(), 0.0);
    EXPECT_GE(took, 1200ms);                                   // 0.75 A at 0.5 A/s = 1.5 s
    EXPECT_LT(took, 3s);
    EXPECT_EQ(pico.get(0x3008), 0u);
}

TEST(CanopenTranslator, RampKeepsItsDurationWithASlowNode) {
    FakePico pico;
    Config s = pico_setup();
    s.cs.legacy_run.setpoint = static_cast<int32_t>(kTestTorque / s.torque().scale);
    s.torque().ramp_rate = kTestTorque / 1.0;                  // 0 -> 2.0 A in 1 s
    auto t = make_translator(pico, s);
    ASSERT_TRUE(t->configure());
    pico.reply_delay_ms = 60;                                  // every SDO answer takes 60 ms
    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->run_stop(true));
    const auto took = std::chrono::steady_clock::now() - t0;
    // run = control_mode + verify + direction + drive_enable + ramp (1 s) + final write: ~1.3 s.
    // Step-counted ramp: 50 steps x (20 + 60) ms = 4 s for the ramp alone.
    EXPECT_LT(took, 1800ms);
    EXPECT_GE(took, 1000ms);
    auto up = q17_writes(pico, 0x3004);
    EXPECT_GT(up.size(), 5u);                                  // still a ramp, just coarser
    EXPECT_DOUBLE_EQ(up.back(), kTestTorque);
    pico.reply_delay_ms = 0;
    t->run_stop(false);
}

TEST(CanopenTranslator, FixedTimeRampStillSupported) {
    // ramp_ms (fixed duration) instead of ramp_rate, here on the stop sequence.
    FakePico pico;
    Config s = pico_setup();
    find_step(s.cs.stop, "torque_reference").ramp_ms = 400;
    auto t = make_translator(pico, s);
    ASSERT_TRUE(t->configure());
    pico.set(0x3004, 0, encode_value(ValueType::Q17, kTestTorque));
    pico.set(0x3008, 0, 1);
    pico.clear_writes();
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(t->run_stop(false));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 300ms);
    EXPECT_GT(q17_writes(pico, 0x3004).size(), 10u);
    EXPECT_DOUBLE_EQ(q17_writes(pico, 0x3004).back(), 0.0);
    EXPECT_EQ(pico.get(0x3008), 0u);
}

TEST(CanopenTranslator, StopInterruptsRampUp) {
    FakePico pico;
    auto t = make_translator(pico, ramp_setup());
    ASSERT_TRUE(t->configure());

    std::atomic<bool> run_result{true};
    std::thread runner([&] { run_result = t->run_stop(true); });
    std::this_thread::sleep_for(500ms);                        // ramp-up in progress (~0.25 A)
    const auto t0 = std::chrono::steady_clock::now();
    t->run_stop(false);
    runner.join();

    EXPECT_FALSE(run_result);                                  // RUN gave way
    EXPECT_FALSE(t->is_running());
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
    auto up = q17_writes(pico, 0x3004);
    double peak = 0;
    for (double v : up) peak = std::max(peak, v);
    EXPECT_LT(peak, 1.0);                                      // never reached the 2 A target
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 4s);
}

TEST(CanopenTranslator, RampDownFailureStillDisablesDrive) {
    FakePico pico;
    auto t = make_translator(pico, ramp_setup());
    ASSERT_TRUE(t->run_stop(true));
    pico.reject_writes.insert(0x3004);                         // every torque write fails
    EXPECT_FALSE(t->run_stop(false));
    EXPECT_EQ(pico.get(0x3008), 0u);                           // drive disable still sent
    EXPECT_FALSE(t->is_running());
}

TEST(CanopenTranslator, StopNeverRunsConfigure) {
    FakePico pico;
    auto t = make_translator(pico);
    pico.set(0x3008, 0, 1);  // drive left enabled, gateway never configured
    pico.set(0x3004, 0, encode_value(ValueType::Q17, 0.5));
    pico.reject_writes.insert(0x3015);  // configure would fail here
    ASSERT_FALSE(t->is_configured());

    EXPECT_TRUE(t->run_stop(false));
    EXPECT_FALSE(find_write(pico.writes(), 0x3002));  // no configure step written
    EXPECT_EQ(pico.get(0x3004), 0u);
    EXPECT_EQ(pico.get(0x3008), 0u);
}

TEST(CanopenTranslator, StopContinuesAfterFailedStep) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->run_stop(true));
    pico.reject_writes.insert(0x3004);  // "torque = 0" rejected...

    EXPECT_FALSE(t->run_stop(false));
    EXPECT_EQ(pico.get(0x3008), 0u);  // ...drive is still disabled
    EXPECT_FALSE(t->is_running());
}

TEST(CanopenTranslator, WaitNextTelemetryTimesOutWhileStopped) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());

    TelemetrySample s{};
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(t->wait_next_telemetry(100ms, s), TelemetryWait::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);  // returned, did not block

    ASSERT_TRUE(t->run_stop(true));
    TelemetryWait r = TelemetryWait::Timeout;
    for (int i = 0; i < 20 && r != TelemetryWait::Sample; ++i) r = t->wait_next_telemetry(100ms, s);
    EXPECT_EQ(r, TelemetryWait::Sample);
    ASSERT_TRUE(t->run_stop(false));
}

TEST(CanopenTranslator, TelemetryOnlyWhileRunning) {
    FakePico pico;
    auto t = make_translator(pico);
    ASSERT_TRUE(t->configure());

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(pico.syncs.load(), 0);  // STOPPED: no SYNC on the bus

    ASSERT_TRUE(t->run_stop(true));
    bool pos = false, spd = false, cur = false;
    for (int i = 0; i < 200 && !(pos && spd && cur); ++i) {
        TelemetrySample sample{};
        ASSERT_NE(t->wait_next_telemetry(1s, sample), TelemetryWait::Closed);
        const TelemetrySample* s = &sample;
        if (s->timestamp_us == 0) continue;  // timeout, try again
        if (s->kind == TelemetryKind::Position) { pos = true; EXPECT_EQ(s->value, 1000); }
        if (s->kind == TelemetryKind::Velocity) { spd = true; EXPECT_EQ(s->value, 1500); }
        if (s->kind == TelemetryKind::Current)  { cur = true; EXPECT_EQ(s->value, 750); }  // 0.75 A -> mA
    }
    EXPECT_TRUE(pos && spd && cur);

    ASSERT_TRUE(t->run_stop(false));
    std::this_thread::sleep_for(50ms);
    const int after_stop = pico.syncs.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(pico.syncs.load(), after_stop);
}

TEST(CanopenTranslator, ProbeReconfiguresAfterPowerCycle) {
    FakePico pico;
    auto t = make_translator(pico);
    EXPECT_TRUE(t->probe_alive());  // first contact configures
    EXPECT_TRUE(t->is_configured());

    pico.silent = true;
    EXPECT_FALSE(t->probe_alive());
    EXPECT_FALSE(t->is_configured());

    pico.silent = false;
    pico.clear_writes();
    pico.set(0x3015, 0, 1);  // came back with factory motor type
    EXPECT_TRUE(t->probe_alive());
    EXPECT_TRUE(t->is_configured());
    EXPECT_EQ(pico.get(0x3015), 0u);
    EXPECT_TRUE(find_write(pico.writes(), 0x3002));
}

TEST(CanopenTranslator, DeviceInfoFromProfile) {
    FakePico pico;
    auto t = make_translator(pico);
    auto info = t->read_device_info();
    ASSERT_TRUE(info);
    EXPECT_EQ(info->vendor_id, 0u);
    EXPECT_EQ(info->product_code, 0x123u);
    EXPECT_EQ(info->revision, 0x0000B020u);
    EXPECT_EQ(info->serial, 0u);
}
