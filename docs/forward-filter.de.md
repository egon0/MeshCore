# Forward-Filter für Repeater — Handbuch

*🇬🇧 [English version](./forward-filter.md) · 📻 [Flash-Anleitung](./flashing-repeater.de.md)*

Dieses Handbuch beschreibt **alle** Forward-Filter-Funktionen der ACETyr-Repeater-Firmware
(`repeater-v1.16.0.fwdfilterN`) an einer Stelle. Es ersetzt die über die einzelnen Releases verteilten
Beschreibungen — die Release-Notes dokumentieren ab jetzt nur noch, *was sich geändert hat*, dieses
Dokument beschreibt, *was das Gerät kann*.

---

## Das Wichtigste zuerst

**Alle Filter sind ab Werk ausgeschaltet.** Eine frisch geflashte Node verhält sich exakt wie ein
Standard-MeshCore-1.16.0-Repeater. Es passiert nichts, solange du nicht selbst etwas einschaltest.

Die Filter greifen ausschließlich **lokal auf diesem einen Knoten** — es gibt keine Protokolländerung,
keine Absprache mit anderen Nodes. Ein Netz aus gemischter Firmware ist unproblematisch, ein einzelner
gefilterter Knoten in einem sonst unveränderten Netz genauso.

Bedient wird alles über die **Admin-CLI**: lokal per USB oder aus der Ferne über Funk
(Remote-Administration in der MeshCore-App, Admin-Login erforderlich). Siehe
[Zugang zur CLI](./flashing-repeater.de.md#zugang-zur-cli).

---

## Alle Befehle auf einen Blick

| Befehl | Werte | Standard | Wirkung |
|---|---|---|---|
| `set fwd.hashfilter` | `off` · `advert` · `all` | `off` | Weiterleitung von Paketen mit 1-Byte-Path-Hash unterbinden |
| `set fwd.hashfilter.prob` | `0`–`100` | `100` | Mit welcher Wahrscheinlichkeit ein Treffer tatsächlich verworfen wird |
| `set fwd.block.add` | `<64-hex> [prune\|advert\|both]` | Aktion `prune` | Knoten in die Policy-Tabelle aufnehmen (max. 16) |
| `set fwd.block.del` | `<hex-Präfix>` | — | Einträge mit passendem Präfix entfernen |
| `set fwd.block.clear` | — | — | Policy-Tabelle leeren |
| `set fwd.whitelist` | `on` · `off` | `off` | Last-Hop-Whitelist scharf schalten |
| `set fwd.whitelist.0hop` | `allow` · `drop` | `allow` | Umgang mit Floods, die noch keinen Hop hinter sich haben |
| `set fwd.whitelist.add` | `<64-hex>` | — | Knoten in die Whitelist aufnehmen (max. 16) |
| `set fwd.whitelist.del` | `<hex-Präfix>` | — | Einträge mit passendem Präfix entfernen |
| `set fwd.whitelist.clear` | — | — | Whitelist leeren |
| `set fwd.scoped.reserve` | `0`–`100` | `0` | Prozent des TX-Airtime-Budgets für Scoped-Traffic reservieren |
| `set flood.max.request` | `0`–`64` | `64` | Hop-Limit für geflutete REQUEST-Pakete |
| `set flood.max.anon.request` | `0`–`64` | `64` | Hop-Limit für geflutete ANON_REQUEST-Pakete |
| `set flood.max.response` | `0`–`64` | `64` | Hop-Limit für geflutete RESPONSE-Pakete |

Zu jedem `set` gibt es ein passendes `get`:

| Befehl | Beispielausgabe |
|---|---|
| `get fwd.hashfilter` | `> advert prob=75` |
| `get fwd.hashfilter.prob` | `> 75` |
| `get fwd.block` | `> 2 entries \| a1b2c3d4e5f6 P \| 9988776655ff PA` |
| `get fwd.whitelist` | `> on 0hop=allow 3 entries \| a1b2c3d4e5f6 \| …` |
| `get fwd.scoped.reserve` | `> 40` |
| `get fwd.scoped.stats` | `> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms` |
| `get flood.max.request` | `> 64` |

In den Listenausgaben steht pro Eintrag nur das **6-Byte-Präfix** des Pubkey; bei `get fwd.block`
bedeutet `P` = *prune*, `A` = *advert*. Lange Listen werden in der Anzeige nach rund 140 Zeichen
abgeschnitten — die Einträge selbst bleiben natürlich aktiv, nur die Ausgabe ist gekürzt.

---

## Warum das Ganze

Zwei Probleme, die im laufenden Betrieb großer Netze auftreten:

**1-Byte-Path-Hashes.** Der Path-Hash ist ein Präfix des Node-Pubkey. Bei einem Byte bleiben nur 256
mögliche Werte — in einem Netz mit mehr als ein paar Dutzend Knoten kollidieren die zwangsläufig. Ein
solcher Hop lässt sich keinem Knoten mehr eindeutig zuordnen, und das Direct-Route-Matching kann
mehrdeutig auflösen. Ergebnis: Relay-Traffic, den niemand mehr zurückverfolgen kann, und Pfade, die
falsch abbiegen.

**Unscoped Floods.** Ein nicht konfigurierter Knoten sendet immer ohne Region-Code — das ist das
Standard- und Legacy-Verhalten und aus Kompatibilitätsgründen so gewollt. In der Praxis stellt dieser
Traffic in gewachsenen Netzen aber die Mehrheit und verdrängt die Zustellung zwischen korrekt
konfigurierten Repeatern aus dem Airtime-Budget.

Die Filter geben dem Betreiber Werkzeuge gegen beides — ohne eine Protokolländerung, die alle
mitmachen müssten.

---

## Stufe 1 — Hash-Size-Filter

Verwirft Pakete, deren Path-Hash nur ein Byte breit ist.

```
set fwd.hashfilter off      # aus (Standard)
set fwd.hashfilter advert   # nur Adverts mit 1-Byte-Hash nicht weiterleiten
set fwd.hashfilter all      # jeglichen 1-Byte-Flood- und Direct-Traffic nicht weiterleiten
```

`advert` ist der sanfte Einstieg: Adverts sind der Traffic, der die Routing-Tabellen im Netz mit
mehrdeutigen Einträgen füllt. `all` erfasst zusätzlich Nutzdaten.

Mit der Wahrscheinlichkeit lässt sich das Ganze dosieren statt hart zu schalten:

```
set fwd.hashfilter.prob 100   # jeder Treffer wird verworfen (Standard)
set fwd.hashfilter.prob 75    # drei von vier Treffern werden verworfen
set fwd.hashfilter.prob 0     # kein Treffer wird verworfen (Filter faktisch wirkungslos)
```

Die Wahrscheinlichkeit wirkt auf **alle** Treffer des eingestellten Modus, also im Modus `advert` auch
auf die Adverts. Ein Wert unter 100 ist sinnvoll, wenn du den Druck auf betroffene Nachbarn erhöhen
willst, ohne sie vollständig abzuklemmen — ihre Pakete kommen dann noch durch, nur unzuverlässiger.

---

## Stufe 2 — Policy-Tabelle pro Pubkey

Eine Liste mit bis zu 16 Knoten und jeweils einer von drei Aktionen.

```
set fwd.block.add <64-stelliger-hex-pubkey> prune    # Pfad-Prune (Standard, wenn nichts angegeben)
set fwd.block.add <64-stelliger-hex-pubkey> advert   # Adverts dieses Knotens nicht weiterleiten
set fwd.block.add <64-stelliger-hex-pubkey> both     # beides
```

**`prune`** verwirft Flood-Kopien, deren Pfad über den genannten Knoten gelaufen ist. Der Test läuft
**vor** der Duplikat-Unterdrückung — eine Kopie desselben Pakets, die über einen anderen Pfad
eintrifft, kann also noch gewinnen. Damit steuert man Pfade, statt Pakete zu vernichten: „nimm nicht
den Weg über X, nimm den anderen". Zuverlässig ist das nur bei Mehrbyte-Hashgrößen, weil der Pfad-Hop
sonst mehrdeutig ist.

**`advert`** unterbindet die Weiterleitung von Adverts, die dieser Knoten selbst ausgesendet hat. Hier
wird der vollständige Pubkey exakt verglichen, das funktioniert daher bei jeder Hashgröße.

Entfernen und prüfen:

```
set fwd.block.del a1b2c3d4      # Präfix genügt, entfernt alle passenden Einträge -> "OK (1 removed)"
set fwd.block.clear             # Tabelle leeren
get fwd.block                   # Inhalt anzeigen
```

Das Hinzufügen eines bereits vorhandenen Pubkey ersetzt dessen Aktion, es entsteht kein Doppeleintrag.

---

## Stufe 3 — Last-Hop-Whitelist

Der schärfste Filter: ein Flood wird nur noch weitergeleitet, wenn sein **unmittelbarer Absender** —
der letzte Hop im Pfad — auf der Erlaubnisliste steht. Alles andere wird verworfen.

```
set fwd.whitelist.add <64-stelliger-hex-pubkey>   # Backbone-Nachbarn eintragen (max. 16)
set fwd.whitelist.del a1b2c3d4                    # Präfix genügt
set fwd.whitelist.clear
get fwd.whitelist
set fwd.whitelist on|off                          # Durchsetzung (Standard: off)
set fwd.whitelist.0hop allow|drop                 # Standard: allow
```

`0hop` betrifft Floods, die noch keinen Hop hinter sich haben, also direkt vom Absender kommen. Da es
hier keinen „letzten Hop" gibt, den man prüfen könnte, braucht es eine eigene Regel. Standard ist
`allow`, damit direkt gehörte Knoten weiterhin durchkommen.

**Damit du dich nicht selbst aussperrst**, sind drei Paketarten grundsätzlich von der Whitelist
ausgenommen und passieren immer: Adverts, `ANON_REQ` und Floods, die an diesen Knoten selbst adressiert
sind. Der Admin-Login über Funk funktioniert also auch bei falsch gesetzter Whitelist noch.

Der Last-Hop wird auf der Hashgröße des Pakets verglichen. Bei einem 1-Byte-Hash heißt das: 256
mögliche Werte, und jeder fremde Knoten mit demselben Präfix passiert die Whitelist ebenfalls.
**Deshalb `fwd.hashfilter all` zuerst einschalten** — dann greift die Whitelist auf Mehrbyte-Ebene und
der Vergleich ist aussagekräftig.

---

## Stufe 4 — Airtime-Reserve für Scoped-Traffic

```
set fwd.scoped.reserve 0     # aus (Standard)
set fwd.scoped.reserve 40    # 40 % des TX-Budgets für Scoped-Traffic reservieren
```

Reserviert den angegebenen Prozentsatz des **TX-Duty-Cycle-Budgets dieses Knotens** für **scoped**
(region-codierten) Flood-Traffic. Nähert sich das Budget dieser Reserve, werden **unscoped** Floods
verworfen; scoped Floods und der gesamte Direct-Traffic laufen unabhängig davon immer durch.

Der Mechanismus ist **lastadaptiv** und braucht keine Schwellwert-Pflege: Der Token-Bucket steht bei
ruhigem Kanal nahe voll, dann passiert unscoped Traffic ganz normal. Erst unter anhaltender Last läuft
der Bucket leer, und die Reserve wird zur harten Grenze — unscoped drosselt zuerst, scoped behält sein
Budget. Kurze Bursts kommen also durch, dauerhaftes Fluten nicht.

Reserviert wird ausschließlich das **Sendebudget dieses Knotens**, nicht der Funkkanal.

Wirkung kontrollieren:

```
get fwd.scoped.stats
> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms
```

- `fwd_scoped` / `fwd_unscoped` — weitergeleitete Floods, nach Scope getrennt
- `drop_unscoped` — von der Reserve verworfene Floods
- `saved_air` — dadurch eingespartes Airtime in Millisekunden

Die Zähler zählen die **Weiterleitungs-Entscheidung**, nicht das bestätigte Senden. Sie liegen im RAM
und **werden bei jedem Neustart auf 0 gesetzt** — das ist Absicht, ein Zähler pro Paket im Flash würde
den nRF52 verschleißen. Für Langzeitstatistik den Wert regelmäßig extern abholen und aufsummieren; ein
Reboot sieht dann einfach wie ein Zähler-Reset aus.

---

## Hop-Limits für geflutete Pakete

```
set flood.max.request 64        # REQUEST      (Standard 64)
set flood.max.anon.request 64   # ANON_REQUEST (Standard 64)
set flood.max.response 64       # RESPONSE     (Standard 64)
```

Begrenzt, wie weit geflutete Pakete dieser Typen laufen dürfen. Der Standard 64 entspricht dem
Maximum und ist damit wirkungslos. Gedacht sind die Limits gegen fehlkonfigurierten
Automatisierungs-Traffic, der das ganze Netz durchquert, obwohl er lokal bleiben sollte.

> ⚠️ **An Backbone-Hochstandorten bei 64 belassen.** Der RF-Admin-Login läuft über
> `ANON_REQUEST`/`RESPONSE`. Wer diese Werte an einem entfernten Knoten zu niedrig setzt, kappt genau
> den Traffic, mit dem er den Knoten wieder erreichen wollte.

(Diese drei Einstellungen stammen aus Mainline-PR #2797 und sind nicht Teil des Forward-Filters im
engeren Sinn, werden aber im selben `/fwd_prefs` gespeichert und über dieselbe CLI bedient.)

---

## Typische Konfigurationen

**„Ich will erst mal nur beobachten."**
Nichts tun. Der Auslieferungszustand filtert nicht. Regelmäßig `get fwd.scoped.stats` abfragen liefert
dir schon ohne aktiven Filter die Aufteilung scoped/unscoped an deinem Standort.

**„Die 1-Byte-Adverts sollen aufhören."**
```
set fwd.hashfilter advert
```
Der übliche erste Schritt. Wirkt nur auf Adverts, Nutzdaten bleiben unangetastet.

**„Ein bestimmter Knoten flutet mich zu."**
```
set fwd.block.add <pubkey> both
get fwd.block
```

**„Der Weg über Knoten X ist schlecht, ich will den anderen Pfad."**
```
set fwd.block.add <pubkey-von-X> prune
```
Kein Datenverlust — Kopien über andere Pfade gewinnen weiterhin.

**„Mein Backbone-Knoten soll nur noch für den Backbone relayen."**
Siehe [Sichere Inbetriebnahme](#sichere-inbetriebnahme) — das ist der Fall, bei dem die Reihenfolge
zählt.

**„Unscoped Traffic frisst mein Airtime."**
```
set fwd.scoped.reserve 40
```
Nach ein paar Stunden `get fwd.scoped.stats` prüfen und den Wert nachziehen. 100 verwirft unscoped
Floods, sobald überhaupt Budgetdruck herrscht.

---

## Sichere Inbetriebnahme

Für die Whitelist (Stufe 3) — der einzige Filter, mit dem man einen Knoten funktional aus dem Netz
nehmen kann. **Bis Schritt 4 greift nichts ein**, die Schritte 1–3 befüllen und prüfen nur und sind
daher gefahrlos an einem laufenden Knoten durchführbar.

1. **Die echten Backbone-Nachbarn ermitteln** — aus aktiver Relay-Adjazenz bzw. TRACE, **nicht** aus
   der Nachbartabelle des Geräts. Die listet Knoten, die du *hörst*, nicht die, die deinen Traffic
   tatsächlich weiterleiten. Das ist der häufigste Fehler an dieser Stelle.

2. **Bei weiterhin ausgeschalteter Whitelist** jeden bestätigten Pubkey eintragen:
   ```
   set fwd.whitelist.add <64hex>
   ```
   Das befüllt nur die Tabelle, an der Weiterleitung ändert sich nichts.

3. **Tabelle prüfen** — `get fwd.whitelist`. Steht dort genau das, was du erwartest?

4. **Erst jetzt scharf schalten**, und in dieser Reihenfolge:
   ```
   set fwd.hashfilter all
   set fwd.whitelist on
   ```
   Der Hashfilter zuerst, damit die Whitelist auf Mehrbyte-Ebene vergleicht statt auf
   kollisionsanfälligem 1-Byte. `0hop allow` (Standard) belassen, bis bestätigt ist, dass der Backbone
   weiterhin über diesen Knoten läuft.

**Rückweg — jederzeit:**
```
set fwd.whitelist off
set fwd.hashfilter off
```

> ⚠️ **Vorsicht bei entfernten Knoten und Masten.** Mit aktiver Whitelist relayt der Knoten für keinen
> nicht gelisteten Last-Hop mehr. Der Admin-*Login* funktioniert weiterhin (Adverts und `ANON_REQ` sind
> grundsätzlich ausgenommen), aber Mehr-Hop-Pfade **durch** diesen Knoten zu dahinterliegenden Knoten
> brechen ab, wenn die Whitelist unvollständig ist. Vor Schritt 4 an einem Standort ohne Backhaul einen
> Rückweg sicherstellen: lokal über USB/BLE erreichbar, oder ein Knoten, über den du noch hinkommst.

---

## Wo die Einstellungen liegen

Alle `fwd.*`- und `flood.max.*`-Einstellungen werden in einer eigenen Datei **`/fwd_prefs`** im
Dateisystem des Knotens gespeichert, in einem selbstbeschreibenden TLV-Format. Die
Mainline-Einstellungsdatei `/com_prefs` bleibt davon unberührt und Byte-für-Byte identisch zur
Mainline.

Das ist keine Kosmetik: Würden die Filter-Felder in `/com_prefs` mitgeschrieben, könnte ein künftiger
Mainline-Merge, der dort ein Feld einfügt, die Positionen verschieben — und dann werden Funk- oder
Regionseinstellungen still gegen Filterwerte verrechnet. Durch die Trennung kann das
konstruktionsbedingt nicht passieren.

Praktische Folgen:

- **Wechsel zwischen Fork und Mainline-Firmware** verliert nur die Filterkonfiguration. Funk-, Regions-
  und Identitätseinstellungen bleiben erhalten.
- **Ältere Firmware liest neuere `/fwd_prefs`** und ignoriert unbekannte Felder. Fehlt ein Feld, gilt
  der Standardwert — also „aus".
- **Beim Update von fwdfilter3 auf fwdfilter4 oder neuer** wird die Filterkonfiguration einmalig auf
  Standardwerte zurückgesetzt (Umstellung von `/com_prefs` auf `/fwd_prefs`). Whitelist- und
  Blacklist-Einträge danach neu setzen. Funk- und Regionseinstellungen bleiben erhalten. Zwischen
  fwdfilter4 und allen neueren Versionen bleibt die Konfiguration erhalten.

---

## Versionsverlauf

| Version | Datum | Was dazukam |
|---|---|---|
| `fwdfilter1` | 2026-06-16 | Stufe 1 (Hash-Size-Filter) + Stufe 2 (Policy-Tabelle) |
| `fwdfilter2` | 2026-06-17 | Stufe 3 (Last-Hop-Whitelist) — **überholt, nicht verwenden** |
| `fwdfilter3` | 2026-06-17 | Fix: Puffer-Überlauf bei `get fwd.whitelist`/`fwd.block` über Funk |
| `fwdfilter4` | 2026-06-20 | Eigene `/fwd_prefs`-Datei · Neuaufsetzen auf Mainline · `flood.max.*` · `get fwd.hashfilter.prob` |
| `fwdfilter5` | 2026-06-21 | Neues Build-Ziel SenseCAP Solar Node P1 (keine Funktionsänderung) |
| `fwdfilter6` | 2026-07-10 | Stufe 4 (`fwd.scoped.reserve`) + `get fwd.scoped.stats` |
| `fwdfilter7` | 2026-07-13 | Fix: Airtime-Schätzung gegen Fehlercodes abgesichert · Version mit führendem `v` |

**Empfehlung: immer die neueste Version.** Alle älteren enthalten mindestens einen der oben
behobenen Fehler.

---

## Grenzen und offene Punkte

- **Je 16 Einträge** in Whitelist und Policy-Tabelle. Das reicht für Backbone-Nachbarschaften, nicht
  für netzweite Listen.
- **Listenausgaben werden gekürzt.** `get fwd.whitelist`/`get fwd.block` zeigen bei voller Tabelle
  nicht alle Einträge an. Die Einträge sind trotzdem aktiv.
- **Statistikzähler sind flüchtig** (siehe Stufe 4) und zählen die Weiterleitungs-Entscheidung, nicht
  das bestätigte Senden.
- **Die Airtime-Reserve wirkt auf Stundenskala.** Der Duty-Cycle-Bucket braucht anhaltende Last, um zu
  leeren. Kurze Bursts unscoped Traffic passieren, auch bei hoher Reserve — so gewollt.
- **Path-Prune ist bei 1-Byte-Hashes unzuverlässig**, weil der Pfad-Hop mehrdeutig ist. Bei
  Mehrbyte-Hashes zuverlässig.

Fehler und Rückmeldungen bitte als
[Issue](https://github.com/ACETyr/MeshCore/issues) melden.
