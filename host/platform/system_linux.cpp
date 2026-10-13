// Host system backend: monotonic clock via CLOCK_MONOTONIC, battery
// from the standard sysfs power-supply interface (the OS already
// integrates the hardware; we just read its numbers). No die sensor.

#include "platform/system.hpp"

#include <ctime>

#include <cstdio>
#include <cstring>

namespace platform {

namespace {

uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 1'000ull;
}

// PicoCalc battery register via the custom keyboard firmware's sysfs
// node. The driver prints the raw 8-bit register in decimal, so it
// needs unpacking before use: bit7 = charging flag (1 = charging),
// bits 6..0 = percent. Uncharging 44% reads "44"; charging 44% reads
// "172" (0xAC) — treating that as a percent would be nonsense.
bool read_picocalc_battery(int* percent, bool* charging) {
    std::FILE* f = std::fopen("/sys/firmware/picocalc/battery_percent", "r");
    if (f == nullptr) {
        return false;
    }
    int raw = -1;
    const int got = std::fscanf(f, "%d", &raw);
    std::fclose(f);
    if (got != 1 || raw < 0 || raw > 255) {
        return false;
    }
    *charging = (raw & 0x80) != 0;
    int pct = raw & 0x7F;
    if (pct > 100) {
        pct = 100;
    }
    *percent = pct;
    return true;
}

// Best-effort sysfs battery read: first supply that reports a capacity.
bool read_sysfs_battery(int* percent, bool* charging) {
    static const char* kSupplies[] = {"picocalc", "BAT0", "BAT1", "bat", "battery", "axp20x-battery"};
    for (const char* name : kSupplies) {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", name);
        std::FILE* f = std::fopen(path, "r");
        if (f == nullptr) {
            continue;
        }
        int pct = -1;
        const int got = std::fscanf(f, "%d", &pct);
        std::fclose(f);
        if (got != 1 || pct < 0 || pct > 100) {
            continue;
        }
        *percent = pct;
        *charging = false;
        std::snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", name);
        f = std::fopen(path, "r");
        if (f != nullptr) {
            char status[32] = {0};
            if (std::fgets(status, sizeof(status), f) != nullptr) {
                *charging = std::strncmp(status, "Charging", 8) == 0 ||
                            std::strncmp(status, "Full", 4) == 0;
            }
            std::fclose(f);
        }
        return true;
    }
    return false;
}

BatteryInfo g_battery;         // cache; percent = -1 until first success
uint64_t g_last_poll_us = 0;

}  // namespace

BatteryInfo read_battery_info() {
    BatteryInfo info;
    int pct = -1;
    bool charging = false;
    // The PicoCalc node first (the device has no power_supply class);
    // fall back to the generic interface for dev machines.
    if (!read_picocalc_battery(&pct, &charging) && !read_sysfs_battery(&pct, &charging)) {
        return info;
    }
    info.percent = pct;
    info.charging = charging;
    return info;
}

BatteryInfo battery_status() {
    return g_battery;
}

BatteryInfo battery_poll() {
    // Same pacing as the firmware (~5 s between STM32 reads); sysfs is
    // far cheaper but there is no reason to poll it per frame either.
    const uint64_t now = now_us();
    if (g_last_poll_us == 0 || now - g_last_poll_us >= 5'000'000ull) {
        g_last_poll_us = now;
        g_battery = read_battery_info();
    }
    return g_battery;
}

uint64_t uptime_us() {
    return now_us();
}

uint32_t uptime_ms() {
    return static_cast<uint32_t>(now_us() / 1000);
}

float die_temp_c() {
    // No RP2 die sensor on this target. Read the SoC thermal zone if it
    // exists, else NaN — the diag path treats NaN as "unavailable".
    std::FILE* f = std::fopen(
        "/sys/class/thermal/thermal_zone0/temp", "r");
    if (f == nullptr) {
        return 0.0f / 0.0f;
    }
    int milli = 0;
    const int got = std::fscanf(f, "%d", &milli);
    std::fclose(f);
    if (got != 1) {
        return 0.0f / 0.0f;
    }
    return static_cast<float>(milli) / 1000.0f;
}

}  // namespace platform
