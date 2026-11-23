
// Exercise the production VGN V98Pro V4 poller using simulated HID replies.
// No physical keyboard or receiver required.

#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

namespace {

constexpr USHORT VID = 0x391D;
constexpr USHORT PID = 0x2A04;

constexpr BYTE REPORT_ID = 0x09;

HidColl Receiver() {
    HidColl c;
    c.vid = VID;
    c.pid = PID;
    c.iface = 1;
    c.product = L"VGN V98Pro V4 2.4G Dongle";
    c.path = L"vgn-v98prov4-receiver";
    c.caps.UsagePage = 0xFF60;
    c.caps.Usage = 0x0061;
    c.caps.InputReportByteLength = 64;
    c.caps.OutputReportByteLength = 64;
    return c;
}

// -----------------------------------------------------------------------------
// Mock transport state
// -----------------------------------------------------------------------------

int opens = 0;
int writes = 0;
int reads = 0;
int readTimeout = 0;

BYTE percent = 92;
BYTE charge = 0x01;

bool noResponse = false;
bool invalidHeader = false;
bool shortResponse = false;

std::deque<std::vector<BYTE>> pending;

// Reset only transport counters and queued reports.
// Do not reset scenario configuration here.
void ResetTransport() {
    opens = 0;
    writes = 0;
    reads = 0;
    readTimeout = 0;
    pending.clear();
}

// Reset simulated device behavior before each test.
void ResetScenario() {
    percent = 92;
    charge = 0x01;

    noResponse = false;
    invalidHeader = false;
    shortResponse = false;

    ResetTransport();
}

// -----------------------------------------------------------------------------
// Test helpers
// -----------------------------------------------------------------------------

BYTE ExpectedChecksum(const std::vector<BYTE>& data) {
    assert(data.size() >= 64);

    BYTE sum = 0;

    for (size_t i = 0; i < 63; ++i) {
        sum = static_cast<BYTE>(sum + data[i]);
    }

    return static_cast<BYTE>(~sum);
}

PollContext Poll(std::vector<HidColl> colls) {
    PollContext ctx;
    ctx.colls = std::move(colls);

    ResetTransport();

    PollVgnKeyboard(ctx);

    return ctx;
}

} // namespace

// -----------------------------------------------------------------------------
// Mock HID implementation
//
// These definitions must remain in the global namespace because HidDev is
// declared in the global namespace.
// -----------------------------------------------------------------------------

std::wstring HidProduct(const HidColl& c) {
    return c.product;
}

bool HidDev::Open(const HidColl& c) {
    ++opens;
    c_ = c;
    return true;
}

void HidDev::Close() {
}

void HidDev::Flush() {
    pending.clear();
}

bool HidDev::Write(const std::vector<BYTE>& data, int) {
    ++writes;

    assert(data.size() >= 64);
    assert(data.size() <= c_.outLen());

    auto report = data;
    report.resize(c_.outLen(), 0);

    // Validate VGN battery request.
    assert(report[0] == REPORT_ID);
    assert(report[1] == 0x82);
    assert(report[2] == 0x01);
    assert(report[3] == 0x00);
    assert(report[4] == 0x01);
    assert(report[5] == 0x00);
    assert(report[6] == 0x06);

    // Bytes 7..62 must be zero.
    assert(std::all_of(
        report.begin() + 7,
        report.begin() + 63,
        [](BYTE b) {
            return b == 0;
        }
    ));

    // Validate checksum.
    assert(report[63] == ExpectedChecksum(report));

    // Simulate a sleeping keyboard / no battery response.
    if (noResponse) {
        return true;
    }

    // Simulate an unrelated or malformed response.
    if (invalidHeader) {
        pending.push_back({
            REPORT_ID,
            0xAA,
            0xBB,
            percent,
            charge
        });

        return true;
    }

    // Simulate a truncated response.
    if (shortResponse) {
        pending.push_back({
            REPORT_ID,
            0xFE
        });

        return true;
    }

    // Valid VGN battery response:
    // 09 FE 05 XX YY
    pending.push_back({
        REPORT_ID,
        0xFE,
        0x05,
        percent,
        charge
    });

    return true;
}

int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    ++reads;

    assert(timeoutMs > 0);
    assert(timeoutMs <= 1000);

    readTimeout = timeoutMs;

    if (pending.empty()) {
        return -1;
    }

    out = std::move(pending.front());
    pending.pop_front();

    return static_cast<int>(out.size());
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

namespace {

// 1. Device filtering.
void TestDeviceFiltering() {
    ResetScenario();

    auto vgn = Receiver();

    auto wrongVendor = vgn;
    wrongVendor.vid = 0x320F;

    auto wrongPid = vgn;
    wrongPid.pid = 0x2A05;

    auto wrongPage = vgn;
    wrongPage.caps.UsagePage = 0xFFEF;

    auto wrongUsage = vgn;
    wrongUsage.caps.Usage = 0x0062;

    auto shortInput = vgn;
    shortInput.caps.InputReportByteLength = 32;

    auto shortOutput = vgn;
    shortOutput.caps.OutputReportByteLength = 32;

    auto wrongName = vgn;
    wrongName.product = L"VGN V98Pro V3";

    auto ctx = Poll({
        wrongVendor,
        wrongPid,
        wrongPage,
        wrongUsage,
        shortInput,
        shortOutput,
        wrongName
    });

    assert(ctx.out.empty());
    assert(opens == 0);
    assert(writes == 0);
}

// 2. Normal battery response.
void TestNormalBattery() {
    ResetScenario();

    auto vgn = Receiver();

    percent = 92;
    charge = 0x01;

    auto ctx = Poll({vgn});

    assert(opens == 1);
    assert(writes == 1);
    assert(reads >= 1);

    assert(ctx.out.size() == 1);

    const auto& status = ctx.out.front();

    assert(status.name == L"VGN V98Pro V4");
    assert(status.kind == DeviceKind::Keyboard);
    assert(status.percent == 92);
    assert(!status.charging);
    assert(!status.asleep);
}

// 3. Charging response.
void TestChargingBattery() {
    ResetScenario();

    auto vgn = Receiver();

    percent = 65;
    charge = 0x10;

    auto ctx = Poll({vgn});

    assert(ctx.out.size() == 1);

    const auto& status = ctx.out.front();

    assert(status.percent == 65);
    assert(status.charging);
    assert(!status.asleep);
}

// 4. Battery boundaries: 0% and 100%.
void TestBatteryBoundaries() {
    ResetScenario();

    auto vgn = Receiver();

    percent = 0;
    charge = 0x10;

    auto ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().percent == 0);
    assert(ctx.out.front().charging);
    assert(!ctx.out.front().asleep);

    ResetScenario();

    percent = 100;
    charge = 0x01;

    ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().percent == 100);
    assert(!ctx.out.front().charging);
    assert(!ctx.out.front().asleep);
}

// 5. Invalid battery percentage and charging state.
void TestInvalidBattery() {
    ResetScenario();

    auto vgn = Receiver();

    percent = 101;
    charge = 0x01;

    auto ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().asleep);
    assert(ctx.out.front().percent < 0);

    ResetScenario();

    percent = 92;
    charge = 0x03;

    ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().asleep);
    assert(ctx.out.front().percent < 0);
}

// 6. Invalid and truncated responses.
void TestInvalidResponses() {
    ResetScenario();

    auto vgn = Receiver();

    invalidHeader = true;

    auto ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().asleep);
    assert(ctx.out.front().percent < 0);

    ResetScenario();

    shortResponse = true;

    ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(ctx.out.front().asleep);
    assert(ctx.out.front().percent < 0);
}

// 7. Keyboard does not respond.
void TestNoResponse() {
    ResetScenario();

    auto vgn = Receiver();

    noResponse = true;

    auto ctx = Poll({vgn});

    assert(ctx.out.size() == 1);
    assert(opens == 1);
    assert(writes == 1);
    assert(ctx.out.front().asleep);
    assert(ctx.out.front().percent < 0);
}

// 8. Device claiming.
void TestClaimedDevice() {
    ResetScenario();

    auto vgn = Receiver();

    auto ctx = Poll({vgn});

    assert(ctx.IsClaimed(VID, PID));
}

// 9. Wrong device must not be claimed.
void TestWrongDeviceDoesNotClaim() {
    ResetScenario();

    auto vgn = Receiver();
    vgn.pid = 0x2A05;

    auto ctx = Poll({vgn});

    assert(ctx.out.empty());
    assert(!ctx.IsClaimed(VID, 0x2A05));
}

} // namespace

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {

    TestDeviceFiltering();
    TestInvalidBattery();
    TestInvalidResponses();
    TestNoResponse();
    TestWrongDeviceDoesNotClaim();

    TestNormalBattery();
    TestChargingBattery();
    TestBatteryBoundaries();
    TestClaimedDevice();

    std::puts(
        "VGN V98Pro V4 production driver tests passed"
    );

    return 0;
}
