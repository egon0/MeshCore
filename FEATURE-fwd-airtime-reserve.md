# FEATURE — Forward-filter airtime reserve for scoped traffic (`fwd.scoped.reserve`)

**Status: IMPLEMENTED + HW-VALIDATED 13/13 (2026-07-10).** All 6 touchpoints in place; `pio run -e
RAK_4631_repeater` = SUCCESS (RAM 14.0%, Flash 63.5%). Bench validation via `bench_scoped_test.py` on the
RAK4631 (COM12): CLI set/get + clamp + stats + mainline pass-through, PERSIST across reboot (TLV 0x50), and
the LIVE gate proven on real mesh traffic — pct=0 window +3 fwd_unscoped/+0 drop, pct=100 window +0
fwd_unscoped/+3 drop (clean symmetric flip). SCOPED-BYPASS also live-proven: defined region `at` on the RAK,
set pct=100, injected scoped floods from the Heltec companion (`meshcore-cli -s COM3 scope at public "..."`)
-> RAK fwd_scoped 0->2 while unscoped stayed dropped. Remaining: commit + tag fwdfilter5 + release. Target:
fwdfilter5 on top of fwdfilter4. Worktree `MeshCore-rpt-main` (replanted on `meshcore/main`).

NOTE vs plan: shipped **4** runtime counters (n_fwd_scoped, n_fwd_unscoped, n_drop_unscoped,
airtime_saved_unscoped) instead of 6 — airtime is tracked only on the drop path (where `est` already exists),
so pct=0 stays a true no-op with zero extra `getEstAirtimeFor` calls.

## Problem

Unscoped flood traffic (no region code) is >80% of the AT network, should be the minority. Mostly from
misconfigured / legacy nodes (an unconfigured node ALWAYS sends unscoped — the default+legacy behaviour for
compatibility with pre-region firmware). It starves the legitimate scoped traffic between correctly
configured repeaters. Goal: under airtime pressure, drop unscoped floods so scoped delivery keeps its budget.

## Design (frozen)

A single new pref: **`fwd.scoped.reserve <0..100>`** = percentage of this node's TX airtime budget reserved
for scoped-only traffic. Load-adaptive, built on the EXISTING token bucket:

- `max_budget = getDutyCycleWindowMs() / (1 + getAirtimeBudgetFactor())` (currently 3600000 / (1+1.0) =
  1,800,000 ms). `reserve_ms = scoped_reserve_pct/100 * max_budget`.
- In `allowPacketForward`, for an **unscoped flood**: drop iff
  `getRemainingTxBudget() - estAirtime < reserve_ms`. **Scoped** floods: never dropped by this gate.
- `pct = 0` → today's behaviour (no reservation). `pct = 100` → drop all unscoped. Between → sliding.
- Load-adaptive for free: bucket sits near full when quiet (unscoped passes), drains under sustained load
  (unscoped chokes first, scoped protected). Reserves only THIS node's TX budget, NOT the RF channel.
- **Default = 0** → install-and-forget nodes are unchanged; only an operator who sets a value opts in.
  Critical because firmware distribution is uncontrolled. Purely local mechanism → partial adoption works,
  mixed networks are protocol-safe (dropping a flood is always legal).

### Why this is safe (verified in code)
- **Scoped replies stay scoped:** `MyMesh::sendFloodReply()` (MyMesh.cpp:416) sends the reply with the
  request's scope. Scoped-in → scoped-out; unscoped-in → unscoped-out. No cross-contamination.
- **Path returns are DIRECT** (`sendDirect`, Mesh.cpp:167), not flood → the gate never touches them.
- **`recv_pkt_region` is already resolved** per packet in `filterRecvFloodPacket()` (MyMesh.cpp:591), which
  runs BEFORE `allowPacketForward`. The scoped/unscoped predicate is therefore free.

### Gate predicate (using existing `recv_pkt_region`)
`filterRecvFloodPacket()` sets:
- unknown transport code / wildcard denies flood → `recv_pkt_region == NULL` → **already hard-dropped** at
  MyMesh.cpp:475 (existing `REGION_DENY_FLOOD` path — unchanged).
- plain FLOOD, wildcard allows → `recv_pkt_region == &wildcard` → **unscoped**, apply the new reserve gate.
- TRANSPORT_FLOOD matched region → `recv_pkt_region != NULL && !isWildcard()` → **scoped**, bypass gate.

So the new gate fires only when `recv_pkt_region != NULL && recv_pkt_region->isWildcard()`.

## Changes, file by file (with anchors)

### 1. `src/helpers/FwdPrefs.h`
- New field in `struct FwdPrefs` (after the flood_max_* block, ~line 70):
  `uint8_t scoped_reserve_pct;   // 0..100 = % of TX airtime budget reserved for scoped-only (0 = off/default)`
- New TLV tag (new id region, after 0x42): `#define FWD_TAG_SCOPED_RESERVE  0x50`

### 2. `src/helpers/FwdPrefs.cpp`
- `reset()` (~line 54): `scoped_reserve_pct = 0;`  ← DEFAULT 0 = backward-compatible no-op.
- `sanitise()` (~line 66): `if (scoped_reserve_pct > 100) scoped_reserve_pct = 100;`
- `load()` switch (~line 105): `case FWD_TAG_SCOPED_RESERVE: fwd_read_u8(file, len, &scoped_reserve_pct); break;`
- `save()` (~line 143): `fwd_write_u8(file, FWD_TAG_SCOPED_RESERVE, scoped_reserve_pct);`
  (Absent tag on an old file → reset()'s 0 preserved = off. Forward/back-compat holds by construction.)

### 3. `examples/simple_repeater/MyMesh.cpp` — the gate in `allowPacketForward()`
Insert a new stage inside the `if (packet->isRouteFlood()) { ... }` region, AFTER the existing
`recv_pkt_region == NULL` hard-drop (line 475) so NULL is already handled:
```
// [fwd-filter Stage 4] Airtime reserve: under TX-budget pressure, drop UNSCOPED floods so the reserved
// slice stays free for scoped delivery. pct=0 => no-op (default). Scoped (non-wildcard region) bypasses.
if (_fwd_prefs.scoped_reserve_pct > 0
    && recv_pkt_region != NULL && recv_pkt_region->isWildcard()) {   // unscoped, flood-allowed
  unsigned long max_budget = getDutyCycleWindowMs() / (1.0f + getAirtimeBudgetFactor());
  unsigned long reserve_ms = (uint32_t)((uint64_t)max_budget * _fwd_prefs.scoped_reserve_pct / 100);
  uint32_t est = _radio->getEstAirtimeFor(packet->getRawLength());
  if (getRemainingTxBudget() < reserve_ms + est) {
    // increment drop counters (Stage 5); MESH_DEBUG_PRINTLN(...)
    return false;
  }
}
```
Notes: `getRemainingTxBudget()`, `getDutyCycleWindowMs()`, `getAirtimeBudgetFactor()` are all reachable
(public / protected-virtual, MyMesh is the subclass). No Dispatcher change needed. Decision-time budget is
an approximation of send-time budget — acceptable.

### 4. `examples/simple_repeater/MyMesh.cpp` — CLI (set/get symmetry is mandatory)
- In `handleFwdCommand()` `set` chain (~after line 1468, next to `flood.max.response`):
  `else if (memcmp(config, "fwd.scoped.reserve ", 19) == 0) { _fwd_prefs.scoped_reserve_pct =
   constrain(atoi(&config[19]), 0, 100); _fwd_prefs.save(_fs); strcpy(reply, "OK"); }`
- In the `get` chain (~after line 1510): `else if (memcmp(config, "fwd.scoped.reserve", 18) == 0)
   { sprintf(reply, "> %d", (int)_fwd_prefs.scoped_reserve_pct); }`
  (Matches the `get fwd.hashfilter.prob` symmetry precedent.)

### 5. Observability counters (runtime, NOT prefs) — mirror the existing `n_recv_flood` pattern
New `uint32_t` members on MyMesh + a `get` command exposing them (and optionally the Observer MQTT stats
frame). Counted at the `allowPacketForward` decision, estimated airtime:
- `fwd_scoped_pkts` / `fwd_scoped_airtime`  (scoped forwarded)
- `fwd_unscoped_pkts` / `fwd_unscoped_airtime`  (unscoped forwarded)
- `drop_unscoped_pkts` / `drop_unscoped_airtime`  (unscoped dropped by the reserve — the "saved" airtime)
Cost ≈ 24–40 B RAM, ~0.5–1 KB flash. Purpose: tune `pct` and prove the effect at the real node, since
CoreScope currently mis-measures scoped vs unscoped (its data is worthless for this; a CoreScope-side issue
may already be open — track separately).

**Lifecycle: SINCE-BOOT only.** The counters are RAM-only `MyMesh` members, reset to 0 on every reboot and
NOT persisted — deliberately: incrementing per-packet counters into flash would wear the nRF52 flash, and this
matches the Dispatcher's existing RAM-only stat counters (`n_recv_flood` etc.). For long-term / cumulative
stats, aggregate externally (observer polls `get fwd.scoped.stats`, or via MQTT) — an external aggregator sees
the reboot as a counter reset and keeps summing, so nothing is lost. (Shipped as 4 counters, airtime tracked
only on the drop path — see the status note at the top.)

## Test plan (bench, mirrors fwdfilter3/4 validation)
1. **CLI/persist:** `set fwd.scoped.reserve 40` → reboot → `get fwd.scoped.reserve` == 40; `/fwd_prefs`
   round-trips; old file without the tag loads as 0; over-range clamps to 100. (serial + RF-admin both.)
2. **Gate behaviour:** with `pct=100`, drive an unscoped flood → dropped; a scoped (transport) flood →
   forwarded. With `pct=0` → both forwarded (no regression). Verify via the drop counters + packet log.
3. **Budget-pressure:** artificially lower the budget (small `airtime_factor` window or a bench harness that
   drains `tx_budget_ms`) and confirm unscoped chokes at the reserve line while scoped keeps flowing.
4. **Safety:** confirm a scoped request→reply exchange completes with the gate at pct=100 (reply is scoped,
   path-return is direct → neither dropped).
5. Counters increment correctly and are readable via `get` (and MQTT if wired).

## Build / release
- Tag `repeater-1.16.0.fwdfilter5`. Build `pio run -e RAK_4631_repeater_debug` (bench) + prod env.
- HW-validate on the bench rig (Heltec companion + RAK repeat_debug), then remote-flash path
  (RF-admin `start ota` → BLE-DFU → OTAFix) as proven in fwdfilter3/4.
- Keep fork-private unless upstreaming (the reserve is a natural companion to mainline's region/transport
  flood + `REGION_DENY_FLOOD`; could be PR'd as the load-adaptive tier).

## Open / risks
- Bucket is hour-scale (cap ~30 min): the reserve bites under SUSTAINED load, brief unscoped bursts pass.
  This is desirable, but confirm the drain timescale matches the starvation you see in the field.
- Decision-time vs send-time budget skew — acceptable, but watch for edge behaviour at very low budget.
- Counters count forward-INTENT (allowPacketForward=true), not confirmed TX — note this in the `get` output.

See memory `project_fwd_airtime_reserve.md`; lineage in `project_hash_forward_filter.md` /
`FEATURE-fwdprefs-tlv.md`.
