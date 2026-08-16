# Carried upstream patches

Changes in this fork that originate **upstream** and are carried ahead of mainline merging them.
Listed so that provenance is visible without diffing, and so each one has a written removal
condition instead of silently becoming permanent fork divergence.

Fork-original work (the forward filter, `/fwd_prefs`, the airtime guard) is not listed here — see
`FEATURE-*.md` and `docs/forward-filter.md` for those.

---

## Active

### meshcore-dev/MeshCore#2933 — median noise-floor estimator

| | |
|---|---|
| Upstream author | usrflo |
| Upstream PR | https://github.com/meshcore-dev/MeshCore/pull/2933 (OPEN) |
| Applied as | `a028adcb`, carried through the 1.17 replant |
| Files | `src/helpers/radiolib/RadioLibWrappers.{h,cpp}` |
| Fork issue | ACETyr/MeshCore#3 |

**The patch is usrflo's work, applied essentially verbatim** — the `sortInt16()` helper, the
explanatory comments and the `_floor_block_ready` flag are all his. This fork contributed only an
independent bench reproduction, posted to the PR on 2026-08-09.

**What it fixes.** `RadioLibWrapper::loop()` admitted a noise-floor sample only if
`rssi < _noise_floor + SAMPLING_THRESHOLD`, a one-way ratchet: each 64-sample block mean came from a
lower-truncated set, walked down to the -120 clamp and stuck there, leaving the RSSI-margin LBT
permanently over-sensitive. The patch accepts every idle sample and reduces the block to its median,
which recovers in both directions.

**Why carried rather than waited out.** Reproduced on our own hardware and verified fixed: 250
zero-hop adverts at 0.4 s wedges a stock node at -120 in 18.9 s; the patched build under identical
stimulus holds -104 with 56 blocks in 199 s, 15 up / 14 down. Mainline 1.17 and current `dev` both
still ship the ratchet.

**Removal condition.** Drop this patch when mainline merges a fix for the ratchet, then re-verify on
the bench rig before release. Two competing PRs are open and neither has a maintainer review:

- **#2933** (usrflo) — what we carry. +42/-22 across 2 files.
- **#2842** (yg-ht) — same root cause, much broader: absolute clamps, `noise.sample.ms` /
  `noise.window.secs` / `noise.clamp.low` / `noise.clamp.high`, a `stats-noise` CLI, unit tests and
  VNA cross-validation. +1235/-61 across 22 files.

If **#2842** wins, this is not a clean revert — it rewrites the estimator our patch touches. Expect
to drop `a028adcb`'s hunks wholesale and take upstream's version, then re-run the bench stimulus,
because #2842's absolute clamps (`noise.clamp.low` default -125) interact with the -120 behaviour we
tested against.

**Attribution is owed and unpaid.** `a028adcb` is authored by this fork with no `Co-authored-by:`
trailer, and it is already pushed, so amending would rewrite published history. Decision
(2026-08-09): settle it in the **release notes** instead. Any release carrying this patch — starting
with the first 1.17-based one — must credit usrflo and link #2933 by name in the notes, in both the
German and English text. This is not optional garnish; it is the only place the credit now appears
for anyone flashing the firmware.

Do **not** re-submit this patch upstream under fork authorship. It is already filed as #2933.

---

## Resolved

### meshcore-dev/MeshCore#3137 — `fem_rxgain` bound to the wrong field

Upstream author agessaman. The fork carried the one-line `CommonCLI.h` fix because
`[env:heltec_v4_repeater]` builds from this branch and `HeltecV4Board::canControlLoRaFemLna()`
returns true on a V4.3, so the bug was reachable from our source even though we publish no V4
binaries.

**Merged upstream 2026-08-12**, released in **1.17.1** as `23066573` (part of #3137), which also adds
`fem_txgain`. The 1.17.1 replant deduplicated our line to identical text exactly as the removal
condition foresaw — the only conflict was upstream's *additional* `fem_txgain` line, taken as-is.
Upstream separately disabled the equivalent load/save in `examples/companion_radio/NodePrefs.h`
(`890a2e2c`, `#if 0` "these cannot be set (yet)") and disabled its own round-trip test with it; that
is companion-side and does not affect this branch. Related issue #3145 is closed.

**The test stays.** `test/test_node_prefs_fem/` is fork-original and now guards mainline's own fix:
it passed against 1.17.1 in the replant run (42/42 native). Do not remove it — it is what would tell
us if a future mainline change re-aliased the binding.

---

### meshcore-dev/MeshCore#2797 — per-payload flood hop caps

Folded into the fork at `628b7689` (fwdfilter4) with its `atoi` off-by-one fixed. **Landed in
mainline 1.17** as `flood_max` / `flood_max_unscoped` / `flood_max_advert` with the same 64/64/8
defaults, so the 1.17 replant deduplicated it automatically — the fork copy and mainline's merged as
identical text. No longer carried.

The fork's own additional caps (`fwd.flood.max.request`, `.anon_request`, `.response`) live in
`/fwd_prefs` and are unrelated to #2797.
