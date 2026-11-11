// SPDX-License-Identifier: GPL-3.0-or-later
#include "g7pro.h"

#include "ds4linux/virtual_device.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

using ds4linux::daemon::BatteryInfo;
using ds4linux::daemon::BatteryStatus;
using ds4linux::daemon::DS4InputState;
using ds4linux::daemon::VirtualDevice;

std::atomic<bool> g_running{true};

void on_signal(int) { g_running = false; }

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

DS4InputState to_ds4(const g7::State& g) {
    DS4InputState s;

    s.lx = g.lx;
    s.ly = g.ly;
    s.rx = g.rx;
    s.ry = g.ry;

    s.hat_x = g.hat_x;
    s.hat_y = g.hat_y;

    // Xbox face layout -> PlayStation face layout.
    s.cross = g.a;
    s.circle = g.b;
    s.square = g.x;
    s.triangle = g.y;

    s.l1 = g.lb;
    s.r1 = g.rb;
    s.l2_btn = g.lt_digital;
    s.r2_btn = g.rt_digital;
    s.l2 = g.lt;
    s.r2 = g.rt;

    s.share = g.view;
    s.options = g.menu;
    s.l3 = g.l3;
    s.r3 = g.r3;
    s.ps = g.guide;

    // The G7 Pro's Share/Capture button and L4 both land on the DS4
    // touchpad click; the other three paddles have no DS4 equivalent.
    s.touchpad = g.share || g.l4;

    s.gyro_x = g.gyro_x;
    s.gyro_y = g.gyro_y;
    s.gyro_z = g.gyro_z;
    s.accel_x = g.accel_x;
    s.accel_y = g.accel_y;
    s.accel_z = g.accel_z;

    s.battery.percent = g.battery;
    if (g.battery < 0) {
        s.battery.status = BatteryStatus::Unknown;
    } else if (g.charging) {
        s.battery.status = BatteryStatus::Charging;
    } else if (g.battery >= 100) {
        s.battery.status = BatteryStatus::Full;
    } else {
        s.battery.status = BatteryStatus::Discharging;
    }

    return s;
}

void usage(const char* argv0) {
    std::printf(
        "usage: %s [options]\n"
        "\n"
        "  --probe           read and print G7 Pro frames; no virtual device\n"
        "  --list            list GameSir USB devices\n"
        "  --rate HZ         virtual DS4 report rate (default 250)\n"
        "  --settle N        heartbeat warm-up frames (default 16)\n"
        "  --mac XX:XX:...   virtual DS4 MAC (default 02:47:37:00:00:01)\n"
        "  --verbose         print connection changes\n"
        "  -h, --help        this text\n"
        "\n"
        "The daemon needs /dev/uhid access (normally root).  The virtual DS4\n"
        "is what SDL3, Eden and ScarletNX see, including gyro and accelerometer.\n",
        argv0);
}

bool parse_mac(const std::string& text, std::array<std::uint8_t, 6>& mac) {
    unsigned v[6];
    if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2],
                    &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (v[i] > 0xff) return false;
        mac[i] = static_cast<std::uint8_t>(v[i]);
    }
    return true;
}

int probe(int settle) {
    g7::Device dev;
    std::string err;
    if (!dev.open(settle, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }

    std::printf("reading G7 Pro input frames (Ctrl+C to stop)\n");
    const std::uint64_t end = now_ms() + 5000;
    while (g_running && now_ms() < end) {
        g7::State s;
        const auto r = dev.read(s, 500, err);
        if (r == g7::ReadResult::Error) {
            std::fprintf(stderr, "read: %s\n", err.c_str());
            return 1;
        }
        if (r != g7::ReadResult::Ok) continue;
        std::printf(
            "sticks=%3u,%3u,%3u,%3u trig=%3u,%3u "
            "A%d B%d X%d Y%d LB%d RB%d View%d Menu%d L3%d R3%d Guide%d Share%d "
            "L4%d R4%d L5%d R5%d dpad=%d,%d "
            "gyro=%6d,%6d,%6d accel=%6d,%6d,%6d bat=%d chg=%d\n",
            s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, s.a, s.b, s.x, s.y, s.lb,
            s.rb, s.view, s.menu, s.l3, s.r3, s.guide, s.share, s.l4, s.r4,
            s.l5, s.r5, s.hat_x, s.hat_y, s.gyro_x, s.gyro_y, s.gyro_z,
            s.accel_x, s.accel_y, s.accel_z, s.battery, s.charging);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int settle = 16;
    int rate = 250;
    bool verbose = false;
    std::array<std::uint8_t, 6> mac{0x02, 0x47, 0x37, 0x00, 0x00, 0x01};

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--list") {
            g7::Device::list();
            return 0;
        }
        if (a == "--probe") {
            std::signal(SIGINT, on_signal);
            std::signal(SIGTERM, on_signal);
            return probe(settle);
        }
        if (a == "--verbose") {
            verbose = true;
            continue;
        }
        if (a == "--settle" && i + 1 < argc) {
            settle = std::atoi(argv[++i]);
            if (settle < 1) settle = 1;
            continue;
        }
        if (a == "--rate" && i + 1 < argc) {
            rate = std::atoi(argv[++i]);
            if (rate < 50) rate = 50;
            if (rate > 1000) rate = 1000;
            continue;
        }
        if (a == "--mac" && i + 1 < argc) {
            if (!parse_mac(argv[++i], mac)) {
                std::fprintf(stderr, "bad --mac\n");
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "unknown option: %s\n", a.c_str());
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    g7::Device dev;
    std::unique_ptr<VirtualDevice> vdev;
    g7::State state;
    std::string err;

    bool announced_wait = false;
    std::uint64_t last_open_attempt = 0;
    std::uint64_t last_send = 0;
    std::uint64_t last_motor_ms = 0;
    std::uint8_t last_rumble_h = 0, last_rumble_l = 0;
    const std::uint64_t interval_ms =
        static_cast<std::uint64_t>(1000 / (rate > 0 ? rate : 250));

    while (g_running) {
        const std::uint64_t now = now_ms();

        if (!dev.is_open() && now - last_open_attempt >= 2000) {
            last_open_attempt = now;
            if (!dev.open(settle, err)) {
                if (!announced_wait) {
                    std::fprintf(stderr, "waiting for G7 Pro: %s\n", err.c_str());
                    announced_wait = true;
                }
            } else {
                std::printf("G7 Pro connected; virtual DS4 active\n");
                announced_wait = false;
                if (!vdev) {
                    try {
                        vdev = std::make_unique<VirtualDevice>(mac);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr,
                                     "cannot create virtual DS4: %s\n"
                                     "Run as root, or install the udev rule so "
                                     "/dev/uhid is accessible.\n",
                                     e.what());
                        return 1;
                    }
                }
            }
        }

        if (dev.is_open() && (last_rumble_h || last_rumble_l) &&
            last_motor_ms != 0 && now - last_motor_ms > 1500) {
            // The source stopped updating the motors without a stop report
            // (common when a game loses focus); clear them.
            std::string perr;
            dev.rumble(0, 0, 0, perr);
            last_rumble_h = last_rumble_l = 0;
            last_motor_ms = 0;
        }

        if (!dev.is_open()) {
            if (vdev && now - last_send >= interval_ms) {
                state = g7::State{};
                vdev->send_report(to_ds4(state));
                vdev->take_output();
                last_send = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        const auto r = dev.read(state, 2, err);
        if (r == g7::ReadResult::Error) {
            if (verbose) std::fprintf(stderr, "controller lost: %s\n", err.c_str());
            dev.close();
            continue;
        }

        if (vdev && now - last_send >= interval_ms) {
            vdev->send_report(to_ds4(state));
            const auto out = vdev->take_output();
            if (out.heavy >= 0 || out.light >= 0) {
                if (out.heavy >= 0) last_rumble_h = static_cast<std::uint8_t>(out.heavy);
                if (out.light >= 0) last_rumble_l = static_cast<std::uint8_t>(out.light);
                const bool on = last_rumble_h || last_rumble_l;
                last_motor_ms = on ? now : 0;
                std::string rerr;
                if (!dev.rumble(last_rumble_h, last_rumble_l,
                                on ? 100 : 0, rerr))
                    std::fprintf(stderr, "rumble: %s\n", rerr.c_str());
            }
            last_send = now;
        }
    }

    std::printf("shutting down\n");
    dev.close();
    vdev.reset();
    return 0;
}
