# Mehrspieler (2 Spieler, online) – Plan

Wunsch von DevJunior (11.11.2026): „Möglichkeit für Online-Mehrspieler, nur 2 Spieler pro Sitzung“.
Seine Entscheidungen (11.11.2026):

- **Spieler 2 ist ein zweiter bewaffneter Kämpfer** („otro Leon / Luis“), nicht Ashley.
- **Jeder Spieler lässt sein eigenes Spiel laufen** (Lockstep): beide Geräte haben die Disc-Daten, beide
  rechnen jedes Bild selbst, ausgetauscht werden nur die Tasten. Kein Videostrom.
- **Zuerst im selben WLAN** (kein Server, keine Kosten); Internet später.

Kein Fehler des Prototyps: das Original hat keinen Mehrspieler. Alles läuft als Port-Funktion der Mods
(Schalter, Laufzeitcode in `port/whole/runtime/`, Patches ab 0914).

## Was schon da ist (Grundlage)

Der Vergleichslauf von Meilenstein 12 (`whole_r100_check.sh --compare`, `trace_compare.py`) hat gezeigt:
dieselbe Tastenfolge ergibt auf i686 und x86_64 Bild für Bild denselben Spielzustand (6637 gemeinsame Bilder:
Zufall, Spieler, Gegner, Werke, Ereignisse, Positionen, Kamera, Tonspuren, Raum-Flags), wenn

1. die Disc-Lesevorgänge sofort fertig sind (nicht nach echter Zeit),
2. die float-Funktionen der C-Bibliothek auf beiden Seiten gleich rechnen (`libre4_detmath.so`: über ihre
   double-Fassung),
3. die Debug-Anzeige aus ist (ihre Zeitbalken messen echte Zeit),
4. die Bilder fest takten (`RE4_VI_FAST`).

Genau diese vier Bedingungen braucht Lockstep auf zwei Handys.

## Hürden

- **Gleiche Rechnung auf zwei Geräten:** Android-libm von ARM32 und ARM64 können `sinf`, `atan2f` … anders
  runden. Lösung wie `libre4_detmath`: eigene float-Funktionen im Kern, Compiler ohne zusammengezogene
  Multiplikation-Addition (`-ffp-contract=off`). Bis das geprüft ist: beide Geräte mit derselben ABI.
- **Laden nach echter Zeit:** im Mehrspieler warten beide Geräte, bis eine Disc-Lesung auf *beiden* fertig ist,
  und sie gilt auf beiden im selben Bild als fertig (wie Bedingung 1). Kurze Ladepausen sind die Folge.
- **Filme:** die CRI-Uhr misst echte Zeit. Im Mehrspieler werden Filme übersprungen oder auf beiden Geräten bis
  zum selben Bild gezeigt.
- **Kamera je Gerät:** jedes Gerät soll seinem Spieler folgen. Was das Spiel von der Kamera abhängig macht
  (Gegner außerhalb des Bildes angehalten, Sichtprüfungen, Tonpositionen), muss auf beiden Geräten mit
  *derselben* Kamera rechnen; nur das Bild wird pro Gerät anders gezeichnet. Zu prüfen.
- **Menüs:** Koffer, Karte, Händler, Schreibmaschine halten das Spiel an. Öffnet ein Spieler eins, wartet der
  andere (wie Pause), oder Menüs werden im Mehrspieler pro Spieler ohne Pause geführt (später).
- **Spieler 2 im Spiel:** die Gegner kennen nur zwei Ziele, den Spieler (`pPL`, 2927 Stellen in den
  Gegner-Modulen) und den Begleiter (`pSUB`, 622 Stellen: angreifen, packen, Treffer). Eine dritte Figur
  müsste in jedem Gegner-Modul nachgetragen werden. Der gangbare Weg: **Spieler 2 nimmt den Begleiter-Platz**
  ein, als Luis (`cSubLuis`, Modul `pl14`: kämpft schon, die Gegner greifen ihn schon an), gesteuert mit den
  Tasten von Spieler 2 wie Ashleys Pistole Stufe 3 (Laufen, Zielen, Schießen). Offen ist, was in den Abschnitten
  geschieht, in denen die Geschichte Ashley als Begleiterin braucht (Frage an DevJunior).

## Schritte

- **MP0 – Determinismus auf dem PC prüfen (lokal, Disc-Daten):** zwei `re4_host` mit derselben Tastenfolge,
  auch über mehrere Räume, Kämpfe und Ladevorgänge; dazu ein Lauf mit der gebauten Android-Rechnung
  (`-ffp-contract=off`, eigene float-Funktionen). Ziel: `trace_compare` ohne exakte Abweichung. Ohne das kein
  Lockstep.
- **MP1 – Netzkern (Cloud):** Laufzeitcode `runtime/netplay.cpp`: Gerät finden im WLAN (UDP-Rundruf), Verbindung,
  Start nur bei gleicher Disc, gleicher App-Version und gleicher ABI; der Gastgeber schickt den Spielstand und die
  Einstellungen; je Bild die Tasten beider Spieler mit 2–3 Bildern Vorlauf; das Spiel wartet auf fehlende
  Tasten; alle 30 Bilder eine Prüfsumme des Zustands (Teil des Zustands-Traces) zum Erkennen von
  Auseinanderlaufen, mit Bericht. Test in der Cloud: zwei Prozesse über localhost mit dem Kerntest
  (ohne Disc-Daten); lokal: zwei `re4_host` mit Disc-Daten, Spuren verglichen.
- **MP2 – Spieler 2 als Begleiter:** Luis (oder ein Leon-Modell) auf dem Begleiter-Platz, gesteuert von Spieler 2;
  sein Archiv in Räumen laden, in denen das Spiel ihn nicht lädt (Speicherbudget prüfen); Schaden, Tod,
  Wiederbelebung; Munition.
- **MP3 – Kamera und Anzeige je Gerät**, Menüs im Mehrspieler.
- **MP4 – App:** Menü „Multiplayer“ (Spiel eröffnen / beitreten), Texte in 11 Sprachen, Fehlermeldungen.
- **MP5 – Internet** (später): Vermittlung über einen Server oder Portfreigabe.

Jeder Schritt gilt erst als erreicht, wenn sein Test gelaufen ist (MP0, MP2–MP4 nur lokal mit Disc-Daten
und zwei Geräten).
