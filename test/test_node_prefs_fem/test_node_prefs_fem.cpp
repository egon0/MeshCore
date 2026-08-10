// Regression test for the FEM RX-gain preference binding in NodePrefs.
//
// `radio.fem.rxgain` (the external FEM LNA) and `rxgain` (the SX1262's own
// boosted-RX register) are two different settings, but RadioPrefs::structure()
// binds BOTH JSON keys to `rx_boosted_gain`:
//
//     def("rxgain",     _parent->rx_boosted_gain);
//     def("fem_rxgain", _parent->rx_boosted_gain);   // <-- should be radio_fem_rxgain
//
// so `radio_fem_rxgain` is never serialised at all and `set radio.fem.rxgain`
// does not survive a reboot, even though the CLI handler does call savePrefs().
// Reported in meshcore-dev/MeshCore#3145, fix proposed in #3137.
//
// This is a pure serialisation defect, so it is provable on any host — no
// Heltec V4 hardware required. The test is written against the real NodePrefs,
// not a copy, so it tracks the shipping binding.

#include <gtest/gtest.h>
#include <cstdio>

// IdentityStore.h only defines FILESYSTEM for the Arduino targets. NodePrefs
// itself never touches the filesystem — these tests drive saveSerial/loadSerial
// against streams — but a couple of inline methods in the headers it pulls in
// do, so the host build needs a type with those members present.
struct FILESYSTEM {
  bool mkdir(const char*) { return true; }
};

#include "helpers/CommonCLI.h"

// A stream that captures whatever the serializer writes.
//
// The mock Print in test/mocks/Stream.h stubs every numeric print() to return 0
// without emitting anything, so integer fields would come out blank and the
// assertions below could not tell 0 from 1. These overrides render them.
class CaptureStream : public Stream {
    int len = 0;
    char _buf[2048];
    size_t emit(long long v) {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%lld", v);
        for (int i = 0; i < n; i++) write((uint8_t)tmp[i]);
        return (size_t)n;
    }
public:
    size_t write(uint8_t b) override {
        if (len < (int)sizeof(_buf) - 1) { _buf[len++] = (char)b; _buf[len] = 0; return 1; }
        return 0;
    }
    size_t print(unsigned char v, int r = DEC) override { return emit(v); }
    size_t print(int v, int r = DEC) override { return emit(v); }
    size_t print(unsigned int v, int r = DEC) override { return emit(v); }
    size_t print(long v, int r = DEC) override { return emit(v); }
    size_t print(unsigned long v, int r = DEC) override { return emit((long long)v); }
    size_t print(long long v, int r = DEC) override { return emit(v); }
    size_t print(unsigned long long v, int r = DEC) override { return emit((long long)v); }

    const char* text() { _buf[len] = 0; return _buf; }
};

// A stream that replays a canned config document.
class ReplayStream : public Stream {
    const char* _t; int pos, len;
public:
    ReplayStream(const char* t) : _t(t) { pos = 0; len = (int)strlen(t); }
    int available() override { return len - pos; }
    int read() override { return pos < len ? _t[pos++] : -1; }
    int peek() override { return pos < len ? _t[pos] : -1; }
};

// The two settings must round-trip independently. Give them opposite values so
// a binding that aliases one onto the other cannot pass by coincidence.
TEST(NodePrefsFem, FemRxGainIsSerialisedFromItsOwnField) {
    NodePrefs prefs;
    prefs.rx_boosted_gain  = 0;   // SX1262 internal boosted RX: off
    prefs.radio_fem_rxgain = 1;   // external FEM LNA: on

    CaptureStream out;
    ASSERT_TRUE(prefs.saveSerial(out));

    const char* doc = out.text();
    EXPECT_NE(nullptr, strstr(doc, "rxgain:0"))
        << "expected the SX1262 boosted-gain key to emit 0; got: " << doc;
    EXPECT_NE(nullptr, strstr(doc, "fem_rxgain:1"))
        << "fem_rxgain emitted the wrong field — it is bound to rx_boosted_gain, "
           "so radio_fem_rxgain is never stored. Full document: " << doc;
}

TEST(NodePrefsFem, FemRxGainSurvivesALoad) {
    NodePrefs prefs;
    prefs.rx_boosted_gain  = 1;
    prefs.radio_fem_rxgain = 1;

    // What a node should read back after the operator ran
    // `set radio.fem.rxgain off` and rebooted.
    ReplayStream in("{radio:{rxgain:1,fem_rxgain:0}}");
    ASSERT_TRUE(prefs.loadSerial(in));

    EXPECT_EQ(0, (int)prefs.radio_fem_rxgain)
        << "radio_fem_rxgain did not take the stored fem_rxgain value — "
           "the FEM LNA reverts to its compiled default on every boot";
    EXPECT_EQ(1, (int)prefs.rx_boosted_gain)
        << "rx_boosted_gain was clobbered by the fem_rxgain key";
}

// ── main ───────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
