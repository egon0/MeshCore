#include "FwdPrefs.h"
#include <string.h>

// ---- TLV write helpers ----------------------------------------------------

static void fwd_write_tlv(File& f, uint8_t tag, const uint8_t* val, uint16_t len) {
  f.write(&tag, 1);
  uint8_t ln[2] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };  // u16 little-endian
  f.write(ln, 2);
  if (len) f.write(val, len);
}

static inline void fwd_write_u8(File& f, uint8_t tag, uint8_t val) {
  fwd_write_tlv(f, tag, &val, 1);
}

// ---- TLV read helpers -----------------------------------------------------

static void fwd_skip(File& f, uint16_t len) {
  uint8_t tmp[32];
  while (len) {
    uint16_t n = len > sizeof(tmp) ? (uint16_t)sizeof(tmp) : len;
    if (f.read(tmp, n) != (int)n) return;
    len -= n;
  }
}

// scalar: copy iff exactly one byte, else skip and keep the default
static void fwd_read_u8(File& f, uint16_t len, uint8_t* dst) {
  if (len == 1) { f.read(dst, 1); }
  else          { fwd_skip(f, len); }
}

// blob: copy iff it fits the destination, else skip and keep the default (treat as corrupt)
static void fwd_read_blob(File& f, uint16_t len, uint8_t* dst, uint16_t cap) {
  if (len <= cap) { if (len) f.read(dst, len); }
  else            { fwd_skip(f, len); }
}

// ---- FwdPrefs -------------------------------------------------------------

void FwdPrefs::reset() {
  hashfilter_mode = 0;
  hashfilter_prob = 100;
  block_count = 0;
  memset(block_keys, 0, sizeof(block_keys));
  memset(block_actions, 0, sizeof(block_actions));
  whitelist_mode = 0;
  whitelist_zerohop = 1;     // default: allow 0-hop floods
  whitelist_count = 0;
  memset(whitelist_keys, 0, sizeof(whitelist_keys));
  flood_max_request = 64;
  flood_max_anon_request = 64;
  flood_max_response = 64;
}

void FwdPrefs::sanitise() {
  if (hashfilter_mode > 2) hashfilter_mode = 0;
  if (hashfilter_prob > 100) hashfilter_prob = 100;
  if (block_count > FWD_BLOCK_MAX) block_count = 0;          // corrupt -> drop table
  if (whitelist_mode > 1) whitelist_mode = 0;
  if (whitelist_zerohop > 1) whitelist_zerohop = 1;
  if (whitelist_count > FWD_WL_MAX) whitelist_count = 0;     // corrupt -> drop table
  if (flood_max_request > 64) flood_max_request = 64;
  if (flood_max_anon_request > 64) flood_max_anon_request = 64;
  if (flood_max_response > 64) flood_max_response = 64;
}

void FwdPrefs::load(FILESYSTEM* fs) {
  reset();   // defaults preserved for any tag absent from the file
#if defined(RP2040_PLATFORM)
  File file = fs->open("/fwd_prefs", "r");
#else
  File file = fs->open("/fwd_prefs");
#endif
  if (!file) return;   // absent (fresh / pre-feature node) -> defaults

  uint8_t hdr[5];
  if (file.read(hdr, 5) != 5
      || hdr[0] != FWDPREFS_MAGIC0 || hdr[1] != FWDPREFS_MAGIC1
      || hdr[2] != FWDPREFS_MAGIC2 || hdr[3] != FWDPREFS_MAGIC3) {
    file.close();
    return;            // not our file / truncated header -> keep defaults
  }
  // hdr[4] = format version. v1 is the only grammar; future versions still parse
  // known tags and skip unknown ones, so no hard gate is needed here.

  uint8_t tag;
  while (file.read(&tag, 1) == 1) {
    uint8_t ln[2];
    if (file.read(ln, 2) != 2) break;   // truncated length -> stop
    uint16_t len = (uint16_t)ln[0] | ((uint16_t)ln[1] << 8);
    switch (tag) {
      case FWD_TAG_HF_MODE:         fwd_read_u8(file, len, &hashfilter_mode); break;
      case FWD_TAG_HF_PROB:         fwd_read_u8(file, len, &hashfilter_prob); break;
      case FWD_TAG_BL_COUNT:        fwd_read_u8(file, len, &block_count); break;
      case FWD_TAG_BL_KEYS:         fwd_read_blob(file, len, (uint8_t*)block_keys, sizeof(block_keys)); break;
      case FWD_TAG_BL_ACTIONS:      fwd_read_blob(file, len, block_actions, sizeof(block_actions)); break;
      case FWD_TAG_WL_MODE:         fwd_read_u8(file, len, &whitelist_mode); break;
      case FWD_TAG_WL_ZEROHOP:      fwd_read_u8(file, len, &whitelist_zerohop); break;
      case FWD_TAG_WL_COUNT:        fwd_read_u8(file, len, &whitelist_count); break;
      case FWD_TAG_WL_KEYS:         fwd_read_blob(file, len, (uint8_t*)whitelist_keys, sizeof(whitelist_keys)); break;
      case FWD_TAG_FM_REQUEST:      fwd_read_u8(file, len, &flood_max_request); break;
      case FWD_TAG_FM_ANON_REQUEST: fwd_read_u8(file, len, &flood_max_anon_request); break;
      case FWD_TAG_FM_RESPONSE:     fwd_read_u8(file, len, &flood_max_response); break;
      default:                      fwd_skip(file, len); break;   // unknown tag -> forward-compat skip
    }
  }
  file.close();
  sanitise();
}

void FwdPrefs::save(FILESYSTEM* fs) const {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove("/fwd_prefs");
  File file = fs->open("/fwd_prefs", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  File file = fs->open("/fwd_prefs", "w");
#else
  File file = fs->open("/fwd_prefs", "w", true);
#endif
  if (!file) return;

  const uint8_t hdr[5] = {
    FWDPREFS_MAGIC0, FWDPREFS_MAGIC1, FWDPREFS_MAGIC2, FWDPREFS_MAGIC3, FWDPREFS_FORMAT_VER
  };
  file.write(hdr, 5);

  fwd_write_u8(file, FWD_TAG_HF_MODE, hashfilter_mode);
  fwd_write_u8(file, FWD_TAG_HF_PROB, hashfilter_prob);

  fwd_write_u8(file, FWD_TAG_BL_COUNT, block_count);
  fwd_write_tlv(file, FWD_TAG_BL_KEYS, (const uint8_t*)block_keys, (uint16_t)block_count * FWD_KEY_SIZE);
  fwd_write_tlv(file, FWD_TAG_BL_ACTIONS, block_actions, (uint16_t)block_count);

  fwd_write_u8(file, FWD_TAG_WL_MODE, whitelist_mode);
  fwd_write_u8(file, FWD_TAG_WL_ZEROHOP, whitelist_zerohop);
  fwd_write_u8(file, FWD_TAG_WL_COUNT, whitelist_count);
  fwd_write_tlv(file, FWD_TAG_WL_KEYS, (const uint8_t*)whitelist_keys, (uint16_t)whitelist_count * FWD_KEY_SIZE);

  fwd_write_u8(file, FWD_TAG_FM_REQUEST, flood_max_request);
  fwd_write_u8(file, FWD_TAG_FM_ANON_REQUEST, flood_max_anon_request);
  fwd_write_u8(file, FWD_TAG_FM_RESPONSE, flood_max_response);

  file.close();
}
