#pragma once

#include <Utils.h>

// ---------------------------------------------------------------------------
// Fork-private: channel blocklist (forward-filter Stage 5).
//
// Header-only and free of any node state, so the forwarding decision can be
// unit-tested natively -- same shape as mainline's RoutingPolicy.h.
//
// A group packet names its channel only by payload[0], a ONE-byte hash. In the
// live AT mesh 244 of 256 values are occupied, so matching that byte drops
// unrelated channels as collateral. Instead the operator stores the channel
// secret and we verify the packet's 2-byte MAC under it: exact, and impossible
// to fake without the key.
//
// Nothing here decrypts. matchesChannel() is Utils::MACThenDecrypt() minus the
// decrypt half (it calls the same Utils::MACMatches()), so a repeater can name
// the channel it drops while remaining unable to read a message. That is a
// property of the code -- there is no decrypt() call on this path -- rather
// than a promise in a document.
// ---------------------------------------------------------------------------

// 16, matching FWD_BLOCK_MAX and FWD_WL_MAX. Sized from the use case the feature is argued on:
// an Austrian repeater was measured forwarding TEN channels with no local receiver (#hungary,
// #slovakia, #kosice, #switzerland, #polska, #turiec, #poland, #yo, #australia, #slovenia), so a
// table of 8 could not express it. RAM is not the constraint -- 16 entries cost 784 B on a node
// using 15 % of its RAM -- and neither is CPU: with entries on distinct hash bytes a packet can
// match at most one, so the per-packet cost does not depend on this number at all.
#define FWD_CHAN_MAX          16
#define FWD_CHAN_LABEL_LEN    16   // display only; "" for a raw-key entry

// MeshCore's built-in default group channel. It has no `#name` to derive from, so without this
// constant an operator would have to paste a hex string for the single most common channel there is.
// No secret: the same 16 bytes are in examples/companion_radio/MyMesh.cpp (PUBLIC_GROUP_PSK, as
// base64) and in docs/faq.md, and every companion in the mesh ships with them.
#define FWD_CHAN_PUBLIC_NAME  "Public"
#define FWD_CHAN_PUBLIC_PSK   { 0x8B, 0x33, 0x87, 0xE9, 0xC5, 0xCD, 0xEA, 0x6A, \
                                0xC9, 0xE5, 0xED, 0xBA, 0xA1, 0x15, 0xCD, 0x72 }

namespace mesh {

// GRP_TXT / GRP_DATA payload layout, per Mesh.cpp:227 --
//   [0]      channel hash
//   [1..2]   MAC        (CIPHER_MAC_SIZE)
//   [3..]    ciphertext
#define CHAN_PAYLOAD_MIN_LEN  4   // hash + MAC + at least one ciphertext byte

/**
 * \brief  Channel secret for a public "#name" channel, as the clients derive it.
 *
 * SHA256(name)[0:16], zero-padded to 32. The '#' is part of the string and the name is
 * case-sensitive: '#ping' and '#Ping' are different channels. The firmware itself has no
 * name derivation -- addChannel() takes a raw PSK -- so this mirrors a *client* convention,
 * verified against live traffic (see FEATURE-fwd-channel-filter.md).
 */
inline void deriveChannelKey(const char* name, uint8_t dest[PUB_KEY_SIZE]) {
  memset(dest, 0, PUB_KEY_SIZE);
  Utils::sha256(dest, 16, (const uint8_t*) name, (int) strlen(name));
}

/**
 * \brief  The byte that appears on the wire as payload[0] for this channel.
 *
 * setChannel() picks the hashed length by testing whether bytes 16..31 are zero, so a
 * name-derived (128-bit) channel hashes 16 bytes and a full 256-bit PSK hashes 32.
 * Getting this wrong yields a hash that never matches anything on air.
 */
inline uint8_t channelWireHash(const uint8_t key[PUB_KEY_SIZE]) {
  int klen = 16;
  for (int i = 16; i < PUB_KEY_SIZE; i++) {
    if (key[i]) { klen = PUB_KEY_SIZE; break; }
  }
  uint8_t h[PUB_KEY_SIZE];
  Utils::sha256(h, sizeof(h), key, klen);
  return h[0];
}

/**
 * \brief  Does this group payload belong to the channel held in 'key'?
 * \param  cached_hash  channelWireHash(key), computed once when the operator configured it
 * \returns  true only if the MAC verifies -- a matching hash byte alone is not enough
 */
inline bool matchesChannel(const uint8_t* payload, int payload_len,
                           const uint8_t key[PUB_KEY_SIZE], uint8_t cached_hash) {
  if (payload_len < CHAN_PAYLOAD_MIN_LEN) return false;
  if (payload[0] != cached_hash) return false;   // cheap reject, no crypto
  return Utils::MACMatches(key, &payload[1], payload_len - 1);
}

/**
 * \brief  First blocked channel this payload belongs to, or -1.
 *
 * Cost is one byte compare per configured channel; the HMAC runs only for the entries whose
 * hash byte already matched, which for a handful of channels is rare.
 */
inline int findBlockedChannel(const uint8_t* payload, int payload_len,
                              const uint8_t keys[][PUB_KEY_SIZE], const uint8_t* hashes, int count) {
  if (payload_len < CHAN_PAYLOAD_MIN_LEN) return -1;
  for (int i = 0; i < count; i++) {
    if (payload[0] != hashes[i]) continue;       // cheap reject first
    if (Utils::MACMatches(keys[i], &payload[1], payload_len - 1)) return i;
  }
  return -1;
}

}
