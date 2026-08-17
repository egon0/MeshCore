// Stage 5 pre-flight: the channel blocklist rests on one claim -- a stored channel secret
// identifies a channel exactly, where the 1-byte wire hash cannot. These tests hold that
// claim without depending on anyone's traffic.
//
// Everything here is constructed locally. The two channels are ordinary public hashtag names,
// so their keys are SHA256 of a public string and carry no information about anybody; the
// packets are built with the firmware's own encryptThenMAC(). Nothing captured off the air is
// stored in this repository -- when the wire format itself needs re-checking against reality,
// reference/chan_filter_vectors.py does that on demand against live traffic and keeps the
// packets local.
//
// The test that matters is CollidingChannelIsRejected: two channels that land on the SAME
// wire hash byte. A hash-only filter drops both by construction. This gate must drop only
// one, and that is the entire reason the design pays for an HMAC on the forwarding path.
#include <gtest/gtest.h>
#include "Utils.h"

using namespace mesh;

// Both are public hashtag names. Derived, not captured. `#bench89` was found by brute force
// (reference/chan_filter_vectors.py) precisely because it collides with `#test` on the wire
// hash byte -- that collision is asserted below rather than assumed.
static const char* CHAN_A = "#test";
static const char* CHAN_B = "#bench89";
static const uint8_t CHAN_A_WIRE_HASH = 0xD9;   // recorded from live traffic 2026-08-17

// Client convention: key = SHA256(name)[0:16], zero-padded to 32. The firmware has no name
// derivation of its own -- see FEATURE-fwd-channel-filter.md.
static void derive_channel(const char* name, uint8_t secret[32]) {
  memset(secret, 0, 32);
  Utils::sha256(secret, 16, (const uint8_t*) name, (int) strlen(name));
}

// setChannel picks 16 vs 32 bytes by testing whether bytes 16..31 are zero; a name-derived
// channel is always the 16-byte case.
static uint8_t wire_hash(const uint8_t secret[32]) {
  uint8_t h[32];
  Utils::sha256(h, sizeof(h), secret, 16);
  return h[0];
}

// A GRP_TXT payload as Mesh.cpp:227 reads it: [0] channel hash, [1..2] MAC, [3..] ciphertext.
struct GrpPacket {
  uint8_t payload[MAX_PACKET_PAYLOAD];
  int len;
};

static GrpPacket build_packet(const uint8_t secret[32], const char* text) {
  GrpPacket p;
  memset(&p, 0, sizeof(p));
  p.payload[0] = wire_hash(secret);
  int n = Utils::encryptThenMAC(secret, &p.payload[1], (const uint8_t*) text, (int) strlen(text));
  p.len = 1 + n;
  return p;
}

// The gate as it will sit in allowPacketForward(): cheap byte compare first, HMAC only on a hit.
static bool channel_matches(const uint8_t secret[32], uint8_t cached_hash, const GrpPacket& p) {
  if (p.len < 4) return false;                    // too short for hash + MAC + data
  if (p.payload[0] != cached_hash) return false;  // plain compare, no crypto
  return Utils::MACMatches(secret, &p.payload[1], p.len - 1);
}

class ChanFilter : public ::testing::Test {
protected:
  uint8_t secret_a[32], secret_b[32];
  uint8_t hash_a, hash_b;
  void SetUp() override {
    derive_channel(CHAN_A, secret_a);
    derive_channel(CHAN_B, secret_b);
    hash_a = wire_hash(secret_a);
    hash_b = wire_hash(secret_b);
  }
};

// If this fails, the derivation drifted from what the clients do and every other test here is
// testing a channel nobody uses.
TEST_F(ChanFilter, DerivationMatchesLiveTraffic) {
  EXPECT_EQ(hash_a, CHAN_A_WIRE_HASH);
  uint8_t padding_ok = 0;
  for (int i = 16; i < 32; i++) padding_ok |= secret_a[i];
  EXPECT_EQ(padding_ok, 0) << "a name-derived channel key is 16 bytes, zero-padded to 32";
}

// The premise of the whole feature: one byte is not enough to tell channels apart.
TEST_F(ChanFilter, TheTwoChannelsReallyCollide) {
  EXPECT_EQ(hash_a, hash_b) << "these names no longer collide -- pick a new one with "
                               "reference/chan_filter_vectors.py, the test is pointless without it";
  EXPECT_NE(0, memcmp(secret_a, secret_b, 32)) << "same key: that is not a collision";
}

TEST_F(ChanFilter, OwnChannelMatches) {
  GrpPacket p = build_packet(secret_a, "hello from the bench");
  EXPECT_TRUE(channel_matches(secret_a, hash_a, p));
}

// THE NEGATIVE CONTROL. Same wire hash byte, different channel -- must be forwarded.
TEST_F(ChanFilter, CollidingChannelIsRejected) {
  GrpPacket p = build_packet(secret_b, "traffic that is none of our business");
  ASSERT_EQ(p.payload[0], hash_a) << "precondition: the packet carries the blocked hash byte";
  EXPECT_TRUE(channel_matches(secret_b, hash_b, p)) << "B's own key must still recognise it";
  EXPECT_FALSE(channel_matches(secret_a, hash_a, p))
      << "blocking " << CHAN_A << " also caught " << CHAN_B << " -- the MAC gate buys nothing";
}

// Both polarities must be present, or "all tests pass" is compatible with a gate stuck at a
// constant. This is not hypothetical: an earlier version of this suite ran against a mocked
// SHA256 whose finalizeHMAC() wrote nothing, so the gate could only ever answer "no".
TEST_F(ChanFilter, GateIsNotStuckAtAConstant) {
  GrpPacket a = build_packet(secret_a, "one");
  GrpPacket b = build_packet(secret_b, "two");
  EXPECT_TRUE(channel_matches(secret_a, hash_a, a));
  EXPECT_FALSE(channel_matches(secret_a, hash_a, b));
}

TEST_F(ChanFilter, DetectsCorruptionAnywhereInTheCiphertext) {
  GrpPacket base = build_packet(secret_a, "a message long enough to span several AES blocks");
  ASSERT_TRUE(channel_matches(secret_a, hash_a, base));
  for (int i = 3; i < base.len; i++) {
    GrpPacket c = base;
    c.payload[i] ^= 0x01;
    EXPECT_FALSE(channel_matches(secret_a, hash_a, c))
        << "flipping ciphertext byte " << i << " still matched";
  }
}

TEST_F(ChanFilter, DetectsCorruptionInTheMAC) {
  GrpPacket base = build_packet(secret_a, "short");
  for (int i = 1; i <= 2; i++) {
    GrpPacket c = base;
    c.payload[i] ^= 0x01;
    EXPECT_FALSE(channel_matches(secret_a, hash_a, c))
        << "flipping MAC byte " << i << " still matched";
  }
}

TEST_F(ChanFilter, DependsOnEveryByteOfTheKey) {
  GrpPacket base = build_packet(secret_a, "keyed");
  for (int i = 0; i < 32; i++) {
    uint8_t k[32];
    memcpy(k, secret_a, 32);
    k[i] ^= 0x01;
    EXPECT_FALSE(Utils::MACMatches(k, &base.payload[1], base.len - 1))
        << "flipping secret byte " << i << " still matched";
  }
}

// Short and degenerate inputs must be rejected without reading past the buffer.
TEST_F(ChanFilter, RejectsShortInputs) {
  uint8_t secret[32] = {0};
  uint8_t buf[4] = {0xD9, 0x00, 0x00, 0x00};
  EXPECT_FALSE(Utils::MACMatches(secret, buf, -1));
  EXPECT_FALSE(Utils::MACMatches(secret, buf, 0));
  EXPECT_FALSE(Utils::MACMatches(secret, buf, 1));
  EXPECT_FALSE(Utils::MACMatches(secret, buf, 2));   // exactly the MAC, no ciphertext
}

// MACMatches is the first half of MACThenDecrypt and must agree with it -- otherwise the
// filter and the receive path could disagree about what a packet is.
TEST_F(ChanFilter, AgreesWithMACThenDecrypt) {
  GrpPacket a = build_packet(secret_a, "agreement");
  GrpPacket b = build_packet(secret_b, "disagreement");
  uint8_t dest[MAX_PACKET_PAYLOAD];
  for (const GrpPacket* p : {&a, &b}) {
    for (const uint8_t* k : {secret_a, secret_b}) {
      int len = Utils::MACThenDecrypt(k, dest, &p->payload[1], p->len - 1);
      EXPECT_EQ(len > 0, Utils::MACMatches(k, &p->payload[1], p->len - 1));
    }
  }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
