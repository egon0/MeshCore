# Flashing the repeater firmware

*🇩🇪 [Deutsche Fassung](./flashing-repeater.de.md) (primary) · ⚙️ [Forward-filter manual](./forward-filter.md)*

This guide covers how to get the ACETyr repeater firmware (`repeater-v1.16.0.fwdfilterN`) onto a
device — over USB and over the air.

> This firmware is **not** distributed via <https://flasher.meshcore.io>. The official web flasher only
> knows the mainline builds. The files here come from this fork's
> [releases](https://github.com/ACETyr/MeshCore/releases).

---

## Step 1 — download the right file

Always take the **newest** release ([releases](https://github.com/ACETyr/MeshCore/releases)); older
ones contain bugs that have since been fixed. Which file you need depends on the board **and** the
flashing route:

| Board | USB flash | OTA (over the air) |
|---|---|---|
| **RAK4631** (nRF52840) | `RAK_4631_repeater-…​.uf2` | `RAK_4631_repeater-…​.zip` |
| **Heltec V3** (ESP32-S3) | `Heltec_v3_repeater-…​-merged.bin` | `Heltec_v3_repeater-…​.bin` (**no** `merged`) |
| **SenseCAP Solar Node P1** (nRF52840) | `SenseCap_Solar_repeater-…​.uf2` | `SenseCap_Solar_repeater-…​.zip` |

> ⚠️ **Heltec V3: `merged` vs. non-`merged`.** The `-merged.bin` contains bootloader, partition table
> and application and is written to address `0x0` over USB. The plain `.bin` contains only the
> application and is for OTA exclusively. Mixing the two up gives you a node that will not boot.

> ℹ️ **SenseCAP Solar Node P1** is **build-validated only, not hardware-tested** — no device was
> available. The filter code is board-agnostic and compiles cleanly, but treat these binaries as
> unverified. Feedback from P1 operators is very welcome.

---

## Step 2 — flash it

### Route A — RAK4631 / SenseCAP P1 over USB (`.uf2`)

The simplest and safest route. A UF2 flash cannot brick the node.

1. Connect the device over USB.
2. **Press the reset button twice in quick succession.** A USB drive appears (named `RAK4631` on the
   RAK4631).
3. Copy the `.uf2` file onto that drive.
4. The drive disappears on its own and the device reboots. Done.

If the double reset does not work: try slower (two separate clicks, not a double-click), and try
another USB cable — many cables can only charge, not carry data.

### Route B — Heltec V3 over USB (`-merged.bin` at `0x0`)

**With esptool** (Python, `pip install esptool`):

```bash
esptool.py --chip esp32s3 --port COM5 --baud 921600 write_flash 0x0 Heltec_v3_repeater-v1.16.0.fwdfilter7-a57a106-merged.bin
```

On Linux/macOS use the appropriate port instead of `COM5` (`/dev/ttyUSB0`, `/dev/cu.usbserial-…`). If
the connection is unstable, drop the baud rate to `115200`.

**Without installing anything, in the browser:** open
<https://adafruit.github.io/Adafruit_WebSerial_ESPTool/> in Chrome or Edge, connect, select the
`-merged.bin` at offset `0x0` and write. Firefox and Safari do not support WebSerial.

### Route C — over the air, nRF52 boards (RAK4631, SenseCAP P1)

For nodes you can no longer reach physically. You need BLE range to the device and admin access over
the air.

1. Get the release **`.zip`** onto your phone (not the `.uf2`).
2. Install the **nRF Device Firmware Update** app (iOS App Store / Google Play, search `nrf dfu`).
3. In the MeshCore app, log in to the repeater as admin via remote management and enter `start ota` in
   the command line tab. A reply of `OK` means the device is in OTA mode.
4. In the DFU app's `Settings`, enable **Packet receipt notifications** and set **Number of Packets**
   to `10` (RAK4631; `8` also works, and is the right value for the T114). Without this, uploads
   frequently abort.
5. Select the ZIP file, pick the device from the list and start `Upload`. It takes a few minutes.

If the device does not appear in the list: enable `Force Scanning` in the DFU app and issue
`start ota` again — OTA mode times out after a while.

> 💡 **Strongly recommended for anything on a mast:** install the
> [OTAFIX bootloader](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX) first. It detects an
> invalid application firmware and falls back to OTA DFU mode by itself, instead of leaving the node
> dead on the mast. An aborted OTA flash then stops being a climbing job. Background:
> <https://blog.meshcore.io/2026/04/06/otafix-bootloader>

### Route D — over the air, Heltec V3

1. Have the **non**-`merged` `.bin` of the release ready.
2. Log in as admin via remote management and issue `start ota`.
3. On ESP32 devices this opens a Wi-Fi hotspot named **`MeshCore OTA`**. Connect with a phone or
   laptop.
4. Open <http://192.168.4.1/update> in a browser and upload the `.bin`.

This requires Wi-Fi range to the node — for a masthead node, usually not something that works from the
ground.

---

## Step 3 — verify it worked

At the CLI:

```
ver
> v1.16.0.fwdfilter7-a57a106 (Build: …)
```

If the version matches the release, the device has the right firmware. Cross-check that the fork
features are actually present:

```
get fwd.hashfilter
> off prob=100
```

If the node answers with an error instead of a status, it is running mainline firmware without the
forward filter.

---

## CLI access

Three routes, all equivalent — the forward-filter commands work on any of them:

**Over USB, in the browser.** Open <https://config.meshcore.io> in Chrome or Edge, connect the device
over USB, select the serial port. The most convenient route for initial setup.

**Over the air, from the MeshCore app.** Add the repeater as an admin (remote management), then use
the command line tab. This is the route for field nodes — and the reason the whitelist always lets
adverts and `ANON_REQ` through: so this access survives a broken filter configuration.

**From the command line.** [`meshcore-cli`](https://github.com/fdlamotte/meshcore-cli) — for scripts,
bulk configuration, and anything that needs to be reproducible.

After flashing, continue with the [forward-filter manual](./forward-filter.md).

---

## What survives a flash

A normal firmware flash — by any route — leaves the filesystem and therefore the configuration alone:
node name, admin password, radio and region settings, and the device identity (the pubkey) are
preserved. After the reboot it is the same node.

Two exceptions:

- **A flash erase wipes everything**, including the identity. The node then has a new pubkey and has to
  be re-registered everywhere. Only do this if that is exactly what you want.
- **Upgrading from `fwdfilter3` to `fwdfilter4` or newer** resets the filter configuration once (the
  move to the dedicated `/fwd_prefs` file). Re-add whitelist and blacklist entries afterwards — best
  to run `get fwd.whitelist` and `get fwd.block` beforehand and keep the output. Everything else is
  preserved.

## Going back to mainline

Just flash the official repeater firmware from <https://flasher.meshcore.io> via the same routes as
above. The filter configuration in `/fwd_prefs` stays behind as an orphaned file and is ignored by
mainline firmware, so the filters are inert. Flash a fwdfilter build again later and the old
configuration is back.

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| No USB drive after double reset | Two separate clicks, not a double-click; try another USB cable (many are charge-only) |
| esptool cannot find the port | Missing USB driver (CP210x/CH340); on Linux a permissions issue — add your user to `dialout` |
| Browser does not see the device | WebSerial exists only in Chrome and Edge, not Firefox or Safari |
| Heltec will not boot after flashing | Probably the non-`merged` `.bin` flashed over USB. Re-flash the `-merged.bin` at `0x0` |
| OTA aborts partway through | Enable `Packet receipt notifications`, set `Number of Packets` to 10; toggle Bluetooth on the phone; forget the device in Bluetooth settings and re-pair |
| Device not listed in the DFU app | Issue `start ota` again (the mode times out), enable `Force Scanning` |
| Node dead after a failed OTA | Without the OTAFIX bootloader, only on-site USB helps. Which is exactly why you install OTAFIX **first** |

If it persists: [open an issue](https://github.com/ACETyr/MeshCore/issues) — with board, release,
flashing route, and the exact error message.
