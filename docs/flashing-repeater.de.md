# Repeater-Firmware flashen — Anleitung

*🇬🇧 [English version](./flashing-repeater.md) · ⚙️ [Forward-Filter-Handbuch](./forward-filter.de.md)*

Diese Anleitung beschreibt, wie du die ACETyr-Repeater-Firmware
(`repeater-v1.16.0.fwdfilterN`) auf ein Gerät bekommst — per USB und über Funk (OTA).

> Diese Firmware wird **nicht** über <https://flasher.meshcore.io> verteilt. Der offizielle Web-Flasher
> kennt nur die Mainline-Builds. Die Dateien hier kommen aus den
> [Releases dieses Forks](https://github.com/ACETyr/MeshCore/releases).

---

## Schritt 1 — Die richtige Datei herunterladen

Immer die **neueste** Release-Version nehmen ([Releases](https://github.com/ACETyr/MeshCore/releases)),
ältere enthalten behobene Fehler. Welche Datei du brauchst, hängt von Board **und** Flash-Weg ab:

| Board | USB-Flash | OTA (über Funk) |
|---|---|---|
| **RAK4631** (nRF52840) | `RAK_4631_repeater-…​.uf2` | `RAK_4631_repeater-…​.zip` |
| **Heltec V3** (ESP32-S3) | `Heltec_v3_repeater-…​-merged.bin` | `Heltec_v3_repeater-…​.bin` (**ohne** `merged`) |
| **SenseCAP Solar Node P1** (nRF52840) | `SenseCap_Solar_repeater-…​.uf2` | `SenseCap_Solar_repeater-…​.zip` |

> ⚠️ **Heltec V3: `merged` vs. nicht-`merged`.** Die `-merged.bin` enthält Bootloader, Partitionstabelle
> und Anwendung und wird per USB an Adresse `0x0` geschrieben. Die einfache `.bin` enthält nur die
> Anwendung und ist ausschließlich für OTA gedacht. Wer die beiden verwechselt, bekommt einen Knoten,
> der nicht mehr startet.

> ℹ️ **SenseCAP Solar Node P1** ist bisher nur **build-validiert, nicht auf echter Hardware getestet**
> — es stand kein Gerät zur Verfügung. Der Filter-Code ist boardunabhängig und übersetzt sauber, aber
> behandle diese Binaries als ungeprüft. Rückmeldungen von P1-Betreibern sind ausdrücklich willkommen.

---

## Schritt 2 — Flashen

### Weg A — RAK4631 / SenseCAP P1 per USB (`.uf2`)

Der einfachste und sicherste Weg. Ein UF2-Flash kann den Knoten nicht unbrauchbar machen.

1. Gerät per USB anstecken.
2. **Reset-Taste zweimal kurz hintereinander drücken.** Es erscheint ein USB-Laufwerk
   (bei RAK4631 heißt es `RAK4631`).
3. Die `.uf2`-Datei auf dieses Laufwerk kopieren.
4. Das Laufwerk verschwindet von selbst, das Gerät startet neu. Fertig.

Klappt der Doppel-Reset nicht: langsamer probieren (zwei getrennte Klicks, nicht ein Doppelklick),
anderes USB-Kabel testen — viele Kabel können nur laden, nicht Daten übertragen.

### Weg B — Heltec V3 per USB (`-merged.bin` an `0x0`)

**Mit esptool** (Python, `pip install esptool`):

```bash
esptool.py --chip esp32s3 --port COM5 --baud 921600 write_flash 0x0 Heltec_v3_repeater-v1.16.0.fwdfilter7-a57a106-merged.bin
```

Unter Linux/macOS statt `COM5` den passenden Port angeben (`/dev/ttyUSB0`, `/dev/cu.usbserial-…`).
Bei Verbindungsproblemen die Baudrate auf `115200` senken.

**Ohne Installation, im Browser:** <https://adafruit.github.io/Adafruit_WebSerial_ESPTool/> in Chrome
oder Edge öffnen, verbinden, die `-merged.bin` an Offset `0x0` auswählen und schreiben. Firefox und
Safari können kein WebSerial.

### Weg C — Über Funk, nRF52-Boards (RAK4631, SenseCAP P1)

Für Knoten, an die du nicht mehr physisch herankommst. Du brauchst BLE-Reichweite zum Gerät und
Admin-Zugang über Funk.

1. Die **`.zip`** des Releases auf das Telefon laden (nicht die `.uf2`).
2. App **nRF Device Firmware Update** installieren (iOS App Store / Google Play, Suchbegriff `nrf dfu`).
3. In der MeshCore-App per Remote-Administration am Repeater als Admin anmelden, im Command-Line-Tab
   `start ota` eingeben. Antwort `OK` bedeutet: Gerät ist im OTA-Modus.
4. In der DFU-App unter `Settings` die **Packet receipt notifications** aktivieren und
   **Number of Packets** auf `10` setzen (RAK4631; `8` funktioniert ebenfalls und ist für den T114 der
   richtige Wert). Ohne diese Einstellung brechen Uploads häufig ab.
5. Die ZIP-Datei auswählen, das Gerät aus der Liste wählen, `Upload` starten. Der Vorgang dauert
   einige Minuten.

Taucht das Gerät nicht in der Liste auf: `Force Scanning` in der DFU-App aktivieren, und `start ota`
noch einmal absetzen — der OTA-Modus läuft nach einiger Zeit ab.

> 💡 **Dringende Empfehlung für alles, was an einem Mast hängt:** vorher den
> [OTAFIX-Bootloader](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX) installieren. Er
> erkennt eine ungültige Anwendungs-Firmware und fällt selbsttätig in den OTA-DFU-Modus zurück, statt
> den Knoten tot am Mast zu lassen. Ein abgebrochener OTA-Flash ist damit kein Kletter-Einsatz mehr.
> Hintergrund: <https://blog.meshcore.io/2026/04/06/otafix-bootloader>

### Weg D — Über Funk, Heltec V3

1. Die **nicht**-`merged` `.bin` des Releases bereithalten.
2. Per Remote-Administration als Admin anmelden und `start ota` absetzen.
3. Auf ESP32-Geräten öffnet das einen WLAN-Hotspot namens **`MeshCore OTA`**. Mit Telefon oder Laptop
   verbinden.
4. Im Browser <http://192.168.4.1/update> aufrufen und die `.bin` hochladen.

Das setzt voraus, dass du in WLAN-Reichweite des Knotens bist — für einen Mast also meist nichts,
was vom Boden aus funktioniert.

---

## Schritt 3 — Prüfen, dass es geklappt hat

An der CLI:

```
ver
> v1.16.0.fwdfilter7-a57a106 (Build: …)
```

Stimmt die Versionsnummer mit dem Release überein, hat das Gerät die richtige Firmware. Gegenprobe,
dass die Fork-Funktionen tatsächlich da sind:

```
get fwd.hashfilter
> off prob=100
```

Antwortet die Node hier mit einem Fehler statt mit dem Status, läuft Mainline-Firmware ohne
Forward-Filter.

---

## Zugang zur CLI

Drei Wege, alle gleichwertig — die Forward-Filter-Befehle funktionieren auf jedem davon:

**Per USB, im Browser.** <https://config.meshcore.io> in Chrome oder Edge öffnen, Gerät per USB
verbinden, seriellen Port auswählen. Bequemster Weg für die Erstkonfiguration.

**Über Funk, aus der MeshCore-App.** Repeater als Admin hinzufügen (Remote Management), dann den
Command-Line-Tab benutzen. Das ist der Weg für Knoten im Feld — und der Grund, warum die Whitelist
Adverts und `ANON_REQ` grundsätzlich durchlässt: damit dieser Zugang auch bei fehlerhafter
Filterkonfiguration erhalten bleibt.

**Per Kommandozeile.** [`meshcore-cli`](https://github.com/fdlamotte/meshcore-cli) — für
Skripte, Massenkonfiguration und alles, was reproduzierbar sein soll.

Nach dem Flashen weiter im [Forward-Filter-Handbuch](./forward-filter.de.md).

---

## Was beim Flashen erhalten bleibt

Ein normaler Firmware-Flash — egal auf welchem Weg — lässt das Dateisystem und damit die
Konfiguration unangetastet: Node-Name, Admin-Passwort, Funk- und Regionseinstellungen sowie die
Geräteidentität (der Pubkey) bleiben erhalten. Der Knoten ist nach dem Neustart derselbe Knoten.

Zwei Ausnahmen:

- **Ein Flash-Erase löscht alles**, auch die Identität. Danach hat der Knoten einen neuen Pubkey und
  muss überall neu eingetragen werden. Nur machen, wenn du genau das willst.
- **Beim Update von `fwdfilter3` auf `fwdfilter4` oder neuer** wird die Filterkonfiguration einmalig
  zurückgesetzt (Umstellung auf die eigene `/fwd_prefs`-Datei). Whitelist- und Blacklist-Einträge
  danach neu setzen — am besten vorher `get fwd.whitelist` und `get fwd.block` abfragen und die
  Ausgabe aufheben. Alles andere bleibt erhalten.

## Zurück auf Mainline

Einfach die offizielle Repeater-Firmware von <https://flasher.meshcore.io> flashen, auf demselben Weg
wie oben. Die Filterkonfiguration in `/fwd_prefs` bleibt als verwaiste Datei liegen und wird von der
Mainline-Firmware ignoriert; die Filter sind damit wirkungslos. Flasht du später wieder eine
fwdfilter-Version, ist die alte Konfiguration wieder da.

---

## Wenn etwas nicht klappt

| Problem | Ursache / Abhilfe |
|---|---|
| Kein USB-Laufwerk nach Doppel-Reset | Zwei getrennte Klicks statt Doppelklick; anderes USB-Kabel (viele sind reine Ladekabel) |
| esptool findet den Port nicht | USB-Treiber (CP210x/CH340) fehlt; unter Linux fehlt die Berechtigung — Benutzer in Gruppe `dialout` |
| Browser sieht das Gerät nicht | WebSerial gibt es nur in Chrome und Edge, nicht in Firefox oder Safari |
| Heltec startet nach dem Flash nicht mehr | Vermutlich die nicht-`merged` `.bin` per USB geflasht. Die `-merged.bin` an `0x0` nachflashen |
| OTA bricht mittendrin ab | `Packet receipt notifications` aktivieren, `Number of Packets` auf 10; Bluetooth am Telefon aus- und einschalten; Gerät in den Bluetooth-Einstellungen entfernen und neu koppeln |
| Gerät erscheint nicht in der DFU-App | `start ota` erneut absetzen (der Modus läuft ab), `Force Scanning` aktivieren |
| Knoten nach OTA-Abbruch tot | Ohne OTAFIX-Bootloader hilft nur USB vor Ort. Genau deshalb OTAFIX **vorher** installieren |

Bleibt es dabei: [Issue aufmachen](https://github.com/ACETyr/MeshCore/issues) — mit Board, Release,
Flash-Weg und der genauen Fehlermeldung.
