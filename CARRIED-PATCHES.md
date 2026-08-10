# Carried upstream patches

Changes in this fork that originate **upstream** and are carried ahead of mainline merging them.
Listed so that provenance is visible without diffing, and so each one has a written removal
condition instead of silently becoming permanent fork divergence.

Fork-original work (the forward filter, `/fwd_prefs`, the airtime guard) is not listed here — see
`FEATURE-*.md` and `docs/forward-filter.md` for those.

---

## Active

### meshcore-dev/MeshCore#3137 — `fem_rxgain` bound to the wrong field

| | |
|---|---|
| Upstream author | agessaman |
| Upstream PR | https://github.com/meshcore-dev/MeshCore/pull/3137 (OPEN) |
| Files | `src/helpers/CommonCLI.h` (one line) · `test/test_node_prefs_fem/` (fork-original) |
| Related | upstream issue #3145 |

**What it fixes.** `NodePrefs::RadioPrefs::structure()` bound both JSON keys to the same member:

```cpp
def("rxgain",     _parent->rx_boosted_gain);
def("fem_rxgain", _parent->rx_boosted_gain);   // should be radio_fem_rxgain
```

so `radio_fem_rxgain` was never serialised and `set radio.fem.rxgain` did not survive a reboot, even
though the CLI handler does call `savePrefs()`. `rx_boosted_gain` is the SX1262's own boosted-RX
register; `radio_fem_rxgain` is the external FEM LNA — two different settings. The one-line fix is
agessaman's, from #3137.

**Why carried.** It is reachable from this fork's source. We publish binaries only for RAK4631,
Heltec V3 and SenseCAP P1 — none of which can control a FEM LNA — but `[env:heltec_v4_repeater]` is
present in the tree and **builds from this branch** (verified 2026-08-10), and
`HeltecV4Board::canControlLoRaFemLna()` returns true on a V4.3. Anyone compiling this fork for a
Heltec V4 gets the forward filter and, without this patch, the upstream #3145 behaviour with no way
to persist the workaround.

**The test is ours, the fix is not.** `test/test_node_prefs_fem/test_node_prefs_fem.cpp` is
fork-original: it drives the real `NodePrefs` and asserts both keys round-trip independently with
opposite values, so an aliasing binding cannot pass by coincidence. Verified 2026-08-10 — 2/2 fail on
`dev` @ `f6c25e6a` and on this branch before the fix, 2/2 pass with it, and 2/2 pass against #3137 @
`c58c9b2f` with that PR's full native suite green. Both the verification and a separate observation
about `examples/companion_radio/NodePrefs.h` were posted to #3137 on 2026-08-10.

**Removal condition.** Drop the `CommonCLI.h` line when mainline merges #3137 or an equivalent fix —
the replant should dedupe it to identical text. **Keep the test**; it must still pass afterwards, and
it is the check that tells us the fix actually arrived with the replant.

---

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

### meshcore-dev/MeshCore#2797 — per-payload flood hop caps

Folded into the fork at `628b7689` (fwdfilter4) with its `atoi` off-by-one fixed. **Landed in
mainline 1.17** as `flood_max` / `flood_max_unscoped` / `flood_max_advert` with the same 64/64/8
defaults, so the 1.17 replant deduplicated it automatically — the fork copy and mainline's merged as
identical text. No longer carried.

The fork's own additional caps (`fwd.flood.max.request`, `.anon_request`, `.response`) live in
`/fwd_prefs` and are unrelated to #2797.
