// SPDX-License-Identifier: GPL-3.0-or-later
#include "g7pro.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace g7 {
namespace {

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

bool Device::is_supported_pid(std::uint16_t pid) {
    switch (pid) {
    case 0x109b: // wired config (Shadow Ember and most editions)
    case 0x109c: // 2.4 GHz receiver config
    case 0x100a: // HID mode; still answers the vendor protocol
    case 0x1003: // White Trimode wired
    case 0x1004: // White Trimode receiver
    case 0x105d: // Zenless Zone Zero wired
    case 0x105e: // Zenless Zone Zero receiver
    case 0x10ba: // Amazon edition wired
    case 0x10bb: // Amazon edition receiver
        return true;
    default:
        return false; // includes native/GIP 0x1022
    }
}

bool Device::has_vendor_interface(libusb_device* dev) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return false;
    }

    bool ok = false;
    for (std::uint8_t i = 0; i < cfg->bNumInterfaces && !ok; ++i) {
        const libusb_interface& iface = cfg->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const libusb_interface_descriptor& d = iface.altsetting[a];
            if (d.bInterfaceNumber != 0 || d.bInterfaceClass != 0xff ||
                d.bNumEndpoints < 2) {
                continue;
            }
            bool out = false, in = false;
            for (int e = 0; e < d.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = d.endpoint[e];
                if (ep.bEndpointAddress == 0x02) out = true;
                if (ep.bEndpointAddress == 0x82) in = true;
            }
            if (out && in) ok = true;
        }
    }

    libusb_free_config_descriptor(cfg);
    return ok;
}

Device::~Device() { close(); }

bool Device::open(int settle_heartbeats, std::string& err) {
    close();

    if (libusb_init(&ctx_) != 0 || !ctx_) {
        err = "libusb_init failed";
        ctx_ = nullptr;
        return false;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) {
        err = "libusb_get_device_list failed";
        close();
        return false;
    }

    libusb_device* found = nullptr;
    for (ssize_t i = 0; i < count && !found; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue;
        if (desc.idVendor != kVid || !is_supported_pid(desc.idProduct)) continue;
        if (!has_vendor_interface(dev)) continue;
        found = dev;
    }

    if (!found) {
        libusb_free_device_list(list, 1);
        err = "GameSir G7 Pro not found (wired, vendor interface 0; not 3537:1022)";
        close();
        return false;
    }

    const int rc = libusb_open(found, &handle_);
    if (rc != 0 || !handle_) {
        libusb_free_device_list(list, 1);
        err = std::string("libusb_open failed: ") + libusb_error_name(rc);
        handle_ = nullptr;
        close();
        return false;
    }
    libusb_free_device_list(list, 1);

    // Linux: detach xpad automatically on claim and reattach on release.
    (void)libusb_set_auto_detach_kernel_driver(handle_, 1);

    const int claim = libusb_claim_interface(handle_, 0);
    if (claim != 0) {
        err = std::string("claim interface 0 failed: ") + libusb_error_name(claim) +
              " (is another program using the controller?)";
        close();
        return false;
    }

    // A freshly claimed device accepts heartbeats immediately but needs a
    // warm-up before the first read.  The observed app cadence is ~316 ms.
    for (int i = 0; i < settle_heartbeats; ++i) {
        if (!send_heartbeat(err)) {
            close();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    return true;
}

void Device::close() {
    if (handle_) {
        (void)libusb_release_interface(handle_, 0);
        (void)libusb_attach_kernel_driver(handle_, 0);
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
    seq_ = 0;
    last_heartbeat_ms_ = 0;
}

bool Device::send_packet(const std::uint8_t* data, int len, std::string& err) {
    if (!handle_) {
        err = "device not open";
        return false;
    }
    int transferred = 0;
    const int rc = libusb_interrupt_transfer(
        handle_, 0x02, const_cast<std::uint8_t*>(data), len, &transferred, 1000);
    if (rc != 0) {
        err = std::string("interrupt OUT failed: ") + libusb_error_name(rc);
        return false;
    }
    if (transferred != len) {
        err = "short interrupt OUT transfer";
        return false;
    }
    return true;
}

bool Device::send_heartbeat(std::string& err) {
    std::uint8_t pkt[64] = {};
    pkt[0] = 0x0f;
    pkt[1] = 0x00;
    pkt[2] = ++seq_;
    pkt[3] = 0x02;
    pkt[4] = 0xf2;
    pkt[5] = 0x00;
    if (!send_packet(pkt, sizeof(pkt), err)) return false;
    last_heartbeat_ms_ = now_ms();
    return true;
}

bool Device::parse(const std::uint8_t* d, int len, State& s) {
    if (len < 61) return false;
    if (d[0] != 0x10 || d[3] != 0x3c || d[4] != 0xe0) return false;

    s.lx = d[5];
    s.ly = d[6];
    s.rx = d[7];
    s.ry = d[8];

    // Raw/pre-binding button group.  This is the physical controller, not
    // the result of on-board remaps.
    const std::uint8_t b55 = d[55];
    const std::uint8_t b56 = d[56];
    const std::uint8_t b57 = d[57];
    const std::uint8_t b58 = d[58];

    s.x = (b55 & 0x10) != 0;
    s.a = (b55 & 0x20) != 0;
    s.b = (b55 & 0x40) != 0;
    s.y = (b55 & 0x80) != 0;

    s.lb = (b56 & 0x01) != 0;
    s.rb = (b56 & 0x02) != 0;
    s.lt_digital = (b56 & 0x04) != 0;
    s.rt_digital = (b56 & 0x08) != 0;
    s.view = (b56 & 0x10) != 0;
    s.menu = (b56 & 0x20) != 0;
    s.l3 = (b56 & 0x40) != 0;
    s.r3 = (b56 & 0x80) != 0;

    s.guide = (b57 & 0x01) != 0;
    s.share = (b57 & 0x02) != 0;
    s.l4 = (b57 & 0x08) != 0;
    s.r4 = (b57 & 0x10) != 0;

    s.l5 = (b58 & 0x01) != 0;
    s.r5 = (b58 & 0x02) != 0;

    s.hat_x = 0;
    s.hat_y = 0;
    switch (b55 & 0x0f) {
    case 0x0: s.hat_y = -1; break; // up
    case 0x2: s.hat_x = +1; break; // right
    case 0x4: s.hat_y = +1; break; // down
    case 0x6: s.hat_x = -1; break; // left
    default: break;                // 0xf = neutral
    }

    s.lt = d[12];
    s.rt = d[13];

    s.gyro_x = static_cast<std::int16_t>(rd16(d + 17));
    s.gyro_y = static_cast<std::int16_t>(rd16(d + 19));
    s.gyro_z = static_cast<std::int16_t>(rd16(d + 21));

    s.accel_x = static_cast<std::int16_t>(rd16(d + 23));
    s.accel_y = static_cast<std::int16_t>(rd16(d + 25));
    s.accel_z = static_cast<std::int16_t>(rd16(d + 27));

    s.battery = d[33] <= 100 ? d[33] : -1;
    s.charging = (d[32] & 0x01) != 0;
    return true;
}

bool Device::rumble(std::uint8_t strong, std::uint8_t weak,
                    std::uint8_t duration, std::string& err)
{
    std::uint8_t pkt[13] = {};

    if (!handle_) {
        err = "device not open";
        return false;
    }

    pkt[0] = 0x09;
    pkt[2] = ++seq_;
    pkt[3] = 0x09;
    pkt[4] = 0x00;
    pkt[5] = 0x0f;
    pkt[6] = 0x00;
    pkt[7] = 0x00;
    pkt[8] = static_cast<std::uint8_t>((strong * 100 + 127) / 255);
    pkt[9] = static_cast<std::uint8_t>((weak * 100 + 127) / 255);
    pkt[10] = duration;

    int transferred = 0;
    const int rc = libusb_interrupt_transfer(
        handle_, 0x02, pkt, sizeof(pkt), &transferred, 1000);
    if (rc != 0) {
        err = std::string("rumble OUT failed: ") + libusb_error_name(rc);
        return false;
    }
    if (transferred != static_cast<int>(sizeof(pkt))) {
        err = "short rumble OUT transfer";
        return false;
    }
    return true;
}

ReadResult Device::read(State& out, int timeout_ms, std::string& err) {
    if (!handle_) {
        err = "device not open";
        return ReadResult::Error;
    }

    const std::uint64_t deadline = now_ms() + static_cast<std::uint64_t>(timeout_ms);
    for (;;) {
        const std::uint64_t now = now_ms();
        if (now - last_heartbeat_ms_ >= 300) {
            if (!send_heartbeat(err)) return ReadResult::Error;
        }

        const std::uint64_t left = deadline > now_ms() ? deadline - now_ms() : 0;
        if (left == 0) return ReadResult::Timeout;

        std::uint8_t buf[64] = {};
        int transferred = 0;
        const int rc = libusb_interrupt_transfer(
            handle_, 0x82, buf, sizeof(buf), &transferred,
            static_cast<unsigned int>(left));

        if (rc == LIBUSB_ERROR_TIMEOUT) return ReadResult::Timeout;
        if (rc != 0) {
            err = std::string("interrupt IN failed: ") + libusb_error_name(rc);
            return ReadResult::Error;
        }
        if (parse(buf, transferred, out)) return ReadResult::Ok;
        // Other reports (telemetry/read replies) are not input frames.
    }
}

void Device::list() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) return;
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count > 0) {
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
            if (d.idVendor != kVid) continue;
            std::printf("%04x:%04x  bus %03d  addr %03d  %s\n", d.idVendor,
                        d.idProduct, libusb_get_bus_number(list[i]),
                        libusb_get_device_address(list[i]),
                        is_supported_pid(d.idProduct) ? "supported" : "not supported");
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

} // namespace g7
