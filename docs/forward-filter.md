# Repeater forward filter — operator manual

*🇩🇪 [Deutsche Fassung](./forward-filter.de.md) (primary) · 📻 [Flashing guide](./flashing-repeater.md)*

This manual documents **every** forward-filter feature of the ACETyr repeater firmware
(`repeater-v1.16.0.fwdfilterN`) in one place. It supersedes the per-release descriptions — from now on
the release notes only record *what changed*, this document describes *what the firmware does*.

---

## Read this first

**Every filter ships disabled.** A freshly flashed node behaves exactly like a stock MeshCore 1.16.0
repeater. Nothing happens until you switch something on yourself.

All filters act **locally on this one node** — there is no protocol change and no coordination with
other nodes. Mixed-firmware networks are fine, and so is a single filtered node in an otherwise
untouched network.

Everything is driven from the **admin CLI**: locally over USB, or remotely over the air (remote
management in the MeshCore app, admin login required). See
[CLI access](./flashing-repeater.md#cli-access).

---

## Command reference

| Command | Values | Default | Effect |
|---|---|---|---|
| `set fwd.hashfilter` | `off` · `advert` · `all` | `off` | Stop forwarding packets with a 1-byte path hash |
| `set fwd.hashfilter.prob` | `0`–`100` | `100` | Probability that a match is actually dropped |
| `set fwd.block.add` | `<64-hex> [prune\|advert\|both]` | action `prune` | Add a node to the policy table (max. 16) |
| `set fwd.block.del` | `<hex prefix>` | — | Remove all entries matching the prefix |
| `set fwd.block.clear` | — | — | Empty the policy table |
| `set fwd.whitelist` | `on` · `off` | `off` | Enforce the last-hop whitelist |
| `set fwd.whitelist.0hop` | `allow` · `drop` | `allow` | How to treat floods that have not taken a hop yet |
| `set fwd.whitelist.add` | `<64-hex>` | — | Add a node to the whitelist (max. 16) |
| `set fwd.whitelist.del` | `<hex prefix>` | — | Remove all entries matching the prefix |
| `set fwd.whitelist.clear` | — | — | Empty the whitelist |
| `set fwd.scoped.reserve` | `0`–`100` | `0` | Percent of the airtime allowance (60 s window) kept free for scoped traffic |
| `set flood.max.request` | `0`–`64` | `64` | Hop cap for flooded REQUEST packets |
| `set flood.max.anon.request` | `0`–`64` | `64` | Hop cap for flooded ANON_REQUEST packets |
| `set flood.max.response` | `0`–`64` | `64` | Hop cap for flooded RESPONSE packets |

Every `set` has a matching `get`:

| Command | Example output |
|---|---|
| `get fwd.hashfilter` | `> advert prob=75` |
| `get fwd.hashfilter.prob` | `> 75` |
| `get fwd.block` | `> 2 entries \| a1b2c3d4e5f6 P \| 9988776655ff PA` |
| `get fwd.whitelist` | `> on 0hop=allow 3 entries \| a1b2c3d4e5f6 \| …` |
| `get fwd.scoped.reserve` | `> 40` |
| `get fwd.scoped.stats` | `> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms air=1180/6000ms/60s` |
| `get flood.max.request` | `> 64` |

List output shows only the **6-byte prefix** of each pubkey; in `get fwd.block`, `P` = *prune* and
`A` = *advert*. Long lists are truncated in the display at roughly 140 characters — the entries
themselves stay active, only the output is cut short.

---

## Why this exists

Two problems that show up in large, long-running networks:

**1-byte path hashes.** The path hash is a prefix of the node pubkey. One byte leaves 256 possible
values — in a network with more than a few dozen nodes those collide by necessity. Such a hop can no
longer be attributed to a specific node, and direct-route matching can resolve ambiguously. The result
is relay traffic nobody can trace and paths that take a wrong turn.

**Unscoped floods.** An unconfigured node always transmits without a region code — that is the default
and legacy behaviour, and it is intentional for compatibility. In practice this traffic becomes the
majority in grown networks and crowds delivery between correctly configured repeaters out of the
airtime budget.

These filters give an operator tools against both — without a protocol change everyone would have to
adopt.

---

## Stage 1 — hash-size filter

Drops packets whose path hash is only one byte wide.

```
set fwd.hashfilter off      # off (default)
set fwd.hashfilter advert   # stop forwarding adverts with a 1-byte hash
set fwd.hashfilter all      # stop forwarding all 1-byte flood and direct traffic
```

`advert` is the gentle first step: adverts are the traffic that fills routing tables across the
network with ambiguous entries. `all` covers payload traffic as well.

The probability lets you dose the effect instead of switching hard:

```
set fwd.hashfilter.prob 100   # every match is dropped (default)
set fwd.hashfilter.prob 75    # three out of four matches are dropped
set fwd.hashfilter.prob 0     # nothing is dropped (filter effectively inert)
```

The probability applies to **all** matches of the selected mode, including adverts in `advert` mode. A
value below 100 is useful when you want to increase pressure on affected neighbours without cutting
them off entirely — their packets still get through, just less reliably.

---

## Stage 2 — per-pubkey policy table

A table of up to 16 nodes, each with one of three actions.

```
set fwd.block.add <64-hex-pubkey> prune    # path prune (default if no action given)
set fwd.block.add <64-hex-pubkey> advert   # do not forward this node's adverts
set fwd.block.add <64-hex-pubkey> both     # both of the above
```

**`prune`** drops flood copies whose path traversed the named node. The test runs **before**
duplicate suppression — a copy of the same packet arriving via a different path can still win. This
steers paths rather than destroying packets: "don't go via X, take the other route". It is only
reliable at multibyte hash sizes, because the path hop is otherwise ambiguous.

**`advert`** stops forwarding adverts originated by that node. This compares the full pubkey exactly,
so it works at any hash size.

Removing and inspecting:

```
set fwd.block.del a1b2c3d4      # prefix is enough, removes all matches -> "OK (1 removed)"
set fwd.block.clear             # empty the table
get fwd.block                   # show contents
```

Adding a pubkey that is already present replaces its action; no duplicate entry is created.

---

## Stage 3 — last-hop whitelist

The strictest filter: a flood is only forwarded if its **immediate sender** — the last hop in the path
— is on the allow list. Everything else is dropped.

```
set fwd.whitelist.add <64-hex-pubkey>   # add backbone neighbours (max. 16)
set fwd.whitelist.del a1b2c3d4          # prefix is enough
set fwd.whitelist.clear
get fwd.whitelist
set fwd.whitelist on|off                # enforcement (default: off)
set fwd.whitelist.0hop allow|drop       # default: allow
```

`0hop` covers floods that have not taken a hop yet, i.e. heard directly from the originator. There is
no "last hop" to check, so they need their own rule. The default `allow` keeps directly heard nodes
flowing.

**So you cannot lock yourself out**, three packet classes are always exempt from the whitelist:
adverts, `ANON_REQ`, and floods addressed to this node itself. Remote admin login therefore still
works even with a wrong whitelist.

The last hop is matched at the packet's hash size. With a 1-byte hash that means 256 possible values,
and any foreign node sharing the prefix passes the whitelist too. **This is why you enable
`fwd.hashfilter all` first** — the whitelist then matches at multibyte width and the comparison is
meaningful.

---

## Stage 4 — airtime reserve for scoped traffic

```
set fwd.scoped.reserve 0     # off (default)
set fwd.scoped.reserve 40    # keep 40 % of the airtime allowance free for scoped traffic
```

Keeps that percentage of **this node's transmit airtime** free for **scoped** (region-coded) flood
traffic. If the remaining allowance is no longer enough for another unscoped packet without eating
into the reserve, that packet is dropped; scoped floods and all direct traffic always pass regardless.

**The reference is a 60-second window, not the hourly budget.** The allowance per window follows
directly from the configured duty cycle:

```
allowance per window = 60 s × duty cycle
```

At `set dutycycle 10` that is 6000 ms per window, roughly twelve packets at the SF8 preset. A 40 %
reserve keeps 2400 ms of it free: unscoped floods are dropped once this node has transmitted enough
within the current window that the remainder no longer covers another unscoped packet.

> **Why not the duty-cycle budget itself?** Because nothing can be measured with it. The Dispatcher's
> token bucket runs over a full hour and carries roughly 360,000 ms of slack at a legal 10 %. A flood
> storm of twenty relays costs ~10,000 ms and disappears into it without trace. Measured on the bench:
> bucket at 359,325/360,000 ms (99.8 % full) while the node was relaying a busy mesh at 0.995 % actual
> duty against a 10 % limit. A percentage threshold on a permanently full bucket can only trip at
> 100 %. Up to and including `fwdfilter7` the setting was therefore effectively a switch: 0 = off,
> 100 = drop every unscoped flood, everything in between inert. The short window is the actual fix —
> not a different threshold.

Only this node's **transmit budget** is reserved, not the RF channel.

> **Set this carefully at very low duty cycles.** The allowance shrinks with the duty cycle, but a
> single packet is still ~500 ms. At `set dutycycle 1` only 600 ms per window is available — one
> packet nearly fills the whole allowance, and any reserve above 0 will then drop practically every
> unscoped flood regardless of load. Arithmetically correct, but rarely what you want.

Checking the effect:

```
get fwd.scoped.stats
> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms air=1180/6000ms/60s
```

- `fwd_scoped` / `fwd_unscoped` — forwarded floods, split by scope
- `drop_unscoped` — floods dropped by the reserve
- `saved_air` — airtime saved as a result, in milliseconds
- `air` — transmit airtime used in the current window / allowance / window length. This is the gate's
  input: if the first value sits well below the second, the reserve does not engage, whatever it is
  set to

The counters count the **forwarding decision**, not confirmed transmission. They live in RAM and
**reset to 0 on every reboot** — deliberately, since a per-packet counter in flash would wear out the
nRF52. For long-term statistics, poll the value externally and keep summing; a reboot simply looks
like a counter reset.

---

## Per-payload flood hop caps

```
set flood.max.request 64        # REQUEST      (default 64)
set flood.max.anon.request 64   # ANON_REQUEST (default 64)
set flood.max.response 64       # RESPONSE     (default 64)
```

Caps how far flooded packets of these types may travel. The default of 64 is the maximum and therefore
a no-op. These are meant against misconfigured automation traffic that crosses the entire network when
it should stay local.

> ⚠️ **Leave these at 64 on backbone high sites.** RF admin login rides on
> `ANON_REQUEST`/`RESPONSE`. Setting these too low on a remote node cuts exactly the traffic you would
> need to reach it again.

(These three settings come from mainline PR #2797 and are not part of the forward filter proper, but
they are stored in the same `/fwd_prefs` file and driven from the same CLI.)

---

## Common configurations

**"I just want to observe for now."**
Do nothing. The shipped state does not filter. Polling `get fwd.scoped.stats` gives you the
scoped/unscoped split at your site even with every filter off.

**"Make the 1-byte adverts stop."**
```
set fwd.hashfilter advert
```
The usual first step. Adverts only; payload traffic is untouched.

**"One specific node is flooding me."**
```
set fwd.block.add <pubkey> both
get fwd.block
```

**"The path via node X is bad, I want the other one."**
```
set fwd.block.add <pubkey-of-X> prune
```
No data loss — copies via other paths still win.

**"My backbone node should only relay for the backbone."**
See [Safe deployment](#safe-deployment) — this is the case where the order matters.

**"Unscoped traffic is eating my airtime."**
```
set fwd.scoped.reserve 40
```
Check `get fwd.scoped.stats` after a few hours and adjust, watching `air=` to see whether the node
ever gets near its allowance at all. 100 drops unscoped floods as soon as there
is any budget pressure at all.

---

## Safe deployment

For the whitelist (stage 3) — the only filter that can functionally remove a node from the network.
**Nothing is enforced until step 4**; steps 1–3 only populate and verify and are safe to run on a live
node.

1. **Identify the real backbone neighbours** — from active relay adjacency or TRACE, **not** from the
   device's neighbour table. That lists nodes you *hear*, not the ones that actually relay your
   traffic. This is the most common mistake here.

2. **With the whitelist still off**, add each confirmed pubkey:
   ```
   set fwd.whitelist.add <64hex>
   ```
   This only fills the table; forwarding is unchanged.

3. **Verify the table** — `get fwd.whitelist`. Is it exactly what you expect?

4. **Only now enable enforcement**, in this order:
   ```
   set fwd.hashfilter all
   set fwd.whitelist on
   ```
   Hash filter first, so the whitelist matches at multibyte width instead of collision-prone 1-byte.
   Leave `0hop allow` (the default) until you have confirmed the backbone still relays through this
   node.

**Rollback — at any time:**
```
set fwd.whitelist off
set fwd.hashfilter off
```

> ⚠️ **Careful with remote and masthead nodes.** With the whitelist active the node stops relaying for
> any non-whitelisted last hop. Admin *login* still works (adverts and `ANON_REQ` are always exempt),
> but multi-hop paths **through** this node to nodes beyond it break if the whitelist is incomplete.
> Before step 4 on a site with no backhaul, make sure you have a way back: local USB/BLE access, or a
> node you can still reach.

---

## Where the settings live

All `fwd.*` and `flood.max.*` settings are stored in a dedicated file **`/fwd_prefs`** on the node's
filesystem, in a self-describing TLV format. The mainline settings file `/com_prefs` is untouched and
stays byte-for-byte identical to mainline.

That is not cosmetic. If the filter fields were written into `/com_prefs`, a future mainline merge that
inserts a field there could shift offsets — and then radio or region settings get silently reinterpreted
as filter values. Keeping them separate makes that impossible by construction.

Practical consequences:

- **Switching between fork and mainline firmware** only loses the filter configuration. Radio, region
  and identity settings survive.
- **Older firmware reading a newer `/fwd_prefs`** ignores unknown fields. A missing field means its
  default — i.e. "off".
- **Upgrading from fwdfilter3 to fwdfilter4 or newer** resets the filter configuration once (the move
  from `/com_prefs` to `/fwd_prefs`). Re-add whitelist and blacklist entries afterwards. Radio and
  region settings are preserved. From fwdfilter4 onward the configuration survives every upgrade.

---

## Version history

| Version | Date | What it added |
|---|---|---|
| `fwdfilter1` | 2026-06-16 | Stage 1 (hash-size filter) + stage 2 (policy table) |
| `fwdfilter2` | 2026-06-17 | Stage 3 (last-hop whitelist) — **superseded, do not use** |
| `fwdfilter3` | 2026-06-17 | Fix: reply-buffer overflow in `get fwd.whitelist`/`fwd.block` over RF |
| `fwdfilter4` | 2026-06-20 | Dedicated `/fwd_prefs` file · replanted on mainline · `flood.max.*` · `get fwd.hashfilter.prob` |
| `fwdfilter5` | 2026-06-21 | New build target SenseCAP Solar Node P1 (no functional change) |
| `fwdfilter6` | 2026-07-10 | Stage 4 (`fwd.scoped.reserve`) + `get fwd.scoped.stats` |
| `fwdfilter7` | 2026-07-13 | Fix: airtime estimate guarded against error codes · version string gained a leading `v` |

**Recommendation: always run the newest release.** Every older one carries at least one of the bugs
fixed above.

---

## Limits and known caveats

- **16 entries each** for the whitelist and the policy table. Enough for backbone adjacencies, not for
  network-wide lists.
- **List output is truncated.** `get fwd.whitelist`/`get fwd.block` will not show every entry on a full
  table. The entries are still active.
- **Statistics counters are volatile** (see stage 4) and count the forwarding decision, not confirmed
  transmission.
- **The airtime reserve works on an hour timescale.** The duty-cycle bucket needs sustained load to
  drain. Short bursts of unscoped traffic pass even at a high reserve — by design.
- **Path prune is unreliable at 1-byte hashes**, because the path hop is ambiguous. Reliable at
  multibyte hash sizes.

Please report bugs and feedback as an [issue](https://github.com/ACETyr/MeshCore/issues).
