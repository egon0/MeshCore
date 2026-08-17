#include "MyMesh.h"
#include <algorithm>

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
      break;
  }
}

unsigned long MyMesh::airWindowUsed() {
  unsigned long now = _ms->getMillis();
  // Tumbling window: on expiry, re-base against the running TX-airtime total rather than keeping a
  // ring of samples. One window of lag is immaterial against the hour-scale bucket this replaces.
  if (air_win_start == 0 || (now - air_win_start) >= FWD_AIR_WINDOW_MS) {
    air_win_start = now;
    air_win_base = getTotalAirTime();
  }
  unsigned long total = getTotalAirTime();
  return (total >= air_win_base) ? (total - air_win_base) : 0;   // guard the counter reset on reboot
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  // [fwd-filter Stage 1] Net-health: throttle forwarding of unidentifiable 1-byte path-hash traffic
  // (1-byte hashes collide in a 256-value space, so large networks can't attribute them; pushes
  // nodes to multibyte). Two-step: (1) decide if the packet is MATCHED by the mode, then (2) drop a
  // matched packet with `prob`% chance. hashfilter_mode: 0=off, 1=adverts only, 2=all. `prob` applies
  // in BOTH modes (adverts are probabilistic too, not auto-dropped); default prob=100 => every match
  // dropped, lower it to thin. (Explicit match/drop split for readability -- equivalent to the old
  // `(mode==2 || is_advert) && rand<prob`, which read as if adverts bypassed the mode.)
  if (_fwd_prefs.hashfilter_mode != 0 && packet->getPathHashSize() == 1) {
    bool is_advert = packet->getPayloadType() == PAYLOAD_TYPE_ADVERT;
    bool matched = false;
    if (_fwd_prefs.hashfilter_mode == 1) {         // adverts only
      matched = is_advert;
    } else if (_fwd_prefs.hashfilter_mode == 2) {  // all 1-byte traffic
      matched = true;
    }
    // (no else: the outer guard excludes mode 0 and FwdPrefs::sanitise() clamps >2 to 0, so mode is 1 or 2.)
    if (matched && (int)(rand() % 100) < _fwd_prefs.hashfilter_prob) {
      MESH_DEBUG_PRINTLN("fwd-filter: drop 1-byte %s (hashfilter mode=%d prob=%d)",
                         is_advert ? "advert" : "pkt", (int)_fwd_prefs.hashfilter_mode,
                         (int)_fwd_prefs.hashfilter_prob);
      return false;
    }
  }
  // [fwd-filter Stage 2] Suppress adverts originated by a blacklisted node (DROP_ADVERT), at any
  // hash size. An advert's payload begins with the originator's full pub_key -> exact identity match.
  if (_fwd_prefs.block_count > 0 && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT
      && packet->payload_len >= PUB_KEY_SIZE) {
    for (uint8_t k = 0; k < _fwd_prefs.block_count; k++) {
      if ((_fwd_prefs.block_actions[k] & FWD_BLOCK_DROP_ADVERT)
          && memcmp(packet->payload, _fwd_prefs.block_keys[k], PUB_KEY_SIZE) == 0) {
        MESH_DEBUG_PRINTLN("fwd-filter: drop advert from blocklisted node (entry %d)", (int)k);
        return false;
      }
    }
  }
  if (packet->isRouteFlood()) {
    // Upstream 1.17.1 (fad11c90) folded the three standard caps into this helper. It is
    // behaviour-identical to the three lines it replaced, so our per-payload caps stay beside it
    // rather than being merged into it -- keeping the fork's addition visible as an addition.
    if (mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped,
                                      _prefs.flood_max_advert)) return false;
    // [fwd-filter / PR #2797] per-payload flood hop caps (default 64 = no-op)
    if (packet->getPayloadType() == PAYLOAD_TYPE_REQ && packet->getPathHashCount() >= _fwd_prefs.flood_max_request) return false;
    if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ && packet->getPathHashCount() >= _fwd_prefs.flood_max_anon_request) return false;
    if (packet->getPayloadType() == PAYLOAD_TYPE_RESPONSE && packet->getPathHashCount() >= _fwd_prefs.flood_max_response) return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  // [fwd-filter Stage 5] Channel blocklist: drop group traffic belonging to a channel the operator
  // configured. Identified by verifying the packet's MAC under the stored channel secret -- the
  // 1-byte wire hash alone is not selective (244 of 256 values occupied in the live mesh), so a
  // hash-only filter would drop unrelated channels. Nothing on this path decrypts; see
  // helpers/ChannelFilter.h. chan_count = 0 => no-op (default), and the byte compare rejects
  // almost everything before any HMAC runs.
  if (_fwd_prefs.chan_count > 0 && packet->isRouteFlood()
      && (packet->getPayloadType() == PAYLOAD_TYPE_GRP_TXT
          || packet->getPayloadType() == PAYLOAD_TYPE_GRP_DATA)) {
    int idx = mesh::findBlockedChannel(packet->payload, packet->payload_len,
                                       _fwd_prefs.chan_keys, _fwd_prefs.chan_hash,
                                       _fwd_prefs.chan_count);
    if (idx >= 0) {
      n_drop_chan++;
      airtime_saved_chan += _radio->getEstAirtimeFor(packet->getRawLength());
      MESH_DEBUG_PRINTLN("fwd-filter: drop group packet on blocked channel %s (hash %02X)",
                         _fwd_prefs.chan_label[idx][0] ? _fwd_prefs.chan_label[idx] : "(raw key)",
                         (uint32_t)_fwd_prefs.chan_hash[idx]);
      return false;
    }
  }
  // [fwd-filter Stage 4] Airtime reserve: under sustained TX-budget pressure, drop UNSCOPED floods so the
  // reserved slice of the duty-cycle budget stays free for scoped delivery. Scoped floods (non-wildcard
  // region) and direct traffic bypass. scoped_reserve_pct=0 => no-op (default). Counts the forward/drop mix.
  // (recv_pkt_region is set by filterRecvFloodPacket() before this; NULL was already dropped above.)
  if (packet->isRouteFlood() && recv_pkt_region != NULL) {
    if (recv_pkt_region->isWildcard()) {                 // unscoped flood (wildcard region, flood-allowed)
      if (_fwd_prefs.scoped_reserve_pct > 0) {
        // Measured over FWD_AIR_WINDOW_MS, NOT the Dispatcher's one-hour duty-cycle bucket. The
        // bucket is a regulatory accumulator whose slack (360000 ms at a legal 10% duty) swallows
        // any realistic burst, so a percentage of it could only trip at 100%. The short window
        // carries the same duty-cycle-derived allowance over a span where a burst is visible.
        unsigned long win_max = airWindowMax();
        unsigned long used = airWindowUsed();
        unsigned long remaining = (win_max > used) ? (win_max - used) : 0;
        unsigned long reserve_ms = (unsigned long)((uint64_t)win_max * _fwd_prefs.scoped_reserve_pct / 100);
        uint32_t est = _radio->getEstAirtimeFor(packet->getRawLength());
        if (remaining < reserve_ms + est) {
          n_drop_unscoped++; airtime_saved_unscoped += est;
          MESH_DEBUG_PRINTLN("fwd-filter: drop unscoped flood (reserve %d%%, air %lu/%lums used, remaining=%lu reserve=%lu)",
                             (int)_fwd_prefs.scoped_reserve_pct, used, win_max, remaining, reserve_ms);
          return false;
        }
      }
      n_fwd_unscoped++;
    } else {
      n_fwd_scoped++;                                     // scoped flood -- always allowed by this gate
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
}

// Fork-private forward filter. Up to 1.16 mainline overrode filterRecvFloodPacket() purely to work
// out recv_pkt_region and our stages were appended to it; 1.17 moved that into MyMesh::onRecvPacket()
// above and dropped the override, so we re-add it here carrying the fork stages only. Mesh calls this
// for FLOOD packets only, and before the dedup table is marked -- returning true drops this copy
// without marking it seen, so a copy arriving via a different path can still win.
bool MyMesh::filterRecvFloodPacket(mesh::Packet* pkt) {
  // [fwd-filter Stage 2] Prune flood copies whose path traverses a blacklisted node (PRUNE_IF_IN_PATH).
  // Called before hasSeen(): returning true drops THIS copy without marking it seen, so a copy via a
  // different/better path can still win. Path entries are hash prefixes, reliable only at multibyte
  // sizes (1-byte handled broadly by the hash-size filter).
  if (_fwd_prefs.block_count > 0) {
    uint8_t sz = pkt->getPathHashSize();
    uint8_t n = pkt->getPathHashCount();
    for (uint8_t h = 0; h < n; h++) {
      const uint8_t* hop = &pkt->path[h * sz];
      for (uint8_t k = 0; k < _fwd_prefs.block_count; k++) {
        if ((_fwd_prefs.block_actions[k] & FWD_BLOCK_PRUNE_PATH)
            && memcmp(hop, _fwd_prefs.block_keys[k], sz) == 0) {
          MESH_DEBUG_PRINTLN("fwd-filter: prune %d-byte flood via blocklisted hop (hop %d/%d entry %d)",
                             (int)sz, (int)h, (int)n, (int)k);
          return true;  // drop this copy; not marked seen
        }
      }
    }
  }
  // [fwd-filter Stage 3] Last-hop WHITELIST: only relay a flood if its immediate sender (the last path
  // hop) is allow-listed. Exemptions prevent admin lockout: adverts (neighbour learning), ANON_REQ
  // (login/initial contact), and floods addressed to this node always pass. Matched at the packet's
  // hash size (collision-prone at 1-byte -- operator should also enable `fwd.hashfilter all`).
  if (_fwd_prefs.whitelist_mode != 0) {
    uint8_t ptype = pkt->getPayloadType();
    if (ptype == PAYLOAD_TYPE_ADVERT || ptype == PAYLOAD_TYPE_ANON_REQ) return false;  // exempt
    if ((ptype == PAYLOAD_TYPE_TXT_MSG || ptype == PAYLOAD_TYPE_REQ
         || ptype == PAYLOAD_TYPE_RESPONSE || ptype == PAYLOAD_TYPE_PATH)
        && pkt->payload_len >= 1 && self_id.isHashMatch(&pkt->payload[0])) return false;  // for us
    uint8_t n = pkt->getPathHashCount();
    if (n == 0) {   // 0-hop flood: heard directly, originator not identifiable
      if (_fwd_prefs.whitelist_zerohop) return false;   // allow
      MESH_DEBUG_PRINTLN("fwd-filter: drop 0-hop flood (whitelist, 0hop=drop)");
      return true;                                      // drop
    }
    uint8_t sz = pkt->getPathHashSize();
    const uint8_t* last = &pkt->path[(n - 1) * sz];   // immediate sender = last appended hop
    for (uint8_t k = 0; k < _fwd_prefs.whitelist_count; k++) {
      if (memcmp(last, _fwd_prefs.whitelist_keys[k], sz) == 0) return false;  // whitelisted -> relay
    }
    MESH_DEBUG_PRINTLN("fwd-filter: drop %d-byte flood, last-hop not whitelisted (%d entries)",
                       (int)sz, (int)_fwd_prefs.whitelist_count);
    return true;  // last hop not whitelisted -> drop (not marked seen)
  }
  // do normal processing
  return false;
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = 0xFF;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    // a DIRECT login can reply via the stored out_path, as onPeerDataRecv() does for REQ
    ClientInfo* client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool have_out_path = client != NULL && client->out_path_len != OUT_PATH_UNKNOWN;

    auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), reply_path_len != 0xFF, have_out_path);

    if (route == mesh::REPLY_ROUTE_PATH_RETURN) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      return;
    }

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
    if (reply == NULL) return;

    if (route == mesh::REPLY_ROUTE_DIRECT_SUPPLIED) {
      sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    } else if (route == mesh::REPLY_ROUTE_DIRECT_OUT_PATH) {
      sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
    }
  }
}

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      } else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
  recv_pkt_region = NULL;
  n_fwd_scoped = n_fwd_unscoped = n_drop_unscoped = 0;
  airtime_saved_unscoped = 0;
  n_drop_chan = 0;
  airtime_saved_chan = 0;
  air_win_start = air_win_base = 0;

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  _fwd_prefs.load(_fs);   // fork-private forward-filter prefs from /fwd_prefs (independent of /com_prefs)
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

#if defined(USE_LR2021)
bool MyMesh::configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) {
  return radio_driver.configSideDetectors(sideDetSFs, num, bw);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
  } else if (handleFwdCommand(command, reply)) {
    // fork-private forward-filter command (fwd.* / flood.max.*request|response) handled in-place;
    // intercepted here so it works over BOTH serial and RF admin without touching CommonCLI.
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

// Handles the fork-private forward-filter CLI surface. Returns true (and fills `reply`) iff `command`
// is one of ours; returns false (leaving `reply` untouched) so non-fwd commands fall through to
// CommonCLI. Operates on _fwd_prefs and persists to /fwd_prefs only -- never touches /com_prefs.
bool MyMesh::handleFwdCommand(char* command, char* reply) {
  if (memcmp(command, "set ", 4) == 0) {
    char* config = command + 4;
    if (memcmp(config, "fwd.hashfilter.prob ", 20) == 0) {
      _fwd_prefs.hashfilter_prob = constrain(atoi(&config[20]), 0, 100);
      _fwd_prefs.save(_fs); strcpy(reply, "OK");
    } else if (memcmp(config, "fwd.hashfilter ", 15) == 0) {
      config += 15;
      if (memcmp(config, "off", 3) == 0) { _fwd_prefs.hashfilter_mode = 0; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else if (memcmp(config, "advert", 6) == 0) { _fwd_prefs.hashfilter_mode = 1; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else if (memcmp(config, "all", 3) == 0) { _fwd_prefs.hashfilter_mode = 2; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, must be off, advert, or all");
    } else if (memcmp(config, "fwd.block.add ", 14) == 0) {
      char buf[160];
      strncpy(buf, &config[14], sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
      uint8_t actions = FWD_BLOCK_PRUNE_PATH;   // default: path-prune (steering)
      char* sp = strchr(buf, ' ');
      if (sp) {
        *sp = 0;
        const char* fl = sp + 1;
        if (memcmp(fl, "both", 4) == 0) actions = FWD_BLOCK_PRUNE_PATH | FWD_BLOCK_DROP_ADVERT;
        else if (memcmp(fl, "advert", 6) == 0) actions = FWD_BLOCK_DROP_ADVERT;
        else if (memcmp(fl, "prune", 5) == 0) actions = FWD_BLOCK_PRUNE_PATH;
      }
      uint8_t key[PUB_KEY_SIZE];
      if (strlen(buf) != PUB_KEY_SIZE * 2 || !mesh::Utils::fromHex(key, PUB_KEY_SIZE, buf)) {
        strcpy(reply, "Error: need 64-hex pubkey [prune|advert|both]");
      } else if (_fwd_prefs.block_count >= FWD_BLOCK_MAX) {
        strcpy(reply, "Error: table full");
      } else {
        int idx = -1;
        for (int k = 0; k < _fwd_prefs.block_count; k++)
          if (memcmp(_fwd_prefs.block_keys[k], key, PUB_KEY_SIZE) == 0) { idx = k; break; }
        if (idx < 0) { idx = _fwd_prefs.block_count++; memcpy(_fwd_prefs.block_keys[idx], key, PUB_KEY_SIZE); }
        _fwd_prefs.block_actions[idx] = actions;
        _fwd_prefs.save(_fs);
        strcpy(reply, "OK");
      }
    } else if (memcmp(config, "fwd.block.del ", 14) == 0) {
      const char* hex = &config[14];
      uint8_t key[PUB_KEY_SIZE];
      int klen = min((int)strlen(hex), PUB_KEY_SIZE * 2) / 2;
      int removed = 0;
      if (klen >= 1 && mesh::Utils::fromHex(key, klen, hex)) {
        for (int k = 0; k < _fwd_prefs.block_count; ) {
          if (memcmp(_fwd_prefs.block_keys[k], key, klen) == 0) {
            for (int j = k; j < _fwd_prefs.block_count - 1; j++) {
              memcpy(_fwd_prefs.block_keys[j], _fwd_prefs.block_keys[j + 1], PUB_KEY_SIZE);
              _fwd_prefs.block_actions[j] = _fwd_prefs.block_actions[j + 1];
            }
            _fwd_prefs.block_count--; removed++;
          } else k++;
        }
      }
      _fwd_prefs.save(_fs);
      sprintf(reply, "OK (%d removed)", removed);
    } else if (memcmp(config, "fwd.block.clear", 15) == 0) {
      _fwd_prefs.block_count = 0;
      _fwd_prefs.save(_fs);
      strcpy(reply, "OK");
    } else if (memcmp(config, "fwd.whitelist.0hop ", 19) == 0) {
      config += 19;
      if (memcmp(config, "allow", 5) == 0) { _fwd_prefs.whitelist_zerohop = 1; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else if (memcmp(config, "drop", 4) == 0) { _fwd_prefs.whitelist_zerohop = 0; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, must be allow or drop");
    } else if (memcmp(config, "fwd.whitelist.add ", 18) == 0) {
      const char* hex = &config[18];
      uint8_t key[PUB_KEY_SIZE];
      if (strlen(hex) != PUB_KEY_SIZE * 2 || !mesh::Utils::fromHex(key, PUB_KEY_SIZE, hex)) {
        strcpy(reply, "Error: need 64-hex pubkey");
      } else if (_fwd_prefs.whitelist_count >= FWD_WL_MAX) {
        strcpy(reply, "Error: table full");
      } else {
        int idx = -1;
        for (int k = 0; k < _fwd_prefs.whitelist_count; k++)
          if (memcmp(_fwd_prefs.whitelist_keys[k], key, PUB_KEY_SIZE) == 0) { idx = k; break; }
        if (idx < 0) { idx = _fwd_prefs.whitelist_count++; memcpy(_fwd_prefs.whitelist_keys[idx], key, PUB_KEY_SIZE); }
        _fwd_prefs.save(_fs);
        strcpy(reply, "OK");
      }
    } else if (memcmp(config, "fwd.whitelist.del ", 18) == 0) {
      const char* hex = &config[18];
      uint8_t key[PUB_KEY_SIZE];
      int klen = min((int)strlen(hex), PUB_KEY_SIZE * 2) / 2;
      int removed = 0;
      if (klen >= 1 && mesh::Utils::fromHex(key, klen, hex)) {
        for (int k = 0; k < _fwd_prefs.whitelist_count; ) {
          if (memcmp(_fwd_prefs.whitelist_keys[k], key, klen) == 0) {
            for (int j = k; j < _fwd_prefs.whitelist_count - 1; j++)
              memcpy(_fwd_prefs.whitelist_keys[j], _fwd_prefs.whitelist_keys[j + 1], PUB_KEY_SIZE);
            _fwd_prefs.whitelist_count--; removed++;
          } else k++;
        }
      }
      _fwd_prefs.save(_fs);
      sprintf(reply, "OK (%d removed)", removed);
    } else if (memcmp(config, "fwd.whitelist.clear", 19) == 0) {
      _fwd_prefs.whitelist_count = 0;
      _fwd_prefs.save(_fs);
      strcpy(reply, "OK");
    } else if (memcmp(config, "fwd.whitelist ", 14) == 0) {
      config += 14;
      if (memcmp(config, "on", 2) == 0) { _fwd_prefs.whitelist_mode = 1; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else if (memcmp(config, "off", 3) == 0) { _fwd_prefs.whitelist_mode = 0; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, must be on or off");
    } else if (memcmp(config, "flood.max.anon.request ", 23) == 0) {
      int m = atoi(&config[23]);
      if (m <= 64) { _fwd_prefs.flood_max_anon_request = m; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, max 64");
    } else if (memcmp(config, "flood.max.request ", 18) == 0) {
      int m = atoi(&config[18]);
      if (m <= 64) { _fwd_prefs.flood_max_request = m; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, max 64");
    } else if (memcmp(config, "flood.max.response ", 19) == 0) {
      int m = atoi(&config[19]);   // NOTE: PR #2797 had an off-by-one here ([18]); fixed.
      if (m <= 64) { _fwd_prefs.flood_max_response = m; _fwd_prefs.save(_fs); strcpy(reply, "OK"); }
      else strcpy(reply, "Error, max 64");
    } else if (memcmp(config, "fwd.scoped.reserve ", 19) == 0) {
      _fwd_prefs.scoped_reserve_pct = constrain(atoi(&config[19]), 0, 100);
      _fwd_prefs.save(_fs); strcpy(reply, "OK");
    } else if (memcmp(config, "fwd.chan.block ", 15) == 0) {
      char buf[160];
      strncpy(buf, &config[15], sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
      char* sp = strchr(buf, ' ');
      const char* label = NULL;
      if (sp) { *sp = 0; label = sp + 1; }

      uint8_t key[FWD_KEY_SIZE];
      memset(key, 0, sizeof(key));
      int hexlen = strlen(buf);
      bool ok;
      if (buf[0] == '#') {
        mesh::deriveChannelKey(buf, key);       // public hashtag channel: key is SHA256 of the name
        if (!label) label = buf;                // remember what was blocked, for `get`
        ok = true;
      } else if (strcmp(buf, FWD_CHAN_PUBLIC_NAME) == 0 || strcmp(buf, "public") == 0) {
        // Convenience alias for the built-in default channel, which has no '#name' to derive from.
        static const uint8_t pub[16] = FWD_CHAN_PUBLIC_PSK;
        memcpy(key, pub, sizeof(pub));
        if (!label) label = FWD_CHAN_PUBLIC_NAME;
        ok = true;
      } else if (hexlen == 32 || hexlen == 64) {
        ok = mesh::Utils::fromHex(key, hexlen / 2, buf);   // raw PSK: 128-bit or 256-bit
      } else {
        ok = false;
      }

      if (!ok) {
        strcpy(reply, "Error: need #name, or 32/64-hex key [label]");
      } else if (_fwd_prefs.chan_count >= FWD_CHAN_MAX) {
        strcpy(reply, "Error: table full");
      } else {
        int idx = -1;
        for (int k = 0; k < _fwd_prefs.chan_count; k++)
          if (memcmp(_fwd_prefs.chan_keys[k], key, FWD_KEY_SIZE) == 0) { idx = k; break; }
        if (idx < 0) { idx = _fwd_prefs.chan_count++; memcpy(_fwd_prefs.chan_keys[idx], key, FWD_KEY_SIZE); }
        // Hash and key derivation happen HERE, once. The forwarding path never hashes a name.
        _fwd_prefs.chan_hash[idx] = mesh::channelWireHash(key);
        memset(_fwd_prefs.chan_label[idx], 0, FWD_CHAN_LABEL_LEN);
        if (label) {
          strncpy(_fwd_prefs.chan_label[idx], label, FWD_CHAN_LABEL_LEN - 1);
        }
        _fwd_prefs.save(_fs);
        sprintf(reply, "OK (hash %02X)", (uint32_t)_fwd_prefs.chan_hash[idx]);
      }
    } else if (memcmp(config, "fwd.chan.unblock ", 17) == 0) {
      const char* arg = &config[17];
      uint8_t key[FWD_KEY_SIZE];
      int removed = 0;
      int by_index = -1;
      if (arg[0] == '#') {
        mesh::deriveChannelKey(arg, key);
      } else if (strcmp(arg, FWD_CHAN_PUBLIC_NAME) == 0 || strcmp(arg, "public") == 0) {
        static const uint8_t pub[16] = FWD_CHAN_PUBLIC_PSK;   // same alias as fwd.chan.block
        memset(key, 0, sizeof(key));
        memcpy(key, pub, sizeof(pub));
      } else if (arg[0] >= '0' && arg[0] <= '9' && strlen(arg) <= 2) {
        by_index = atoi(arg);                    // also accept the index shown by `get fwd.chan`
      } else {
        int hexlen = strlen(arg);
        memset(key, 0, sizeof(key));
        if (!(hexlen == 32 || hexlen == 64) || !mesh::Utils::fromHex(key, hexlen / 2, arg)) {
          strcpy(reply, "Error: need #name, 32/64-hex key, or index");
          return true;
        }
      }
      for (int k = 0; k < _fwd_prefs.chan_count; ) {
        bool hit = (by_index >= 0) ? (k == by_index)
                                   : (memcmp(_fwd_prefs.chan_keys[k], key, FWD_KEY_SIZE) == 0);
        if (hit) {
          for (int j = k; j < _fwd_prefs.chan_count - 1; j++) {
            memcpy(_fwd_prefs.chan_keys[j], _fwd_prefs.chan_keys[j + 1], FWD_KEY_SIZE);
            _fwd_prefs.chan_hash[j] = _fwd_prefs.chan_hash[j + 1];
            memcpy(_fwd_prefs.chan_label[j], _fwd_prefs.chan_label[j + 1], FWD_CHAN_LABEL_LEN);
          }
          _fwd_prefs.chan_count--; removed++;
          break;   // indices shift; one unblock removes one entry
        }
        k++;
      }
      _fwd_prefs.save(_fs);
      sprintf(reply, "OK (%d removed)", removed);
    } else if (memcmp(config, "fwd.chan.clear", 14) == 0) {
      _fwd_prefs.chan_count = 0;
      _fwd_prefs.save(_fs);
      strcpy(reply, "OK");
#ifdef FWD_CHAN_TIMING
    } else if (memcmp(config, "fwd.chan.bench ", 15) == 0) {
      // Measurement scaffold, compiled ONLY with -D FWD_CHAN_TIMING -- not in any release build.
      // Times the gate itself on a synthetic payload whose hash byte matches every configured
      // entry, so every entry is forced through an HMAC and none of them match: the worst case.
      // The number that matters is the DIFFERENCE against chan_count = 0, which measures the same
      // loop with the gate skipped. A single absolute figure would just be micros() overhead.
      //   set fwd.chan.bench <iterations> [payload_bytes]
      int iters = atoi(&config[15]);
      if (iters < 1) iters = 1;
      int plen = MAX_PACKET_PAYLOAD;
      char* sp = strchr((char*)&config[15], ' ');
      if (sp) {
        int p = atoi(sp + 1);
        if (p >= CHAN_PAYLOAD_MIN_LEN && p <= MAX_PACKET_PAYLOAD) plen = p;
      }
      static uint8_t bench_buf[MAX_PACKET_PAYLOAD];
      for (int i = 0; i < plen; i++) bench_buf[i] = (uint8_t)i;
      bench_buf[0] = _fwd_prefs.chan_count ? _fwd_prefs.chan_hash[0] : 0;
      volatile int sink = 0;   // keeps the optimiser from deleting the whole loop
      unsigned long t0 = micros();
      for (int i = 0; i < iters; i++) {
        sink += mesh::findBlockedChannel(bench_buf, plen, _fwd_prefs.chan_keys,
                                         _fwd_prefs.chan_hash, _fwd_prefs.chan_count);
      }
      unsigned long dt = micros() - t0;
      sprintf(reply, "> n=%d len=%d iters=%d: %luus total, %luns/call (sink=%d)",
              (int)_fwd_prefs.chan_count, plen, iters, dt,
              (unsigned long)((uint64_t)dt * 1000 / (uint32_t)iters), (int)sink);
#endif
    } else {
      return false;   // not a fwd 'set' -> let CommonCLI handle it
    }
    return true;
  }

  if (memcmp(command, "get ", 4) == 0) {
    char* config = command + 4;
    if (memcmp(config, "fwd.hashfilter.prob", 19) == 0) {   // dedicated getter (set/get symmetry)
      sprintf(reply, "> %d", (int)_fwd_prefs.hashfilter_prob);
    } else if (memcmp(config, "fwd.hashfilter", 14) == 0) {
      const char* m = _fwd_prefs.hashfilter_mode == 1 ? "advert"
                    : (_fwd_prefs.hashfilter_mode == 2 ? "all" : "off");
      sprintf(reply, "> %s prob=%d", m, (int)_fwd_prefs.hashfilter_prob);
    } else if (memcmp(config, "fwd.block", 9) == 0) {
      char* p = reply;
      p += sprintf(p, "> %d entr%s", (int)_fwd_prefs.block_count, _fwd_prefs.block_count == 1 ? "y" : "ies");
      for (int k = 0; k < _fwd_prefs.block_count && (p - reply) < 140; k++) {
        char hex[16];
        mesh::Utils::toHex(hex, _fwd_prefs.block_keys[k], 6); hex[12] = 0;  // 6-byte prefix
        uint8_t a = _fwd_prefs.block_actions[k];
        p += sprintf(p, " | %s %s%s", hex, (a & FWD_BLOCK_PRUNE_PATH) ? "P" : "",
                     (a & FWD_BLOCK_DROP_ADVERT) ? "A" : "");
      }
    } else if (memcmp(config, "fwd.whitelist", 13) == 0) {
      char* p = reply;
      p += sprintf(p, "> %s 0hop=%s %d entr%s", _fwd_prefs.whitelist_mode ? "on" : "off",
                   _fwd_prefs.whitelist_zerohop ? "allow" : "drop",
                   (int)_fwd_prefs.whitelist_count, _fwd_prefs.whitelist_count == 1 ? "y" : "ies");
      for (int k = 0; k < _fwd_prefs.whitelist_count && (p - reply) < 140; k++) {
        char hex[16];
        mesh::Utils::toHex(hex, _fwd_prefs.whitelist_keys[k], 6); hex[12] = 0;  // 6-byte prefix
        p += sprintf(p, " | %s", hex);
      }
    } else if (memcmp(config, "flood.max.anon.request", 22) == 0) {
      sprintf(reply, "> %d", (int)_fwd_prefs.flood_max_anon_request);
    } else if (memcmp(config, "flood.max.request", 17) == 0) {
      sprintf(reply, "> %d", (int)_fwd_prefs.flood_max_request);
    } else if (memcmp(config, "flood.max.response", 18) == 0) {
      sprintf(reply, "> %d", (int)_fwd_prefs.flood_max_response);
    } else if (memcmp(config, "fwd.chan.stats", 14) == 0) {   // before the fwd.chan prefix below
      sprintf(reply, "> blocked=%lu saved_air=%lums",
              (unsigned long)n_drop_chan, (unsigned long)airtime_saved_chan);
    } else if (memcmp(config, "fwd.chan", 8) == 0) {
      // Two forms, because the reply buffer is 160 bytes and a labelled list of FWD_CHAN_MAX
      // entries does not fit in it. Listing every entry with its label would silently truncate,
      // and the table size would end up dictated by the width of a serial reply.
      //   get fwd.chan       -> compact: count + one hash byte each, always fits
      //   get fwd.chan <n>   -> one entry with its label
      if (config[8] == ' ') {
        int k = atoi(&config[9]);
        if (k < 0 || k >= _fwd_prefs.chan_count) {
          sprintf(reply, "Error: index 0..%d", (int)_fwd_prefs.chan_count - 1);
        } else {
          // A raw-key entry has no name to show -- the wire hash is not reversible.
          // reference/chan_filter_vectors.py names a hash byte host-side.
          sprintf(reply, "> %d: %s (%02X)", k,
                  _fwd_prefs.chan_label[k][0] ? _fwd_prefs.chan_label[k] : "(raw)",
                  (uint32_t)_fwd_prefs.chan_hash[k]);
        }
      } else {
        char* p = reply;
        p += sprintf(p, "> %d entr%s", (int)_fwd_prefs.chan_count, _fwd_prefs.chan_count == 1 ? "y" : "ies");
        for (int k = 0; k < _fwd_prefs.chan_count; k++) {
          p += sprintf(p, "%s%02X", k ? " " : " | ", (uint32_t)_fwd_prefs.chan_hash[k]);
        }
      }
    } else if (memcmp(config, "fwd.scoped.reserve", 18) == 0) {   // dedicated getter (set/get symmetry)
      sprintf(reply, "> %d", (int)_fwd_prefs.scoped_reserve_pct);
    } else if (memcmp(config, "fwd.scoped.stats", 16) == 0) {
      // air= is the gate's actual input: TX airtime used within the current short window, over the
      // allowance for that window (which scales with the operator's configured duty cycle). Reported
      // so the reserve's behaviour is observable on a live node instead of derived from the source.
      sprintf(reply, "> reserve=%d%% fwd_scoped=%lu fwd_unscoped=%lu drop_unscoped=%lu saved_air=%lums air=%lu/%lums/%ds",
              (int)_fwd_prefs.scoped_reserve_pct, (unsigned long)n_fwd_scoped, (unsigned long)n_fwd_unscoped,
              (unsigned long)n_drop_unscoped, (unsigned long)airtime_saved_unscoped,
              airWindowUsed(), airWindowMax(), (int)(FWD_AIR_WINDOW_MS / 1000));
    } else {
      return false;   // not a fwd 'get' -> let CommonCLI handle it
    }
    return true;
  }

  return false;
}

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}
