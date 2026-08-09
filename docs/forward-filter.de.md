# Forward-Filter für Repeater — Handbuch

*🇬🇧 [English version](./forward-filter.md) · 📻 [Flash-Anleitung](./flashing-repeater.de.md)*

Dieses Handbuch beschreibt **alle** Forward-Filter-Funktionen der ACETyr-Repeater-Firmware
(`repeater-v1.17.0.fwdfilterN`) an einer Stelle. Es ersetzt die über die einzelnen Releases verteilten
Beschreibungen — die Release-Notes dokumentieren ab jetzt nur noch, *was sich geändert hat*, dieses
Dokument beschreibt, *was das Gerät kann*.

---

## Das Wichtigste zuerst

**Alle Filter sind ab Werk ausgeschaltet.** Eine frisch geflashte Node verhält sich exakt wie ein
Standard-MeshCore-1.17.0-Repeater. Es passiert nichts, solange du nicht selbst etwas einschaltest.

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
| `set fwd.scoped.reserve` | `0`–`100` | `0` | Prozent der Airtime-Zuteilung (60-s-Fenster) für Scoped-Traffic freihalten |
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
| `get fwd.scoped.stats` | `> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms air=1180/6000ms/60s` |
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
set fwd.scoped.reserve 40    # 40 % der Airtime-Zuteilung für Scoped-Traffic freihalten
```

Hält den angegebenen Prozentsatz der **Sende-Airtime dieses Knotens** für **scoped** (region-codierten)
Flood-Traffic frei. Reicht die verbleibende Zuteilung nicht mehr für ein weiteres unscoped Paket, ohne
die Reserve anzugreifen, wird dieses Paket verworfen; scoped Floods und sämtlicher Direct-Traffic
laufen unabhängig davon immer durch.

**Bezugsgröße ist ein 60-Sekunden-Fenster, nicht das Stundenbudget.** Die Zuteilung pro Fenster ergibt
sich direkt aus dem eingestellten Duty Cycle:

```
Zuteilung pro Fenster = 60 s × Duty Cycle
```

Bei `set dutycycle 10` sind das 6000 ms je Fenster, also rund zwölf Pakete im SF8-Preset. Eine Reserve
von 40 % hält davon 2400 ms frei: unscoped Floods werden verworfen, sobald dieser Knoten im laufenden
Fenster so viel gesendet hat, dass der Rest für ein weiteres unscoped Paket nicht mehr reicht.

> **Warum nicht das Duty-Cycle-Budget selbst?** Weil sich damit nichts messen lässt. Der Token-Bucket
> im Dispatcher läuft über eine ganze Stunde und hat bei legalen 10 % rund 360 000 ms Spielraum. Ein
> Flood-Sturm aus zwanzig Weiterleitungen kostet ~10 000 ms und verschwindet darin spurlos. Auf der
> Bench gemessen: Bucket bei 359 325/360 000 ms (99,8 % voll), während der Knoten ein belebtes Netz
> weitergeleitet hat — bei 0,995 % tatsächlichem Duty Cycle gegen ein 10-%-Limit. Eine Prozentschwelle
> auf einen dauerhaft vollen Bucket kann nur bei 100 % auslösen. Bis einschließlich `fwdfilter7` war
> die Einstellung deshalb faktisch ein Schalter: 0 = aus, 100 = alle unscoped Floods verwerfen, alles
> dazwischen wirkungslos. Das kurze Fenster ist der eigentliche Fix — nicht ein anderer Schwellwert.

Freigehalten wird ausschließlich das **Sendebudget dieses Knotens**, nicht der Funkkanal.

### Gemessenes Verhalten

Zwei Durchläufe mit identischer Last (je 93 Flood-Adverts, `dutycycle 10`, also 6000 ms Zuteilung je
Fenster):

| Reserve | weitergeleitet | verworfen | Airtime-Spitze im Fenster |
|---|---|---|---|
| `0` | 93 | 0 | **7733** / 6000 ms — 129 % |
| `50` | 24 | 69 | 2579 / 6000 ms — 43 % |

Ohne Reserve überzieht diese Last die Fensterzuteilung um 29 %. Mit 50 % Reserve leitet der Knoten die
ersten Pakete weiter und hält die Airtime danach flach bei 2577–2579 ms, über alle sieben Messpunkte
hinweg.

Die Reserve **schaltet also nicht ab, sie regelt sich ein.** Am Haltepunkt standen noch 3423 ms zur
Verfügung, gegen eine Schwelle von 2400 ms Reserve plus rund 500 ms für das nächste Paket — der Knoten
lässt gegen eine 50-%-Reserve rund 57 % der Zuteilung frei, statt an einer festen Grenze umzukippen.
Wie viel tatsächlich durchkommt, hängt damit an der Last und nicht am eingestellten Wert allein.

> **Bei sehr niedrigem Duty Cycle vorsichtig einstellen.** Die Zuteilung schrumpft mit, ein einzelnes
> Paket bleibt aber ~500 ms groß. Bei `set dutycycle 1` stehen nur 600 ms je Fenster zur Verfügung —
> ein einziges Paket füllt die Zuteilung fast vollständig aus, und dann verwirft jede Reserve über 0
> praktisch jeden unscoped Flood, unabhängig von der Last. Rechnerisch korrekt, aber selten gewollt.

Wirkung kontrollieren:

```
get fwd.scoped.stats
> reserve=40% fwd_scoped=812 fwd_unscoped=95 drop_unscoped=1043 saved_air=214500ms air=1180/6000ms/60s
```

- `fwd_scoped` / `fwd_unscoped` — weitergeleitete Floods, nach Scope getrennt
- `drop_unscoped` — von der Reserve verworfene Floods
- `saved_air` — dadurch eingespartes Airtime in Millisekunden
- `air` — im laufenden Fenster verbrauchte Sende-Airtime / Zuteilung / Fensterlänge. Das ist die
  Eingangsgröße des Gates: liegt der erste Wert weit unter dem zweiten, greift die Reserve nicht, und
  zwar unabhängig davon, was eingestellt ist

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
Mainline-Einstellungen liegen davon vollständig getrennt in ihrer eigenen Datei — seit 1.17 ist das
`/prefs.json`, davor war es der Blob `/com_prefs`.

Das ist keine Kosmetik: Würden die Filter-Felder in der Mainline-Datei mitgeschrieben, könnte ein
Mainline-Merge, der dort ein Feld einfügt, die Positionen verschieben — und dann werden Funk- oder
Regionseinstellungen still gegen Filterwerte verrechnet. Durch die Trennung kann das
konstruktionsbedingt nicht passieren. Der Wechsel des Mainline-Formats von `/com_prefs` auf
`/prefs.json` in 1.17 ist genau dieser Fall: `/fwd_prefs` war davon nicht betroffen.

Praktische Folgen:

- **Wechsel zwischen Fork und Mainline-Firmware** verliert nur die Filterkonfiguration. Funk-, Regions-
  und Identitätseinstellungen bleiben erhalten.
- **Ältere Firmware liest neuere `/fwd_prefs`** und ignoriert unbekannte Felder. Fehlt ein Feld, gilt
  der Standardwert — also „aus".
- **Beim Update von fwdfilter3 auf fwdfilter4 oder neuer** wird die Filterkonfiguration einmalig auf
  Standardwerte zurückgesetzt (Umstellung von `/com_prefs` auf `/fwd_prefs`). Whitelist- und
  Blacklist-Einträge danach neu setzen. Funk- und Regionseinstellungen bleiben erhalten. Zwischen
  fwdfilter4 und allen neueren Versionen bleibt die Konfiguration erhalten.
- **Beim Update auf fwdfilter8** wandern die *Mainline*-Einstellungen einmalig von `/com_prefs` nach
  `/prefs.json`. Die Filterkonfiguration ist davon nicht betroffen, ein Rückschritt auf ältere Firmware
  hat aber Folgen — siehe
  [Downgrade nach fwdfilter8](./flashing-repeater.de.md#downgrade-nach-fwdfilter8).

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
| `fwdfilter8` | 2026-08-10 | Basis auf MeshCore 1.17.0 · Fix: Stufe 4 misst über ein 60-s-Fenster statt über den Stundenbucket (1–99 war zuvor wirkungslos) · Fix: Rauschgrund-Schätzer klemmte auf −120 fest · Fix: abgesicherte Airtime-Schätzung brach laufende Sendungen ab |

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
- **Das Airtime-Fenster ist ein fester 60-Sekunden-Wert**, keine Einstellung. Die Länge ist eine
  Abwägung: kurz genug, dass ein Flood-Sturm sichtbar wird, lang genug, dass einzelne Pakete das
  Ergebnis nicht dominieren. Ein gemessenes Optimum ist sie nicht.
- **Das Fenster ist rollend, nicht gleitend.** Ein Burst, der kurz nach einem Fensterwechsel eintrifft,
  bekommt die volle Zuteilung — die Reserve dämpft anhaltende Last, nicht jede einzelne Spitze.
- **Die Lastmessung oben wurde mit Flood-Adverts gefahren.** Bei gemischtem Realverkehr — größere
  Nutzlasten, andere Airtime pro Paket — ist das Verhalten hergeleitet, nicht gemessen.
- **Path-Prune ist bei 1-Byte-Hashes unzuverlässig**, weil der Pfad-Hop mehrdeutig ist. Bei
  Mehrbyte-Hashes zuverlässig.

Fehler und Rückmeldungen bitte als
[Issue](https://github.com/ACETyr/MeshCore/issues) melden.
