# FEATURE — Stage 5: key-based channel blocklist (`fwd.chan.block`)

**Status: SPEC ONLY — nothing implemented.** Written 2026-08-17 off the CoreScope measurement below.
Target: fwdfilter10 at the earliest, on top of `repeater-v1.17.1.fwdfilter9`. The key derivation that
blocked this spec is now **verified end to end against live traffic** (§ Key derivation); no known
blocker remains, only the site-local justification question in § Open.

## Problem

Group-channel traffic is a large, measurable slice of what a repeater forwards, and some of it is a
single noisy channel. Measured 2026-08-16 over 24 h across all CoreScope observers, 76 298 unique
flood packets (`reference/corescope_channel_profile.py`, class-1 fields only — `payload_type`,
`route_type`, `raw_hex`; nothing derived from path-hash attribution):

| | share of flood packets | share of flood airtime |
|---|---|---|
| ADVERT | 18.6 % | 32.7 % |
| **GRP_TXT** | **19.2 %** | **22.3 %** |
| REQ | 23.0 % | 13.9 % |
| GRP_DATA | 0.0 % | 0.1 % |

Group traffic is **22.4 % of flood airtime**. It concentrates hard: the top 5 channel hashes carry
42.4 % of group packets, and the top 3 alone are **7.4 % of all flood airtime**. The single busiest,
`0xD9`, is 3.3 %.

Group traffic is effectively flood-only here — of 14 638 GRP_TXT packets in the window, 14 637 were
flood and 1 was direct.

**Why the obvious implementation is wrong.** The wire carries only `payload[0]`, a one-byte channel
hash (`SHA256(secret)[0]`, `BaseChatMesh.cpp:887`). Matching on that byte alone is not selective:
**244 of 256 possible hash values were occupied** in the window, median 8 packets each. Blocking a
hash would almost certainly also block unrelated channels that collide on that byte, and there is no
larger hash available — the protocol transmits one byte, full stop.

## Key derivation — VERIFIED end to end (2026-08-17)

Three separate facts, each checked in source and then confirmed against live traffic.

**1. The firmware has no name→key derivation at all.** `BaseChatMesh::addChannel(name, psk_base64)`
(`BaseChatMesh.cpp:880`) base64-decodes a PSK into a zeroed 32-byte buffer and accepts only length 16
or 32. The name is a label and nothing else. **The `#name` convention lives in the client**, so the
repeater must implement the client convention itself — there is no firmware function to reuse.

**2. The client convention** (`meshcore/commands/device.py:215`, the canonical implementation):

```python
channel_secret = sha256(channel_name.encode("utf-8")).digest()[0:16]   # name INCLUDES the '#'
```

So `#name` channels are always **128-bit**, zero-padded into the 32-byte buffer. That makes the
short-key path the normal case, not an edge case.

**3. The two hashes are taken over different lengths — this is the trap.**

| quantity | input | note |
|---|---|---|
| channel secret | `SHA256("#name")[0:16]` | 16 bytes, buffer zero-filled to 32 |
| `hash[0]` on the wire | `SHA256(secret[0:16])[0]` | over **16** bytes (`setChannel` picks 16 vs 32 by testing whether bytes 16..31 are zero, `BaseChatMesh.cpp:908`) |
| MAC | `HMAC-SHA256(key = secret[0:32])` over the ciphertext, first 2 bytes | over **32** bytes — `MACThenDecrypt` always passes `PUB_KEY_SIZE` (`Utils.cpp:155/163`) |

Hash over 16, HMAC over 32, from the same buffer. Getting either length wrong yields a filter that
silently never matches.

**Confirmed against 6000 live GRP_TXT packets** — derived the key, then MAC-verified only (no
`decrypt()` call anywhere in the check):

| channel | `hash[0]` | packets on that byte | MAC-confirmed |
|---|---|---|---|
| `Public` (default PSK `izOH6cXN6mrJ5e26oRXNcg==`) | `0x11` | 740 | 711 (96.1 %) |
| `#test` | `0xD9` | 1064 | 1025 (96.3 %) |
| `#austria` | `0xFB` | 371 | 361 (97.3 %) |

**The residual is the collision, measured directly.** All 39 non-matching packets on `0xD9` parse
correctly (their MAC equals `decoded_json.mac`) but belong to some other channel.

Candidate names come from **CoreScope's own `/api/channels`** (42 channels it has seen named) —
guessing a wordlist was reinventing that, and worse: guessing found 12 channels, CoreScope's list
found 27, including every neighbouring-country channel nobody here would think to try. The name comes
from CoreScope; the attribution is still MAC-verified locally, so no class-2 inference is involved.
`corescope_channel_profile.py --identify` named **46 % of group flood airtime**:

| hash | channel | share of group airtime | share of that hash byte that really is this channel |
|---|---|---|---|
| `0xD9` | `#test` | 14.4 % | 96 % |
| `0x11` | `Public` (mainline default PSK) | 11.4 % | 96 % |
| `0xFB` | `#austria` | 4.3 % | 96 % |
| `0x28` | `#ping` | 3.1 % | 92 % |
| `0x2F` | `#hungary` | 2.5 % | 93 % |
| `0xCA` | `#bot` | 2.0 % | 96 % |
| `0xDD` | `#vienna` | 1.9 % | 79 % |
| `0xB2` | `#slovakia` | 1.7 % | 95 % |
| `0x81` | `#wardriving` | 1.7 % | 89 % |
| **`0xB3`** | **`#hamradio`** | 0.5 % | **43 %** |
| **`0x8D`** | **`#polska`** | 0.1 % | **19 %** |
| **`0x98`** | **`#yo`** | 0.0 % | **1 %** |

**The strongest argument for the feature is in that list.** An Austrian repeater demonstrably forwards
`#hungary`, `#slovakia`, `#polska`, `#switzerland`, `#kosice`, `#turiec` and `#slovenia` — together
over 4 % of group airtime spent on channels that address nobody in the local network. Unlike blocking
a busy local channel, dropping those is not a contentious policy call.

Names are case- and prefix-exact — the key is `SHA256` of the literal string, so `#ping` matches while
`ping`, `#Ping` and `#PING` all match nothing.

For a busy channel a hash-only filter is ~96 % accurate. For `#wien` or `#chat` **the majority of
traffic on that byte belongs to someone else** — such a filter would drop more foreign traffic than
target traffic. There is no way to tell the two cases apart without the key, which is the whole
argument for this design.

Incidentally this identifies the busiest channels on the AT network: `#test`, the default `Public`
channel, `#austria` and `#ping` — **four channels are a third of all group airtime**, which is what
makes a blocklist of only 8 entries a sensible size.

## Design (proposal)

Identify the channel **cryptographically**, not by its hash byte. The repeater stores the channel
secret, and the two-byte MAC in the packet (`CIPHER_MAC_SIZE = 2`) is HMAC-SHA256 over the ciphertext
under that secret — it matches only for the right key. The ambiguous byte becomes an exact match.

**The filter verifies the MAC and stops.** `Utils::MACThenDecrypt` (`Utils.cpp:147`) does the MAC
compare at `:168` and only then calls `decrypt()`. Stage 5 reimplements the first half and never the
second. This is the point of the whole design: the repeater can name the channel it is dropping
without ever being able to read a message. That is an auditable property of the code — there is no
call to `decrypt()` on this path — and not a promise in a document.

Gate order inside `allowPacketForward()`, cheapest test first:

1. `chan_count == 0` → skip entirely. Zero cost when unconfigured (as with every prior stage).
2. payload type is `PAYLOAD_TYPE_GRP_TXT` (5) or `PAYLOAD_TYPE_GRP_DATA` (6), and `isRouteFlood()`.
3. `payload[0]` vs the cached `chan_hash[i]` byte — a plain compare, no crypto. Non-matching packets
   leave here.
4. Only on a byte match: HMAC-SHA256 over `payload[3..]` with `chan_keys[i]`, compare 2 bytes against
   `payload[1..2]`.
5. MAC matches → `return false` (drop), bump the counter.

False positives: a 2-byte MAC gives 1 in 65 536 per configured channel per packet. With a handful of
entries that is negligible, and vastly better than the hash-only variant where collateral is the norm.

**Restricted to flood on purpose.** Direct group traffic was 1 packet in 24 h; excluding it costs
nothing measurable and keeps the blast radius small.

### CLI (set/get symmetry, per the fork's convention)

```
set fwd.chan.block #atchat        # public hashtag channel: key derived locally from the name
set fwd.chan.block <64-hex>       # raw 256-bit channel key, optional label via a second arg
set fwd.chan.unblock #atchat      # or an index
get fwd.chan.block                # "> 2 entries | #atchat (D9) | (11)"
get fwd.chan.stats                # "> blocked=1832 saved_air=1691000ms"
```

Every `set` gets a dedicated `get`, including `get fwd.chan.stats` — the airtime figure is what makes
the feature's effect observable on a live node instead of inferred from the source.

### Storage

New in `struct FwdPrefs`:

```c
#define FWD_CHAN_MAX  8            // 8 x (32 key + 1 hash + 16 label) = 392 bytes
uint8_t chan_count;
uint8_t chan_keys[FWD_CHAN_MAX][FWD_KEY_SIZE];   // 32-byte buffer, as GroupChannel::secret
uint8_t chan_hash[FWD_CHAN_MAX];                 // cached SHA256(secret)[0]
char    chan_label[FWD_CHAN_MAX][16];            // display only; "" for raw-key entries
```

New TLV tags in the free 0x60 region: `FWD_TAG_CHAN_COUNT 0x60`, `_KEYS 0x61`, `_HASH 0x62`,
`_LABEL 0x63`. Absent tags on an older `/fwd_prefs` file leave `reset()`'s `chan_count = 0` = off, so
forward and backward compatibility hold by construction, exactly as for Stage 4.

The hash byte is **cached at set time**, not recomputed per packet. Key derivation for `#name`
entries also happens once, in the CLI handler — the forwarding path never hashes a name.

## Non-goals

- **No whitelist.** With 244 occupied hash buckets, most of them channels we hold no key for, a
  positive list would silently drop a long tail of legitimate traffic. You cannot allow-list what you
  cannot enumerate.
- **No decryption**, ever, on this path. See above.
- **No channel discovery.** The repeater learns nothing; it only recognises what an operator configured.
- **No private-key distribution mechanism.** Raw keys are accepted, but getting one onto the node is
  the operator's problem.

## Risks

- **Key custody.** A repeater holding private channel keys is a different trust object than one that
  cannot. KK sits on a 20 m mast; a stolen node would leak whatever keys it carries. For public
  `#hashtag` channels this is moot — the key is `SHA256(name)` and anyone can derive it, so the node
  gains no capability an attacker did not already have. **Recommendation: document Stage 5 as intended
  for public channels, and treat raw private keys as an advanced, discouraged option.**
- **Policy, not just engineering.** Dropping a public channel on a shared repeater affects other
  operators' traffic. This belongs in the German docs in the same tone as the existing filter stages:
  what it does, and that the choice is the operator's.
- **CPU on the forwarding path.** One HMAC-SHA256 per configured channel whose hash byte matched.
  Bounded by step 3 and by `FWD_CHAN_MAX`. nRF52840 has CC310 hardware crypto, already used under
  `USE_CC310_HW_CRYPTO`; Heltec V3 does it in software. Needs measuring, not assuming.
- **128-bit channels are the normal case, not an edge case** — every `#name` channel is 16 bytes,
  zero-padded to 32. Resolved above: hash over 16 bytes, HMAC over 32. Mirror mainline's buffer
  handling rather than reimplementing it.

## Test plan (bench, mirroring the fwdfilter3/4 validation)

1. CLI: set/get/unblock round-trip, both `#name` and raw-hex forms, clamp at `FWD_CHAN_MAX`.
2. Persist: reboot, values survive (TLV 0x60–0x63); an old `/fwd_prefs` still loads with the filter off.
3. Zero-cost when off: `chan_count = 0` → forwarding counters identical to fwdfilter9 under load.
4. **Positive:** companion joins `#benchtest`, sends group messages; repeater with that channel blocked
   drops them, `get fwd.chan.stats` climbs, other traffic unaffected.
5. **Negative control — the test that justifies the whole design.** Construct a second channel whose
   key hashes to the *same* first byte (brute-force a name until `SHA256(SHA256(name))[0]` collides),
   block only the first, and prove the second is **still forwarded**. A hash-only filter fails this
   test by construction. Without it we have not shown the crypto buys anything.
6. Airtime: measure `saved_air` against the drop count and the SF8 airtime model.

## Open

- ~~Exact key derivation for `#name` group channels~~ — **RESOLVED 2026-08-17**, see above. Note the
  correction to an earlier assumption: it is *not* `SHA256(name)` over 32 bytes as in
  `TransportKeyStore.cpp:44` (that is the *region* path); group channels truncate to 16.
- Whether `chan_label` earns its 128 bytes, or whether printing the hash byte is enough.

**Not open: whether the netwide mix matches KK's.** This firmware is published for the wider MeshCore
community, and for that audience the netwide measurement *is* the relevant basis — our own two nodes
are two operators among many. A site whose local mix differs simply leaves `chan_count = 0` and pays
nothing, which is the same opt-in shape as every other stage. The feature does not need to earn its
place at KK to earn its place in the release.

What a site-local measurement is still good for is *tuning* — which channel a given operator should
block — and that belongs in the docs as a recipe, not as a gate on shipping.

### Naming a hash the operator has not configured

`get fwd.chan.block` can only print labels for channels the operator entered; the wire hash itself is
not reversible. But it does not have to be: `corescope_channel_profile.py --identify` takes the names
CoreScope already publishes at `/api/channels`, derives the key from each, and **MAC-verifies it
against real packets** — certainty rather than a 1-in-256 byte guess, the same mechanism the filter
uses. That names 46 % of group airtime today, and an operator who knows their local channel names
will do better.

This belongs in the German docs alongside the CLI, or the operator sees `0xD9` and has no way to know
what they would be blocking. Deliberately a **host-side** tool: the repeater needs none of it, and the
question is asked once, when deciding what to block.
