// SPDX-License-Identifier: GPL-3.0-or-later
//
// g7bt — rumble bridge for the GameSir G7 Pro over Bluetooth.
//
// The Bluetooth firmware exposes a plain HID gamepad (3537:1022) with no
// force feedback in its report descriptor, but it still accepts the standard
// Xbox One rumble report on the hidraw node:
//
//     0x03 0x0F LT RT LM RM
//
// where LM/RM are the left/right grip motors.  This daemon grabs the kernel's
// gamepad node, re-exports it as a virtual Xbox 360 pad with FF_RUMBLE, and
// forwards play/stop requests to that report.
//
// No IMU is available over Bluetooth; use the USB bridge for gyro.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr const char* kPhysName = "GameSir-G7 Pro";
constexpr const char* kVirtualName = "GameSir G7 Pro (Bluetooth)";
constexpr std::uint16_t kVendor = 0x045e;  // Microsoft: SDL maps this to Xbox 360
constexpr std::uint16_t kProduct = 0x028e;
constexpr int kMaxEffects = 32;

std::atomic<bool> g_running{true};

void on_signal(int) { g_running = false; }

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string read_file(const std::string& path) {
    char buf[256] = {};
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return {};
    std::string s(buf, static_cast<std::size_t>(n));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Find the Bluetooth gamepad event node: bus 0005, name "GameSir-G7 Pro",
// and a real ABS axis set (the consumer/keyboard/mouse nodes are skipped).
std::string find_event_node() {
    DIR* d = ::opendir("/sys/class/input");
    if (!d) return {};
    std::string found;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("event", 0) != 0) continue;
        const std::string base = "/sys/class/input/" + name + "/device/";
        if (read_file(base + "name") != kPhysName) continue;
        const std::string bustype = read_file(base + "id/bustype");
        if (bustype != "0005") continue;
        if (read_file(base + "capabilities/abs").empty()) continue;
        found = "/dev/input/" + name;
        break;
    }
    ::closedir(d);
    return found;
}

// Find the hidraw node for the Bluetooth identity 3537:1022.
std::string find_hidraw_node() {
    DIR* d = ::opendir("/sys/class/hidraw");
    if (!d) return {};
    std::string found;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("hidraw", 0) != 0) continue;
        const std::string uevent =
            read_file("/sys/class/hidraw/" + name + "/device/uevent");
        if (uevent.find("HID_ID=0005:00003537:00001022") == std::string::npos)
            continue;
        found = "/dev/" + name;
        break;
    }
    ::closedir(d);
    return found;
}

struct Effect {
    bool used = false;
    int type = 0;
    std::uint16_t strong = 0;
    std::uint16_t weak = 0;
    std::uint32_t length_ms = 0;
};

class Rumble {
public:
    Rumble(std::string hidraw, bool verbose)
        : hidraw_(std::move(hidraw)), verbose_(verbose) {}

    void play(std::uint16_t strong, std::uint16_t weak,
              std::uint32_t length_ms) {
        if (length_ms == 0) length_ms = kDefaultMs;
        last_ = strong || weak;
        stop_at_ = last_ ? now_ms() + length_ms : 0;
        send(strong, weak);
    }

    void stop() {
        last_ = false;
        stop_at_ = 0;
        send(0, 0);
    }

    void tick(std::uint64_t now) {
        if (last_ && stop_at_ && now >= stop_at_) stop();
    }

    int timeout_ms(std::uint64_t now) const {
        if (!last_ || !stop_at_) return -1;
        if (stop_at_ <= now) return 0;
        return static_cast<int>(stop_at_ - now);
    }

    void send(std::uint16_t strong, std::uint16_t weak) {
        if (hidraw_fd_ < 0) {
            hidraw_fd_ = ::open(hidraw_.c_str(), O_WRONLY | O_CLOEXEC);
            if (hidraw_fd_ < 0) {
                if (verbose_)
                    std::fprintf(stderr, "hidraw open %s: %s\n",
                                 hidraw_.c_str(), std::strerror(errno));
                return;
            }
        }
        std::uint8_t report[6] = {
            0x03, 0x0f, 0x00, 0x00,
            static_cast<std::uint8_t>((strong * 255u + 32767u) / 65535u),
            static_cast<std::uint8_t>((weak * 255u + 32767u) / 65535u),
        };
        if (::write(hidraw_fd_, report, sizeof(report)) !=
            static_cast<ssize_t>(sizeof(report))) {
            if (verbose_)
                std::fprintf(stderr, "hidraw write: %s\n", std::strerror(errno));
            ::close(hidraw_fd_);
            hidraw_fd_ = -1;
        }
    }

private:
    static constexpr std::uint32_t kDefaultMs = 5000;

    std::string hidraw_;
    bool verbose_ = false;
    int hidraw_fd_ = -1;
    bool last_ = false;
    std::uint64_t stop_at_ = 0;
};

int emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
    struct input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

// Physical axis -> virtual axis.  Bus 0005 pads use the classic gamepad
// layout: X/Y left stick, Z/RZ right stick, GAS/BRAKE triggers.
int map_abs(std::uint16_t code) {
    switch (code) {
    case ABS_X: return ABS_X;
    case ABS_Y: return ABS_Y;
    case ABS_Z: return ABS_RX;
    case ABS_RZ: return ABS_RY;
    case ABS_GAS: return ABS_Z;
    case ABS_BRAKE: return ABS_RZ;
    case ABS_HAT0X: return ABS_HAT0X;
    case ABS_HAT0Y: return ABS_HAT0Y;
    default: return -1;
    }
}

bool map_key(std::uint16_t code, std::uint16_t& out) {
    switch (code) {
    case BTN_A:
    case BTN_B:
    case BTN_X:
    case BTN_Y:
    case BTN_TL:
    case BTN_TR:
    case BTN_SELECT:
    case BTN_START:
    case BTN_MODE:
    case BTN_THUMBL:
    case BTN_THUMBR:
        out = code;
        return true;
    default:
        return false;
    }
}

int create_uinput() {
    const int fd = ::open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "open /dev/uinput: %s\n", std::strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);

    const std::uint16_t keys[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
                                  BTN_SELECT, BTN_START, BTN_MODE,
                                  BTN_THUMBL, BTN_THUMBR};
    for (std::uint16_t k : keys) ioctl(fd, UI_SET_KEYBIT, k);

    struct {
        std::uint16_t code;
        std::int32_t min, max;
    } axes[] = {
        {ABS_X, 0, 255},    {ABS_Y, 0, 255}, {ABS_RX, 0, 255},
        {ABS_RY, 0, 255},   {ABS_Z, 0, 255}, {ABS_RZ, 0, 255},
        {ABS_HAT0X, -1, 1}, {ABS_HAT0Y, -1, 1},
    };
    for (const auto& a : axes) {
        ioctl(fd, UI_SET_ABSBIT, a.code);
        struct uinput_abs_setup abs = {};
        abs.code = a.code;
        abs.absinfo.minimum = a.min;
        abs.absinfo.maximum = a.max;
        abs.absinfo.flat = 0;
        abs.absinfo.fuzz = 0;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);

    struct uinput_setup setup = {};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = kVendor;
    setup.id.product = kProduct;
    setup.id.version = 0x0114;
    setup.ff_effects_max = kMaxEffects;
    std::snprintf(setup.name, sizeof(setup.name), "%s", kVirtualName);

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        std::fprintf(stderr, "UI_DEV_SETUP: %s\n", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        std::fprintf(stderr, "UI_DEV_CREATE: %s\n", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string find_virtual_event() {
    for (int tries = 0; tries < 50; ++tries) {
        DIR* d = ::opendir("/sys/class/input");
        if (!d) return {};
        std::string found;
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.rfind("event", 0) != 0) continue;
            const std::string base = "/sys/class/input/" + name + "/device/";
            if (read_file(base + "name") == kVirtualName) {
                found = "/dev/input/" + name;
                break;
            }
        }
        ::closedir(d);
        if (!found.empty()) return found;
        ::usleep(20000);
    }
    return {};
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--no-grab] [--verbose]\n"
                 "  Grabs the Bluetooth G7 Pro gamepad and re-exports it as a\n"
                 "  virtual Xbox 360 pad with working rumble.\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    bool grab = true;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--no-grab") {
            grab = false;
            continue;
        }
        if (a == "--verbose") {
            verbose = true;
            continue;
        }
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string phys;
    std::string hidraw;
    bool announced_wait = false;

    while (g_running) {
        phys = find_event_node();
        hidraw = find_hidraw_node();
        if (!phys.empty() && !hidraw.empty()) break;
        if (!announced_wait) {
            std::fprintf(stderr, "waiting for a Bluetooth GameSir G7 Pro\n");
            announced_wait = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!g_running) return 0;

    std::printf("bluetooth pad: %s, %s\n", phys.c_str(), hidraw.c_str());

    const int phys_fd = ::open(phys.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (phys_fd < 0) {
        std::fprintf(stderr, "open %s: %s\n", phys.c_str(), std::strerror(errno));
        return 1;
    }

    if (grab && ioctl(phys_fd, EVIOCGRAB, 1) < 0) {
        std::fprintf(stderr, "EVIOCGRAB: %s\n", std::strerror(errno));
        if (!verbose) {
            ::close(phys_fd);
            return 1;
        }
    }

    const int ui_fd = create_uinput();
    if (ui_fd < 0) {
        if (grab) ioctl(phys_fd, EVIOCGRAB, 0);
        ::close(phys_fd);
        return 1;
    }

    const std::string virt = find_virtual_event();
    std::printf("virtual pad: %s (FF_RUMBLE)\n", virt.c_str());

    Rumble rumble(hidraw, verbose);
    std::array<Effect, kMaxEffects> effects{};

    while (g_running) {
        struct pollfd fds[2] = {{phys_fd, POLLIN, 0}, {ui_fd, POLLIN, 0}};
        const std::uint64_t now = now_ms();
        const int rc = ::poll(fds, 2, rumble.timeout_ms(now));
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "poll: %s\n", std::strerror(errno));
            break;
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (fds[0].revents & POLLIN) {
            struct input_event ev;
            while (::read(phys_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_ABS) {
                    const int code = map_abs(ev.code);
                    if (code >= 0) emit(ui_fd, EV_ABS, code, ev.value);
                } else if (ev.type == EV_KEY) {
                    std::uint16_t code = 0;
                    if (map_key(ev.code, code)) emit(ui_fd, EV_KEY, code, ev.value);
                } else if (ev.type == EV_SYN) {
                    emit(ui_fd, EV_SYN, SYN_REPORT, 0);
                }
            }
        }

        if (fds[1].revents & POLLIN) {
            struct input_event ev;
            while (::read(ui_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_UINPUT) {
                    if (ev.code == UI_FF_UPLOAD) {
                        struct uinput_ff_upload up = {};
                        up.request_id = ev.value;
                        if (ioctl(ui_fd, UI_BEGIN_FF_UPLOAD, &up) == 0) {
                            up.retval = 0;
                            if (up.effect.id >= 0 && up.effect.id < kMaxEffects) {
                                Effect& fx = effects[up.effect.id];
                                fx.used = true;
                                fx.type = up.effect.type;
                                fx.length_ms = up.effect.replay.length;
                                if (up.effect.type == FF_RUMBLE) {
                                    fx.strong = up.effect.u.rumble.strong_magnitude;
                                    fx.weak = up.effect.u.rumble.weak_magnitude;
                                } else if (up.effect.type == FF_PERIODIC) {
                                    fx.strong = 0;
                                    fx.weak = up.effect.u.periodic.magnitude;
                                }
                            }
                            ioctl(ui_fd, UI_END_FF_UPLOAD, &up);
                        }
                    } else if (ev.code == UI_FF_ERASE) {
                        struct uinput_ff_erase er = {};
                        er.request_id = ev.value;
                        if (ioctl(ui_fd, UI_BEGIN_FF_ERASE, &er) == 0) {
                            er.retval = 0;
                            if (er.effect_id < kMaxEffects)
                                effects[er.effect_id] = Effect{};
                            ioctl(ui_fd, UI_END_FF_ERASE, &er);
                        }
                    }
                } else if (ev.type == EV_FF) {
                    if (ev.code < kMaxEffects && effects[ev.code].used) {
                        const Effect& fx = effects[ev.code];
                        if (ev.value)
                            rumble.play(fx.strong, fx.weak, fx.length_ms);
                        else
                            rumble.stop();
                    } else if (!ev.value) {
                        rumble.stop();
                    }
                }
            }
        }

        rumble.tick(now_ms());
    }

    std::printf("shutting down\n");
    rumble.stop();
    ioctl(ui_fd, UI_DEV_DESTROY);
    ::close(ui_fd);
    if (grab) ioctl(phys_fd, EVIOCGRAB, 0);
    ::close(phys_fd);
    return g_running ? 1 : 0;
}
