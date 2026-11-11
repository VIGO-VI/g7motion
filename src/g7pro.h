// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// GameSir G7 Pro vendor-USB input reader.
//
// The controller's configuration/telemetry interface is interface 0
// (vendor class ff/47/d0, interrupt OUT 0x02, interrupt IN 0x82).  Linux
// binds xpad to it, so this reader detaches xpad, claims the interface,
// keeps a heartbeat session alive, and reads report 0x10 frames.  Those
// frames carry the physical sticks/buttons and the raw 6-axis IMU.

#include <cstdint>
#include <string>

#include <libusb-1.0/libusb.h>

namespace g7 {

struct State {
    // Processed sticks (0..255, 0x80 centre).
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;

    // Physical (pre-remap) buttons.
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false;
    bool lt_digital = false, rt_digital = false;
    bool view = false, menu = false, l3 = false, r3 = false;
    bool guide = false, share = false;
    bool l4 = false, r4 = false, l5 = false, r5 = false;

    // D-pad: -1/0/+1, y = -1 up.
    int hat_x = 0, hat_y = 0;

    // Triggers 0..255.
    std::uint8_t lt = 0, rt = 0;

    // Raw IMU, little-endian int16.  Gyro is ~16 counts per deg/s,
    // accelerometer ~8192 counts per g (same scale as a DS4).
    std::int16_t gyro_x = 0, gyro_y = 0, gyro_z = 0;
    std::int16_t accel_x = 0, accel_y = 0, accel_z = 0;

    int battery = -1;        // 0..100, -1 unknown
    bool charging = false;
};

enum class ReadResult { Ok, Timeout, Error };

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Open + claim interface 0, then run the settle heartbeats.
    bool open(int settle_heartbeats, std::string& err);
    void close();
    bool is_open() const { return handle_ != nullptr; }

    // Read one 0x10 input frame.  Sends a heartbeat when one is due.
    ReadResult read(State& out, int timeout_ms, std::string& err);

    // Send the 13-byte GIP four-motor rumble packet.  strong/weak are 0..255.
    bool rumble(std::uint8_t strong, std::uint8_t weak,
                std::uint8_t duration, std::string& err);

    // Send one packet with an explicit on-period and no repeat, for timing
    // experiments.  Units of on_period are firmware-defined.
    bool rumble_raw(std::uint8_t on_period, std::uint8_t strong,
                    std::uint8_t weak, std::string& err);

    // Print every GameSir USB device and exit.
    static void list();

    static constexpr std::uint16_t kVid = 0x3537;

private:
    bool send_heartbeat(std::string& err);
    bool send_packet(const std::uint8_t* data, int len, std::string& err);
    static bool parse(const std::uint8_t* data, int len, State& out);
    static bool is_supported_pid(std::uint16_t pid);
    static bool has_vendor_interface(libusb_device* dev);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    std::uint8_t seq_ = 0;
    std::uint64_t last_heartbeat_ms_ = 0;
};

} // namespace g7
