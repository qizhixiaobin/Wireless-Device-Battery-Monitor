
// VGN V98Pro V4 keyboard on its 2.4 GHz receiver.
//
// Device:
//   VID: 391D
//   PID: 2A04
//   Collection: FF60:0061
//   Input Report:  64 bytes
//   Output Report: 64 bytes
//   Report ID: 0x09
//
// Battery request:
//   09 82 01 00 01 00 06 00 ... checksum
//
// Battery response:
//   09 FE 05 XX YY
//
//   XX = battery percentage
//   YY = 0x01 (not charging)
//        0x10 (charging)
//
// Checksum:
//   (~sum(frame[0..62])) & 0xFF
//
// Protocol verified against VGN HUB WebHID communication.

#include "drivers.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr USHORT kVgnVid = 0x391D;
constexpr USHORT kV98ProPid = 0x2A04;

constexpr USHORT kUsagePage = 0xFF60;
constexpr USHORT kUsage = 0x0061;

constexpr BYTE kReportId = 0x09;
constexpr BYTE kWakeCmd = 0x82;

constexpr BYTE kBatteryTag1 = 0xFE;
constexpr BYTE kBatteryTag2 = 0x05;

constexpr BYTE kNotCharging = 0x01;
constexpr BYTE kCharging = 0x10;

constexpr size_t kReportLength = 64;
constexpr int kQueryTimeoutMs = 1000;
constexpr int kReadIntervalMs = 100;

// -----------------------------------------------------------------------------
// Checksum
// -----------------------------------------------------------------------------

BYTE Checksum(const std::vector<BYTE>& frame) {
    BYTE sum = 0;

    const size_t count = std::min<size_t>(63, frame.size());

    for (size_t i = 0; i < count; ++i) {
        sum = static_cast<BYTE>(sum + frame[i]);
    }

    return static_cast<BYTE>(~sum);
}

// -----------------------------------------------------------------------------
// Device identification
// -----------------------------------------------------------------------------

bool IsV98ProV4(const std::wstring& product) {
    std::wstring name = product;

    for (auto& ch : name) {
        ch = towlower(ch);
    }

    return name.find(L"v98pro v4") != std::wstring::npos;
}

bool IsV98ProV4Collection(const HidColl& c) {
    return c.vid == kVgnVid &&
           c.pid == kV98ProPid &&
           c.page() == kUsagePage &&
           c.usage() == kUsage &&
           c.inLen() >= kReportLength &&
           c.outLen() >= kReportLength;
}

// -----------------------------------------------------------------------------
// Battery response parser
// -----------------------------------------------------------------------------

bool ParseBattery(
    const std::vector<BYTE>& report,
    int& percent,
    bool& charging
) {
    if (report.size() < 4) {
        return false;
    }

    // Support both:
    //   09 FE 05 XX YY
    //   FE 05 XX YY

    const size_t offset =
        report[0] == kReportId ? 1 : 0;

    if (report.size() < offset + 4) {
        return false;
    }

    const BYTE* data = report.data() + offset;

    if (data[0] != kBatteryTag1 ||
        data[1] != kBatteryTag2) {
        return false;
    }

    const int value = data[2];

    if (value > 100) {
        return false;
    }

    if (data[3] != kNotCharging &&
        data[3] != kCharging) {
        return false;
    }

    percent = value;
    charging = (data[3] == kCharging);

    return true;
}

// -----------------------------------------------------------------------------
// Request construction
// -----------------------------------------------------------------------------

std::vector<BYTE> BuildBatteryRequest() {
    std::vector<BYTE> request(kReportLength, 0);

    request[0] = kReportId;
    request[1] = kWakeCmd;
    request[2] = 0x01;
    request[4] = 0x01;
    request[6] = 0x06;

    request[63] = Checksum(request);

    return request;
}

// -----------------------------------------------------------------------------
// Battery query
// -----------------------------------------------------------------------------

bool QueryBattery(
    const HidColl& collection,
    int& percent,
    bool& charging
) {
    HidDev device;

    if (!device.Open(collection)) {
        return false;
    }

    // Remove stale input reports before sending a new request.
    device.Flush();

    const std::vector<BYTE> request =
        BuildBatteryRequest();

    if (!device.Write(request)) {
        return false;
    }

    std::vector<BYTE> response;

    const ULONGLONG deadline =
        GetTickCount64() + kQueryTimeoutMs;

    while (true) {
        const ULONGLONG now = GetTickCount64();

        if (now >= deadline) {
            break;
        }

        const int remaining =
            static_cast<int>(deadline - now);

        const int waitMs =
            std::min(remaining, kReadIntervalMs);

        const int received =
            device.Read(response, waitMs);

        if (received <= 0) {
            continue;
        }

        if (ParseBattery(response, percent, charging)) {
            return true;
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// Battery cache
// -----------------------------------------------------------------------------

struct BatteryCache {
    int percent = -1;
    bool charging = false;
};

BatteryCache& GetBatteryCache() {
    static BatteryCache cache;
    return cache;
}

} // namespace

// =============================================================================
// BatteryHub driver entry
// =============================================================================

void PollVgnKeyboard(PollContext& ctx) {

    if (ctx.IsClaimed(kVgnVid, kV98ProPid)) {
        return;
    }

    BatteryCache& cache = GetBatteryCache();

    for (const HidColl* collection :
         ctx.Find(
             kVgnVid,
             kV98ProPid,
             [](const HidColl& c) {
                 return IsV98ProV4Collection(c);
             }
         )) {

        if (collection == nullptr) {
            continue;
        }

        const std::wstring product =
            HidProduct(*collection);

        if (!IsV98ProV4(product)) {
            continue;
        }

        int percent = -1;
        bool charging = false;

        if (QueryBattery(*collection, percent, charging)) {
            cache.percent = percent;
            cache.charging = charging;
        }

        DeviceStatus status;

        status.key = L"vgn:v98prov4";
        status.name = L"VGN V98Pro V4";
        status.kind = DeviceKind::Keyboard;
        status.via = L"2.4 GHz receiver";

        status.percent = cache.percent;
        status.charging = cache.charging;

        // No successful battery reading yet.
        status.asleep = (cache.percent < 0);

        // Keep the last known battery value while the keyboard sleeps.
        status.graceSec = 1800;

        ctx.Add(status, kVgnVid, kV98ProPid);

        break;
    }
}
