# FEATURE: Fork-private prefs file (`/fwd_prefs`, TLV) — design lock

Status: **IMPLEMENTED + HARDWARE-VALIDATED** on `repeater-fwdfilter-main` (uncommitted). Target line: `fwdfilter4`.
Tracking artifact (GH issues disabled on this fork; see also `FEATURE-fwd-hashfilter.md`).

## Validation (2026-06-20, RAK4631 @ COM12, debug build)
- Builds clean: `RAK_4631_repeater` (nRF52, RAM 14.0% / Flash 63.4%) + `Heltec_v3_repeater` (ESP32, Flash 35.0%).
- **Structural goal met:** diff vs `meshcore/main` touches only `MyMesh.{cpp,h}` (+249); `CommonCLI.{h,cpp}`
  have ZERO fwd references. `/com_prefs` is pristine mainline by construction — fwd config lives only in
  the new `FwdPrefs.{h,cpp}` + `/fwd_prefs`. No fwd field can ever collide with a mainline prefs offset.
- **CLI + persistence: 19/19** (`bench_cli_test.py`): full fwd.* surface, new `get fwd.hashfilter.prob`
  (set/get symmetry), all 3 PR#2797 caps incl. the off-by-one-fixed `flood.max.response`, mainline
  pass-through, whitelist/blacklist tables — and **persistence across reboot** (prob/cap/whitelist/block
  all survived a `reboot`), proving the `/fwd_prefs` TLV round-trips.
- **RF forward hook live: 4/4** — blacklisted the Heltec companion's pubkey (DROP_ADVERT); it flooded 4
  adverts, RAK received + dropped all 4 ("fwd-filter: drop advert from blocklisted node"). Full-pubkey
  match on real RF confirms the allowPacketForward hook reads `_fwd_prefs` correctly at decision time.
- RF-admin fwd commands covered by construction (serial + RF both funnel through `MyMesh::handleCommand`
  -> `handleFwdCommand`); a dedicated RF-admin re-test is pending bench tooling (rf_admin_test.py absent).
- Filter ALGORITHMS are a verbatim port of `repeater-1.16.0.fwdfilter3` (already 10/10 RF-bench validated);
  only the config source changed (`_prefs.fwd_*` -> `_fwd_prefs.*`), and that path is now 19/19 + 4/4 proven.

Base: **`meshcore/main` @ e8d3c53b** (stable mainline; worktree `C:/Users/Chris/MeshCore-rpt-main`,
branch `repeater-fwdfilter-main`). This is a **replant** of the fwd feature onto mainline — NOT a
rebase of the agessaman-based `repeater-hash-filter`. Separability is verified: agessaman forks
`CommonCLI` (+464 L) and `simple_repeater` (+168 L) with observer/MQTT, but our filter logic depends
only on `allowPacketForward`/`filterRecvFloodPacket` (stock mainline functions) + `Identity.h` /
`Packet.h` (byte-identical across forks), so it ports verbatim. `meshcore/main` NodePrefs ends at
`flood_max_advert` (slot 292, `// next: 293`) — identical to our old base — so once the fwd fields
move out, `/com_prefs` tracks mainline byte-for-byte. (`radio_fem_rxgain`/`cad_enabled` and #2797 live
only in `dev`; they will append at 293+ in `main` later and can never reach our separate file.)

## Why

MeshCore prefs are a **flat positional binary blob**: `loadPrefsInt`/`savePrefs` do
sequential field-by-field `file.read`/`file.write`, where *byte offset = field identity*.
No tag, no length, no version, no schema. Two independent problems follow:

1. **Single shared offset namespace, two append streams.** Upstream allocates offsets from
   its tail; our fork allocates from *our* tail. Both claim the same low offsets for different
   fields. Our persisted layout already diverges from mainline at **offset 293** (mainline put
   `radio_fem_rxgain`/`cad_enabled` there; we put `fwd_hashfilter_mode`/`prob`). Collision on
   every future merge that touches prefs is guaranteed by construction.
2. **Insertion = silent corruption, not a compile error.** Any field added anywhere but the
   exact tail shifts every following field; a node then reads `freq`/`region`/`tx_power` from
   the wrong bytes. On a masthead (KK, no backhaul, BLE-DFU-on-mast) that is a field trip.

Trigger: upstream PR #2797 (per-payload flood-hop caps) wants offsets 295/296/297 — which our
fork already uses for `fwd_block_count`/`fwd_block_keys`.

## Decision

Extract all fork-private prefs out of `NodePrefs`/`/com_prefs` into a **fork-owned struct
`FwdPrefs`** persisted to its **own file `/fwd_prefs`** in a **self-describing TLV** format.
`NodePrefs`/`/com_prefs` go back to tracking mainline byte-for-byte.

Result: **zero collision surface.** Mainline may add prefs at any offset forever — it can never
misalign `/fwd_prefs` (different file, self-describing). Silent radio/region corruption from a
prefs merge becomes structurally impossible. The TLV makes our *own* future additions
insertion-safe too (unknown tag skipped, missing tag → default).

### Placement rule (the durable principle)

> `/fwd_prefs` (fork-owned, TLV) holds anything whose mainline fate is **uncertain** or that
> mainline **won't take**. `/com_prefs` holds **only** fields already in mainline at a fixed
> offset. Worst case for misjudging is a loud, one-time merge cleanup — never silent corruption.

Censorship-capable primitives (blacklist, last-hop whitelist) will not be accepted upstream by
policy, so they are permanently fork-private. PR #2797's three `flood_max` caps are **also**
placed here: #2797 may never merge, and if it dies mainline is free to reuse 295/296/297 for
something else. If mainline later *does* adopt #2797, we delete our copies and take theirs — a
loud compile/merge-time cleanup, not a runtime hazard. CLI command names are kept identical to
#2797 so users see zero churn across that eventual cleanup.

## `FwdPrefs` struct (moved out of `NodePrefs`)

| Field | Type | Default | Source |
|---|---|---|---|
| `hashfilter_mode` | `u8` | 0 (off) | existing |
| `hashfilter_prob` | `u8` | 100 | existing |
| `block_count` | `u8` | 0 | existing |
| `block_keys[16][32]` | bytes | zeroed | existing (`FWD_BLOCK_MAX`=16, pubkey=32) |
| `block_actions[16]` | `u8[]` | zeroed | existing |
| `whitelist_mode` | `u8` | 0 (off) | existing |
| `whitelist_zerohop` | `u8` | 1 (allow) | existing |
| `whitelist_count` | `u8` | 0 | existing |
| `whitelist_keys[16][32]` | bytes | zeroed | existing (`FWD_WL_MAX`=16) |
| `flood_max_request` | `u8` | 64 | PR #2797 |
| `flood_max_anon_request` | `u8` | 64 | PR #2797 |
| `flood_max_response` | `u8` | 64 | PR #2797 |

## `/fwd_prefs` file format

```
Header:  "FWDP" (4 bytes magic) | format_version u8 (=1)
Record:  tag u8 | len u16 LE | value[len]
         ... repeated until EOF ...
```

`len` is `u16` because the key tables exceed 255 bytes (16×32 = 512). Tables store only the
**active** entries (`value len = count*32`); `len` self-documents the byte count.

### Tag map (fork-owned id space)

| Tag | Field | Value |
|---|---|---|
| `0x10` | hashfilter_mode | 1 B |
| `0x11` | hashfilter_prob | 1 B |
| `0x20` | block_count | 1 B |
| `0x21` | block_keys | `count*32` B |
| `0x22` | block_actions | `count` B |
| `0x30` | whitelist_mode | 1 B |
| `0x31` | whitelist_zerohop | 1 B |
| `0x32` | whitelist_count | 1 B |
| `0x33` | whitelist_keys | `count*32` B |
| `0x40` | flood_max_request | 1 B |
| `0x41` | flood_max_anon_request | 1 B |
| `0x42` | flood_max_response | 1 B |

### Reader semantics (`FwdPrefs::load`)

1. Set all fields to defaults (table above).
2. Open `/fwd_prefs`. Absent → keep defaults, return (fresh/pre-feature node).
3. Read 4-byte magic; mismatch → keep defaults, return (guards garbage/wrong file).
4. Read `format_version`; accept `==1` (future: parse known tags, skip unknown).
5. Loop: read `tag` (EOF → done) | `len` | `value`. Dispatch:
   - known scalar: copy if `len` matches expected, else skip+log.
   - known table: `memcpy` if `len ≤ sizeof(field)`, else skip+log (corrupt).
   - unknown tag: skip `len` bytes (forward-compat).
6. Sanitise (same `constrain()` as today): mode 0..2, prob 0..100,
   counts ≤ MAX else 0, whitelist_mode/zerohop 0..1, flood caps ≤ 64.

### Writer (`FwdPrefs::save`)

Write magic+version, then one TLV per field in fixed tag order (order is not load-significant;
fixed for stable diffs). Tables write only `count` active entries.

## Migration

**None.** Fork firmware is on testbench nodes only (Bench-RAK COM12, Heltec sender) — wipe and
reflash. No legacy reader. The fwd_* `file.read`/`file.write` calls (com_prefs slots
293–294, 295–296, 808, 824–827) are **deleted**; `/com_prefs` returns to base layout.
- These are the **trailing** persisted fields, so removing them only truncates the tail — no
  field after them shifts. ✅ VERIFIED: `savePrefs` writes `fwd_whitelist_keys` last
  (CommonCLI.cpp:222) right before `file.close()`; bridge prefs are at offsets 127–152, ahead
  of the fwd block. (The existing `// next: 824` comment on line 223 is already stale — load
  says 1339 — corroborating the hand-maintained-offset rot this change removes.)
- In-place upgrade (if not reflashed) degrades gracefully: new firmware ignores the old trailing
  fwd bytes in `/com_prefs` and finds no `/fwd_prefs` → fwd config resets to defaults (OFF).
  Acceptable on bench; reflash + reconfigure is the plan.

## Scope / non-goals

- **In scope:** new `FwdPrefs` struct + `/fwd_prefs` TLV store; relocate the 9 existing fwd_*
  fields + #2797's 3 caps; repoint enforcement reads (`allowPacketForward`,
  `filterRecvFloodPacket`) and CLI `fwd.*` + `flood.max.*request/response` handlers to the new
  struct; add #2797's enforcement (3 lines in `allowPacketForward`) + CLI set/get.
- **Better than planned:** the `fwd.*` / `flood.max.*request` CLI handlers ended up **fully** in
  `MyMesh::handleFwdCommand`, intercepted in `MyMesh::handleCommand` before the `_cli` delegation.
  Since both serial and RF admin funnel through `MyMesh::handleCommand` (RF via `handleRequest`->726),
  one choke point covers both paths AND `CommonCLI` stays 100% pristine. No upstream-file fwd delta at all.
- **Out of scope:** the `dev`-only `radio_fem_rxgain`/`cad_enabled` (not in `main`, so no divergence here).
- **RF orthogonality:** `/fwd_prefs` is on-flash only. The `get fwd.whitelist` RF serialization
  (capped <140 B by `e1b1c773`) reads the in-RAM struct, not the file — unaffected.

## #2797 port notes

- Three checks in `allowPacketForward` (REQ / ANON_REQ / RESPONSE `getPathHashCount() >= cap`),
  defaults 64 (no-op). Keep CLI names `flood.max.request` / `.anon.request` / `.response`.
- **Fix #2797 bug on the way in:** response setter parses `atoi(&config[18])` for a 19-char key
  (works only because atoi skips the leading space) → use `[19]`.
- **Deployment:** leave all three at 64 on KK/Hofstetten backbone high-sites. Lowering them
  drops far-traveling REQ/ANON_REQ/RESPONSE = login/contact/**RF-admin** traffic; too low locks
  us out of the no-backhaul masthead. Tactical lowering only against an identified flood source.

## Implementation checklist

- [ ] `src/helpers/FwdPrefs.h` — `FwdPrefs` struct + `FwdPrefsStore` (load/save TLV, magic/ver).
- [ ] Remove fwd_* fields from `NodePrefs` (CommonCLI.h) + their load/save lines (CommonCLI.cpp).
- [x] Verify no persisted field follows `fwd_whitelist_keys` in `/com_prefs`. ✅ confirmed (last write, line 222).
- [ ] Repoint `MyMesh::allowPacketForward` + `filterRecvFloodPacket` reads to `FwdPrefs`.
- [ ] Repoint CLI `fwd.*` set/get + add `flood.max.*request/response` set/get → `FwdPrefs`.
- [ ] `FwdPrefs::save()` called on every `fwd.*` / `flood.max.*` set (replaces `savePrefs()`).
- [ ] Build `RAK_4631_repeater` + `_debug` clean.
- [ ] Bench re-validate: whitelist add/on/0hop, hashfilter, blacklist, 3 flood caps; persistence
      across reboot; confirm `/com_prefs` no longer carries fwd bytes.
- [ ] RF-admin 7/7 (`reference/rf_admin_test.py`) still green.
- [ ] Release `repeater-1.16.0.fwdfilter4`; supersede fwdfilter3.
