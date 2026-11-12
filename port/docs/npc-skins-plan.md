# Skins für Nebenfiguren (Ashley, Luis …) – Plan

Wunsch von DevJunior (11.11.2026): „die NPCs ändern können (auch Luis und Ashley)“, dazu Ada Wong, HUNK, Krauser,
Wesker und die Figuren der Söldner („damit man sie in der Kampagne benutzen und sehen kann“); seine Wahl: Skins (Modelle),
wie bei Leon (`claude/cubeevil4-skins.md` im Projekt, `port/tools/uhd_skin_convert.py`, Patch 0901). Kein Fehler
des Prototyps: eine Port-Funktion der Mods.

## Was schon da ist

- **Mod-Ordner:** `<data>/mods/<pack>/` ersetzt *jede* Datei der Disc gleichen Pfads, auch in anderer Größe
  (`runtime/dvd.cpp` applyMods, die Größe geht in die FST); Pakete schaltet die App einzeln.
- **Umwandler:** `uhd_skin_convert.py` wandelt ein Spielerarchiv der PC-Fassung (`em/plXX.udas`) in das Format der
  Disc; er ist nicht auf Leon festgelegt (Rahmen und Bewegungen aus der Datei der Disc, das Skelett muss gleich
  sein). Die Werkzeuge danach (`skin_slim.py`, `skin_graft.py`, `skin_face.py`, `skin_shapes.py`) kennen bisher nur
  Leons Einträge (Kopf 2, Gesicht 4, Haar 5, Hände 14..21).
- **Ashley als Spielerin** (ihr Kapitel, `pl_type` 1) liest ihr Archiv wie Leon über `ReadPlayerData`: der
  Ausweichplatz für große Spielerarchive (`re4_player_data_place`, 6 MB ab 0x82600000) gilt schon für sie.

## Hürden

1. **Speicher der Begleiter und Gegner-Archive.** Ashley als Begleiterin (`cSubAshley`, Modul-ID 3, Datei
   `em/pl11` bzw. `pl15` im zweiten Kostüm), Luis (`pl14`) und jede andere Figur kommen über `EmReadSearch` →
   `readEmData`: ohne festen Platz ein Block aus dem Spiel-Heap in der Größe der Datei (mindestens der Größe, die
   der Raum angibt, z. B. 0x120000 in r11c). Ein umgewandelter PC-Skin ist mehrere MB groß; der Heap des Raums hat
   so viel nicht frei → „no room“ oder ein anderes Objekt bekommt keinen Platz.
   Vorschlag: eine Mod-Datei eines Figurenarchivs, die größer ist als die der Disc, liest `readEmData` in einen
   eigenen Platz (Patch, wie 0901 für den Spieler), der nicht aus dem Heap kommt und beim Raumwechsel frei wird.
2. **Wo dieser Platz liegt.** Die Entwicklerhälfte von MEM1 (0x81800000..0x83000000) ist fast voll (Kern, Optionen,
   Krausers Waffen, Nahkampf, Waffe auf dem Rücken, Spielerarchiv 6 MB, Primitiv-Überlauf 4 MB). Vorschlag: MEM1 des
   Ports auf 64 MB vergrößern (0x83000000..0x84000000 als Platz für Mod-Archive, z. B. zwei Figuren à 8 MB) – das
   Spiel selbst kennt nur 24 MB. Zu ändern bzw. zu prüfen: `VALID_PTR` (`include/main_mem.h`, bis 0x82FFFFFF; als
   Ersatz-Header in `port/whole/include/`), 14 Stellen des Ports mit `RE4_MEM1_SIZE` (u. a. GX-FIFO, Modul-Lader,
   Schnellspeichern: jeder Zustand wird 16 MB größer), die feste Abbildung auf 32-bit-Android und der
   Speicherbedarf der App (+16 MB).
3. **Gewichte:** große Skins brauchen `cWeightExt` und den gesperrten Cache des Ports (Patch 0901) – zu prüfen, ob das
   nur für das Spielerarchiv eingeschaltet ist.
4. **Zwischensequenzen:** Leon trägt dort Teile des Ereignisses (Kopf `ev0001` mit den Gesichtsformen); Patches
   0906/0907 zeigen den Kopf des Skins. Für Ashley und Luis ist zu prüfen (mit Disc-Daten), welche Teile ihre
   Ereignisse setzen und ob dieselbe Lösung greift.
5. **Werkzeuge:** Einträge je Figur (Kopf, Gesicht, Haar, Hände) in `skin_slim.py`/`skin_shapes.py` usw.; das
   Haar von Ashley hat eigene Physik (`pl_cloth.cpp`: Haar, Rock, Pullover).

- **Ada, HUNK, Krauser, Wesker als Spielfigur der Kampagne** gibt es schon (Cheat-Menü „Spielfigur“, Patch 0195):
  ihr Archiv kommt als Spielerarchiv über `ReadPlayerData` und nutzt damit den Ausweichplatz von 6 MB wie Leon; ihre
  Skins brauchen nur den Umwandler (N2). Als Nebenfigur (Ada und Krauser in den Ereignissen, Luis, Ashley, Gegner)
  kommen sie über `readEmData`: Platz aus N1.

## Schritte

- **N1 – Platz für Mod-Figurenarchive (Cloud): ✅ 11.11.2026, Patch 0914** (`runtime/mod_place.cpp`). Statt MEM1 auf
  64 MB zu vergrößern (auf 64-bit-Android liegt MEM1 im Bereich der großen Java-Objekte, dessen Ende bei kleinem
  `dalvik.vm.heapsize` unter 0x84000000 liegt: der Kern wäre dort gar nicht mehr gestartet) bekommen die Mods einen
  eigenen Platz von 16 MB oberhalb von MEM1, nur wenn ein Mod-Archiv ihn braucht; die Prüfungen des Spiels gehen bis
  `re4_game_top` (Umschreibung in `whole_prepare.py`), GX und ARAM sehen ihn bei 0x04000000. Laufzeittest `modplace`;
  mit Disc-Daten und auf dem Gerät nicht geprüft.
- **N2 – Umwandeln (Cloud, mit den Dateien von DevJunior):** ein PC-Skin für Ashley (`pl11.udas`, und `pl01` für ihr
  Kapitel) und einer für Luis (`pl14.udas`); Vergleich mit `re4_format_compare` wie bei Leon.
- **N3 – Gerät:** Ashley als Begleiterin, in ihrem Kapitel, Luis in der Hütte; Speicher, Bildrate, Zwischensequenzen.
- **N4 – Zwischensequenzen:** Patch 0915 (12.11.2026): die Kopien der Figurenmodelle in den evd-Dateien werden an
  ihren Bytes erkannt und aus dem Archiv des Skins gezeichnet; Gesichter mit Ereignis-Formen (wie 0906/0907 bei Leon)
  folgen, falls die Prüfung es zeigt.

Jeder Schritt gilt erst als erreicht, wenn sein Test gelaufen ist (N3/N4 nur auf dem Gerät).
