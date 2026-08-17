#pragma once

#include <Arduino.h>               // uint8_t / uint16_t
#include <Mesh.h>
#include <helpers/IdentityStore.h> // defines FILESYSTEM + the platform File type (same chain CommonCLI.h uses)
#include <helpers/ChannelFilter.h> // FWD_CHAN_MAX / FWD_CHAN_LABEL_LEN live with the matching logic

// ---------------------------------------------------------------------------
// Fork-private forward-filter preferences.
//
// These fields are intentionally kept OUT of NodePrefs / "/com_prefs" so that
// "/com_prefs" tracks mainline MeshCore byte-for-byte and never collides with
// upstream's positional serialization. See FEATURE-fwdprefs-tlv.md for the full
// rationale. Persisted to its own self-describing TLV file, "/fwd_prefs":
//
//   Header:  "FWDP" (4 B magic) | format_version u8 (=1)
//   Record:  tag u8 | len u16 LE | value[len]   (repeated to EOF)
//
// Unknown tag -> skipped (forward-compat); missing tag -> default (back-compat).
// Tables store only their `count` active entries (value len = count * 32).
// ---------------------------------------------------------------------------

#define FWD_BLOCK_MAX          16
#define FWD_WL_MAX             16
#define FWD_KEY_SIZE           32     // = PUB_KEY_SIZE

// fwd_block_actions bit flags
#define FWD_BLOCK_PRUNE_PATH   0x01   // drop flood copies whose path contains this node (filterRecvFloodPacket)
#define FWD_BLOCK_DROP_ADVERT  0x02   // do not forward adverts originated by this node (allowPacketForward)

// "/fwd_prefs" wire format
#define FWDPREFS_MAGIC0  'F'
#define FWDPREFS_MAGIC1  'W'
#define FWDPREFS_MAGIC2  'D'
#define FWDPREFS_MAGIC3  'P'
#define FWDPREFS_FORMAT_VER  1

// TLV tags (fork-owned id space; high/unique so they never alias an upstream concept)
#define FWD_TAG_HF_MODE          0x10
#define FWD_TAG_HF_PROB          0x11
#define FWD_TAG_BL_COUNT         0x20
#define FWD_TAG_BL_KEYS          0x21
#define FWD_TAG_BL_ACTIONS       0x22
#define FWD_TAG_WL_MODE          0x30
#define FWD_TAG_WL_ZEROHOP       0x31
#define FWD_TAG_WL_COUNT         0x32
#define FWD_TAG_WL_KEYS          0x33
#define FWD_TAG_FM_REQUEST       0x40
#define FWD_TAG_FM_ANON_REQUEST  0x41
#define FWD_TAG_FM_RESPONSE      0x42
#define FWD_TAG_SCOPED_RESERVE   0x50
#define FWD_TAG_CHAN_COUNT       0x60
#define FWD_TAG_CHAN_KEYS        0x61
#define FWD_TAG_CHAN_HASH        0x62
#define FWD_TAG_CHAN_LABEL       0x63

struct FwdPrefs {
  // -- hash-size filter (Stage 1) --
  uint8_t hashfilter_mode;        // 0 = off, 1 = adverts only, 2 = all flood/direct traffic
  uint8_t hashfilter_prob;        // 0..100 = % chance to drop a matched 1-byte packet (100 = always)

  // -- per-pubkey blacklist (Stage 2) --
  uint8_t block_count;                            // active entries (0..FWD_BLOCK_MAX)
  uint8_t block_keys[FWD_BLOCK_MAX][FWD_KEY_SIZE];
  uint8_t block_actions[FWD_BLOCK_MAX];

  // -- last-hop whitelist (Stage 3) --
  uint8_t whitelist_mode;         // 0 = off, 1 = on
  uint8_t whitelist_zerohop;      // 0 = drop 0-hop floods, 1 = allow (default)
  uint8_t whitelist_count;        // active entries (0..FWD_WL_MAX)
  uint8_t whitelist_keys[FWD_WL_MAX][FWD_KEY_SIZE];

  // -- per-payload flood hop caps (ported from meshcore-dev PR #2797; fork-private until/unless merged) --
  uint8_t flood_max_request;      // REQ
  uint8_t flood_max_anon_request; // ANON_REQ
  uint8_t flood_max_response;     // RESPONSE

  // -- airtime reserve (Stage 4): drop unscoped floods under TX-budget pressure so scoped keeps its slice --
  uint8_t scoped_reserve_pct;     // 0..100 = % of TX airtime budget reserved for scoped-only (0 = off/default)

  // -- channel blocklist (Stage 5): drop group traffic whose MAC verifies under a stored channel key --
  uint8_t chan_count;                             // active entries (0..FWD_CHAN_MAX); 0 = off/default
  uint8_t chan_keys[FWD_CHAN_MAX][FWD_KEY_SIZE];  // channel secret, as GroupChannel::secret (16 B zero-padded to 32)
  uint8_t chan_hash[FWD_CHAN_MAX];                // channelWireHash(key), cached so the hot path never hashes
  char    chan_label[FWD_CHAN_MAX][FWD_CHAN_LABEL_LEN];  // display only; "" for a raw-key entry

  void reset();                   // set all fields to defaults (off / empty / cap 64)
  void sanitise();                // clamp out-of-range values after a load
  void load(FILESYSTEM* fs);      // read "/fwd_prefs" (resets to defaults first; absent -> defaults)
  void save(FILESYSTEM* fs) const;// (re)write "/fwd_prefs"
};
