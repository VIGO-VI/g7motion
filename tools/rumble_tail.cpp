// SPDX-License-Identifier: GPL-3.0-or-later
//
// Measure how long the G7 Pro keeps vibrating after a single rumble packet.
//
// A rumble packet carries an on-period; the firmware stops on its own when the
// period expires.  The controller's own accelerometer picks up the vibration,
// so the tail can be measured without opening the pad: send one packet, then
// watch the acceleration variance until it returns to the resting level.
//
// Run with the bridge stopped (it owns the vendor interface):
//
//   systemctl --user stop g7motion
//   ./build/rumble_tail --on 255
//   systemctl --user start g7motion

#include "g7pro.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
}

double magnitude(const g7::State& s) {
    const double x = s.accel_x, y = s.accel_y, z = s.accel_z;
    return std::sqrt(x * x + y * y + z * z);
}

struct Window {
    double sd = 0;
    double peak = 0;
    int n = 0;
};

// Collect magnitude samples for @ms and return per-window standard deviation.
std::vector<Window> sample(g7::Device& dev, int ms, int window_ms,
                           std::string& err) {
    std::vector<Window> out;
    std::vector<double> cur;
    const std::uint64_t end = now_ms() + static_cast<std::uint64_t>(ms);
    std::uint64_t win_end = now_ms() + static_cast<std::uint64_t>(window_ms);

    while (now_ms() < end) {
        g7::State s;
        const auto r = dev.read(s, 20, err);
        if (r == g7::ReadResult::Error) break;
        if (r != g7::ReadResult::Ok) continue;
        const double m = magnitude(s);
        cur.push_back(m);
        if (now_ms() >= win_end) {
            Window w;
            if (!cur.empty()) {
                double mean = 0;
                for (double v : cur) mean += v;
                mean /= cur.size();
                double var = 0;
                for (double v : cur) var += (v - mean) * (v - mean);
                w.sd = std::sqrt(var / cur.size());
                w.peak = std::fabs(cur.front() - mean);
                for (double v : cur) w.peak = std::max(w.peak, std::fabs(v - mean));
                w.n = static_cast<int>(cur.size());
            }
            out.push_back(w);
            cur.clear();
            win_end += static_cast<std::uint64_t>(window_ms);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    int on_period = 0xff;
    int seconds = 4;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--on") && i + 1 < argc) {
            on_period = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--on 0..255] [--seconds N]\n",
                         argv[0]);
            return 2;
        }
    }
    if (on_period < 1 || on_period > 255) on_period = 0xff;
    if (seconds < 1 || seconds > 30) seconds = 4;

    g7::Device dev;
    std::string err;
    if (!dev.open(8, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }

    std::printf("hold the pad still; measuring resting level...\n");
    const auto base = sample(dev, 600, 50, err);
    double base_sd = 0;
    int base_n = 0;
    for (const auto& w : base) {
        base_sd += w.sd;
        ++base_n;
    }
    base_sd = base_n ? base_sd / base_n : 0;
    std::printf("resting sd = %.1f\n", base_sd);

    std::printf("sending one packet, on-period %d...\n", on_period);
    if (!dev.rumble_raw(static_cast<std::uint8_t>(on_period), 100, 100, err)) {
        std::fprintf(stderr, "rumble: %s\n", err.c_str());
        return 1;
    }

    const std::uint64_t t0 = now_ms();
    const auto win = sample(dev, seconds * 1000, 50, err);

    const double threshold = std::max(base_sd * 3.0, 100.0);
    std::uint64_t last_active = 0;
    int idx = 0;
    for (const auto& w : win) {
        const std::uint64_t t = static_cast<std::uint64_t>(idx + 1) * 50;
        std::printf("t=%4llu ms  sd=%7.1f  peak=%7.1f  n=%d%s\n",
                    static_cast<unsigned long long>(t), w.sd, w.peak, w.n,
                    w.sd > threshold ? "  ACTIVE" : "");
        if (w.sd > threshold) last_active = t;
        ++idx;
    }

    std::printf("threshold %.1f, tail=%llu ms\n", threshold,
                static_cast<unsigned long long>(last_active));
    (void)t0;
    dev.close();
    return 0;
}
