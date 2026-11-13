# Release-Plan: Kirby Air Riders Recomp bis 100 % stabil

Stand 2026-11-11. Fortschritt grob: **~70 %** (der erweiterte Plan zählt mehr offene Punkte).
Details zu jedem Punkt: `HANDOFF.md`, `PLAN.md`, `KIRBY_RECOMP_PLAN.md`.

Jeder Punkt ist eine von zwei Arten:
- **[OHNE]**: geht ohne Spielstart (Code, Bauen, Analyse, Doku). Das kann ich jederzeit machen, auch wenn du spielst,
  nur das Bauen kostet kurz CPU.
- **[SPIEL]**: braucht einen Spielstart zum Testen oder Messen. Nur, wenn du nicht spielst.

Status: ✅ fertig · 🔧 gebaut, ungetestet · ⬜ offen

## Arbeitsweise (ab 2026-11-11, vom Nutzer verlangt): systematisch, nicht raten

Jeder Fehler wird gleich behandelt:
1. **Messbares Kriterium festlegen**: was genau ist falsch, woran sieht man „richtig“ (Zahl oder Vergleichsbild).
2. **Datenfluss rückwärts verfolgen**: vom falschen Ausgabewert zu seinen Eingaben, Knoten für Knoten. Pro Knoten
   **eine Messung** (Override, Dump, Log), Ergebnis in eine Tabelle (Knoten, gemessen, erwartet, ok?).
3. **Der erste Knoten, dessen Eingaben stimmen, dessen Ausgabe aber nicht, ist die Ursache.** Erst dann wird
   etwas geändert, und zwar genau dort.
4. **Gegenprüfen** mit derselben Messung und dem Vergleichsbild; Ergebnis eintragen.
5. Keine Änderung „auf Verdacht“ ohne Messung, die sie begründet; Diagnose-Schalter bleiben aus.

## Übersicht (Stand 2026-11-11 abends)

| Abschnitt | ✅ fertig | 🔧 gebaut/in Arbeit | ⬜ offen |
|---|---|---|---|
| 1. Bildfehler | 10 | 1 | 1 |
| 2. Fehlende Icons und UI | 3 | 1 | 1 |
| 3. Stabilität | 2 | 0 | 6 |
| 4. Performance | 1 | 1 | 6 |
| 5. Bildqualität und Optionen | 2 | 0 | 1 |
| 6. Release-Vorbereitung | 5 | 0 | 2 |
| 7. Mehrspieler und Spielmodi | 0 | 0 | 6 |
| 8. Eingabe | 2 | 0 | 3 |
| 9. Audio | 2 | 2 | 2 |
| 10. Spielstände und Daten | 3 | 0 | 1 |
| 11. Fenster und System | 0 | 0 | 5 |
| 12. Testautomatik | 2 | 0 | 3 |
| 13. Verteilung | 2 | 0 | 2 |
| 14. Video-Einstellungen im Spiel-Menü | 4 | 3 | 9 |
| 15. Echtes Einstellungsmenü im Spiel | 0 | 0 | 9 |
| 16. Performance-Architektur | 0 | 0 | 8 |
| **Summe** | **38** | **8** | **65** |

Heute erledigt: Höhlen-Leuchten (1.3), dunkles Kirby + flaues Bild (1.10, Array-Fix 512), Validation (1.9),
Replay-Dialog (3.1), Vorschaukarte (2.3), ungültige Shader (P7/Patch 0026), portabler Ordner (10.3), Anleitung (13.1),
Ausgabegerät (9.6), F12-Screenshot (14.16). Jetzt dran: D8 Schattenkarte (1.11). Untersuchungsprotokolle: `RELEASE_PLAN_ARCHIV.md`.

## Jetzt dran (Detailplan, Reihenfolge fest)

Jeder Schritt hat ein Kriterium und eine Messung; kein Schritt ohne Messung davor.

| # | Schritt | Messung (wie) | Kriterium „fertig“ | Status |
|---|---|---|---|---|
| D1 | **Array-Größe festlegen**: wie viele Elemente benutzt das Spiel wirklich? | `KAR_ARR_N=1024` (`arr1024`): Absturz (4122 Deskriptoren > 4096, 18,8 GB Speicher) → neue Messung `KAR_ARR_SCAN=1` (liest die Handle-Puffer über das Array-Ende hinaus, `scan1`): höchstes belegtes Element **416** in c4/c5/c6 (Puffer 32 KB) | N = 512 | ✅ gemessen → Test N=512 |
| D2 | **Startlinie Runde 2 nach der Eishöhle** (Nutzer-Meldung) | Bilder Frame 4800–6800 mit N=512 (`arr512`) | sieht aus wie Runde 1 / Konsole | ✅ Runde 2 und 3 durchgehend richtig (Höhle, Wiesen, Kirby pink) |
| D3 | Standard `KAR_ARR_N` auf das Ergebnis von D1 setzen, Diagnose-Log behalten | `arr512`: 40–48 fps (mit 256: 45–47) | Bild ok, fps ≥ 45 | ✅ Standard 512 (gfx_shader.c arr_n); fps knapp darunter → als Leistungspunkt in 4.x |
| D4 | **Kirby dunkel auf 1 von 8 Bildern** (arrdef, N=256) | Bilder mit N=512 (`arr512`, 11 Bilder Runde 2–3) | Kirby überall wie Konsole | ✅ mit 512 nicht mehr: alle 11 richtig, im Wald etwas schattiger (plausibel) |
| D5 | **1.10 flaues Bild** neu messen (Bild ist seit dem Array-Fix viel heller) | `console_ref/match3.py` Startlinie (`dull4`): Mittel 178–199 (Konsole 188–199, vorher ~130), p5 100–127 (Konsole 122–150, vorher ~45), Sättigung 0,36–0,50 (Konsole 0,34–0,46) | Abweichung < 10 % | ✅ Mittel < 5 %; dunkelste Stellen noch ~15 % dunkler (Rest unter 1.10) |
| D6 | **P7**: 3 Shader mit ungültigem SPIR-V | Ursache: TXQ fragt Komponente 2 (Tiefe/Schichten) einer 2D-Textur ab, Übersetzer extrahierte sie aus einem vec2. Patch `0026-texture-query-size-missing-component.patch` (liefert 1 wie die Hardware); Lauf `p7run` + spirv-val | spirv-val 0 Fehler | ✅ 8589 Shader, 0 ungültig |
| D7 | Menü-Optionen im Spiel prüfen (9.5, 9.6, 14.13, 14.16) | Lauf `d7` | Option wirkt | ✅ 9.6 und 14.16 automatisch bestätigt; 9.5 (Stumm bei Alt-Tab) und 14.13 (Voreinstellung im Menü wählen) brauchen einen Handtest durch den Nutzer |
| D8 | 1.11 Schattenkarte (1024er d16) | im Rennen kaum neu gezeichnet; Ursache im Frame-Aufbau des Spiels | Karte wird im Rennen alle ~32 Frames neu gezeichnet | 🔧 **Zurückgestellt** bis Konsolenvergleich unter einer Überdachung (Konsolenbild Floria zeigt nur den Fahrzeugschatten, den wir auch haben). Verlauf: RELEASE_PLAN_ARCHIV.md. |

## 1. Bildfehler (Blocker für Release)

| # | Punkt | Art | Status |
|---|---|---|---|
| 1.1 | Regenbogen-Läufe: Ursache war ein übersprungenes Ganzzahl-Löschen (0) auf Float-Datentexturen; behoben in `clear_tex`, 8/8 sauber (Fix #37) | SPIEL | ✅ |
| 1.2 | Regenbogen-Nacharbeit: nicht mehr nötig (Ursache war 1.1) | – | ✅ |
| 1.3 | Kirby leuchtet in der Höhle: BEHOBEN (Patch 0025, Übersetzer findet die lokalen Licht-Datensätze; vorher lieferte die globale Lade-Hilfsfunktion immer 0). Siehe Arbeitsplan R11 | OHNE + SPIEL | ✅ |
| 1.4 | Direct-Shader (41): echte Texturen wieder standardmäßig an (`KAR_POOL_ARR=0` schaltet aus); mit Fix #37 4/4 sauber | SPIEL | ✅ |
| 1.5 | 82 Compute-Shader mit Handles außerhalb ihrer Tabelle (bekommen nichts). Hauptfall: Handle-Wort bei c0+0xE0. Versuch `KAR_C0_TEX=1` im Test: die Wörter sind zur Laufzeit 0, die Slots also wirklich leer; Hypothese widerlegt, bleibt aus. Abgeschlossen: das Spiel bindet dort nichts, auf der Konsole läsen sie dieselben leeren Slots | OHNE + SPIEL | ✅ |
| 1.6 | 3D-Rauschtextur als 2D-Array: im Validation-Lauf (1.9) nicht gemeldet | SPIEL | ✅ |
| 1.7 | Separate Sampler: kein echter Fehler. Alle 16.182 Fälle sind kombiniert, die Laufzeit setzt Textur und Sampler richtig zusammen; die Dump-Ausgabe war irreführend und ist korrigiert | OHNE | ✅ |
| 1.8 | Belichtung/Bloom gegen Konsolen-Screenshots prüfen | SPIEL | ✅ gemessen (2026-11-12, `console_ref/match3.py`, Startlinie): helle Stellen p95 232–254 vs. Konsole 232–249 → Belichtung/Spitzlichter im Rahmen; Bloom sichtbar (Boost-Tore leuchten); dunkle Stellen p5 ~20 % zu dunkel → Rest unter 1.10. Der nicht gebaute 6,3-MB-Compute-Shader ist kein Bloom (vermutlich der neuronale Hochskalierer, ersetzt durch unser Hochskalieren). Szenen-Korrelation nur 0,13–0,48: für eine genaue Aussage bräuchte es Konsolenbilder derselben Stelle |
| 1.10 | Bild wirkt flau/zu dunkel: mit dem Array-Fix (KAR_ARR_N=512) Startlinie Mittel 178–199 wie Konsole 188–199 (vorher 130), Sättigung passt; Rest: dunkelste Stellen ~15 % dunkler (`dull4`) | OHNE | ✅ (Rest beobachten) |
| 1.9 | Validation-Lauf (Rennen, nach Patch 0025, `E:\kirby\val2`): keine Fehler, nur 2 Warnungen: Descriptor-Pool ohne SAMPLER-Typ (behoben, vk_exec_draw.c Pool-Größen) und Fragment schreibt Location 0 ohne Farbziel (harmlos, Leistungshinweis). Menü/City Trial noch nicht | SPIEL | ✅ Rennen |
| 1.11 | Sonnen-Schattenkarte (1024er, fs Slot 2) wird nur im Intro gezeichnet, im Rennen nie; die Matrix folgt aber dem Spieler, Schatten stimmen also nicht (Szene eher zu dunkel). Gezeigt mit `KAR_ALT_SHADOW=3600:c:1024` | SPIEL | 🔧 Weiter nicht gezeichnet (letzter Schreibzugriff Frame 2682, Intro). Die Karte sagt im Rennen praktisch „überall Licht“ (Gegentest shw4) → es fehlen Schatten, das Bild wird dadurch nicht dunkler. Im Rennen gibt es keinen Pass auf 0x5d0a00000 (Dump f3300); die 512er d16-Pässe sind Figuren-Vorschauen, keine Schatten **Nachprüfung 2026-11-13 (shd1, `KAR_DIAG_RT1024=2`, mit 16.7):** unverändert – Schattenkarte 0x5d0a00000 einmal beim Rennstart (Bild 5803), danach nie; die neue Rückschreibung ändert die Entscheidung des Spiels nicht. Weiter nur über die Entscheidungsstelle im Spielcode (Aufrufkette aus KAR_DIAG_RT1024). |
| 1.10 | 59 Tessellation-Stufen ohne Übersetzung: nicht sichtbar, kein einziges Draw in Menü/Rennen ausgelassen | OHNE | ✅ |

## 2. Fehlende Icons und UI

| # | Punkt | Art | Status |
|---|---|---|---|
| 2.1 | Tastensymbole (Ersatzschrift), Original-Schrift aus eigenem Dump möglich | — | ✅ |
| 2.2 | Graues/blaues Quadrat oben links hinter der Minikarte: gibt es auch auf der Konsole (nintendo.com air_ride-Screenshot) → kein Fehler. Seit Patch 0025 fehlt es bei uns in hud1/r11fix → prüfen, ob es nur zeitweise erscheint oder ob 0025 es kaputt macht | SPIEL | 🔧 |
| 2.3 | Menü-Vorschaukarte zeigt nur Platzhalter (Video/Render-to-Texture). Nach Patch 0025 (`E:\kirby\menu5`, f00900): Modus-Karte „Rennen“ zeigt ein echtes Streckenbild, Streckenauswahl mit Vorschaubildern. Vermutlich behoben, Nutzer bestätigt noch | SPIEL (Dump) | ✅? |
| 2.6 | **Leere Sprechblase** über dem gewählten Symbol der Hauptmenü-Leiste (auch Titel-Hinweis) | SPIEL | ⬜ 🔧 Ursache: direkte Pool-Arrays mit Hash `(id ^ id>>5) & 31` – Schriftatlas 58702 und Textur 52558 teilen Element 4. Fix gebaut: `pool_bindless_patch` (gfx_shader.c) liest solche Arrays über den globalen Bindless-Satz, **Schalter `KAR_POOL_BL=1` (aus, im Spiel ungeprüft)**. Test: menu_mail2.txt (Blase bei 2300), Titel, Rennen vergleichen. Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 2.4 | Unbekannte Symbol-Codes ins Font-Skript eintragen (sobald sie auftauchen): wartet auf Fund, übersprungen | OHNE | ⬜ |
| 2.5 | Sprache im Port-Menü einstellbar: gibt es schon („Game language“, Automatisch = Deutsch bei Kirby, Neustart nötig) | — | ✅ |

## 3. Stabilität

| # | Punkt | Art | Status |
|---|---|---|---|
| 3.1 | „About Replays“-Hinweis nach dem Rennen durchklicken | SPIEL | ⬜ |
| 3.2 | Alle Modi durchspielen: City Trial, Top Ride, Road Trip, Optionen, Speichern/Laden | SPIEL | ⬜ **Lauf 2026-11-13 (c32/c32b, Standard mit 16.x, Limit 60):** City Trial, Top Ride, Road Trip je 2× **ohne Absturz** (City ~52–59 fps, Top ~48–59). Erster City-Lauf nach den Shader-Änderungen sah wie ein Hänger aus (Einzelbilder bis 18 s): Compute-Shader wurden neu übersetzt (~1 s je Shader), mit warmem Cache normal (cityp). Prüfpunkte: City-Trial-Bilder bei 4200 sehr dunkel (Lava-Gegend / fast schwarz mit HUD) – Vergleich nötig; Top-Ride-Kartenmüll (1/10) weiter offen. |
| 3.3 | Fehlende Systemdienste (FATAL „service not implemented“) je nach Fund ergänzen | OHNE (nach Fund) | ⬜ **Lauf 2026-11-13 (c32b, alle Modi):** keine „service not implemented“-Meldungen. Dafür „Ampere lowering failed“: opcode 0x34e (LEPC) ~60×/Lauf, immer an +0 mit `LEPC R0` = Anfang eines Geometrie-Programms, das außerhalb des Geometrie-Pfads übersetzt wird (Eglantine 78743f3 lehnt LEPC dort ebenso ab); 0xf60 (TEX mit Handle im Uniform-Register, Sonderform) 6×/Lauf, 0x89a 1×/Lauf. In den Bildern kein fehlender Effekt erkennbar → beobachten. |
| 3.4 | Langzeittest 60 Minuten: Abstürze, Speicherwachstum, Ruckler | SPIEL | ⬜ **✅ Lauf 2026-11-13 (soak1, soak60.txt, Standard mit 16.x):** >60 min ohne Absturz, 7 Bilder ok, Arbeitsspeicher (Working Set) pendelt 8,9–9,9 GB ohne stetigen Anstieg, fps je Minute meist ~60 (Skript), ein Ladefenster 24,5. |
| 3.5 | Echter Controller: Belegung, Vibration, Pause, Hot-Plug | SPIEL (du) | ⬜ |
| 3.6 | Neues Testskript für den frischen deutschen Spielstand | OHNE + SPIEL | ⬜ **✅ 2026-11-13:** `fresh_race.txt` (Repo kirby/scripts): leeres Profil (nur account.uid, z. B. build/profile-kar-fresh), A 400/550/700/850 überspringt Vorspann/Titel, A 1000 schließt den Hinweis „Besuche die Fahrschule!“ (Cursor steht danach auf Fahrschule), UP 1150 → Air Ride, dann A alle 200 Bilder → Streckenwahl (1800), Figur/Maschine, Rennen ab ~3600 (fresh5, Limit 60). Vor jedem Lauf save/, save-backup/, cache/ im Profil löschen. |
| 3.7 | Absturzbericht: steht schon im Log (`logs\kar.log`, letzte 5 Läufe), Datei heißt jetzt `kar.log` statt `dkb.log` | OHNE | ✅ |
| 3.8 | Code-Review der heutigen Änderungen (`pool_patch`, `pool_fill`, `shadow_dummy`): 1 Fehler behoben | OHNE | ✅ |

## 4. Performance

| # | Punkt | Art | Status |
|---|---|---|---|
| 4.1 | Speicher: Boot-Vorbau aus (29 → 24 GB privat, 12 GB RAM) | — | ✅ |
| 4.2 | Ruckler im ersten Rennen (1%-Low 7 fps): Pipelines beim ersten Start/Install vorbauen | OHNE + SPIEL | ✅ über 16.1a: Compute-Prefetch + Nutzungslisten; Rennen 1% low 42–51 fps (ab Sitzung 2) |
| 4.3 | Gezielter Vorbau nur der Rennen-Pipelines statt aller (RAM sparen) | OHNE + SPIEL | ✅ `pipeline-used.bin`/`cs-used.bin` (nur in den letzten 8 Sitzungen Benutztes): 186 statt 3373 Grafik-Pipelines |
| 4.4 | CPU-Profil eines warmen Rennens (`TOTK_PROFILE`), Top-Funktionen optimieren | SPIEL, dann OHNE | ⬜ |
| 4.5 | Pro Draw: Descriptor-Updates/Textur-Lookups (Pool-Arrays schreiben 64 Descriptoren) | OHNE + SPIEL | ⬜ |
| 4.6 | GPU: Barrieren um die Hochskalierung reduzieren | OHNE + SPIEL | ⬜ |
| 4.7 | Speicher-Zensus pro Subsystem im Log (gab es, jetzt mit Prozess-Gesamtwert), dann größte Posten senken | OHNE + SPIEL | 🔧 |
| 4.8 | Pipeline-Cache-Datei (~1 GB) wird beim Start komplett gelesen | OHNE + SPIEL | ✅ eigener Pipeline-Cache für kar* aus (Treiber-Cache genügt), −1,1 GiB, kein 20-s-Speichern mehr |

## 5. Bildqualität und Optionen

| # | Punkt | Art | Status |
|---|---|---|---|
| 5.1 | FSR1-Hochskalierung statt DLSS | — | ✅ |
| 5.2 | Schärfen (RCAS) für den DLSS-Ersatz: läuft (im Test geprüft), standardmäßig aus, an nur mit selbst gesetztem Schärfe-Regler | OHNE + SPIEL | ✅ |
| 5.3 | Einstellungen im Port-Menü: Auflösung, Vollbild, V-Sync, FPS-Limit gibt es schon (DKB-Menü); bei Kirby noch prüfen | SPIEL | ⬜ |

## 6. Release-Vorbereitung (alles OHNE)

| # | Punkt | Status |
|---|---|---|
| 6.1 | Diagnose-Ausgaben hinter Schalter (`KAR_DIAG`); Draw-Statistik schreibt nur noch bei ausgelassenen Draws | ✅ |
| 6.2 | Fix-Protokoll und `docs/kirby-running.md` aktuell halten (neue Schalter): Stand 2026-11-09 eingetragen | ✅ |
| 6.3 | Kirby-spezifischen Code sauber von DKB trennen (`kar*`-Abfragen bündeln) | ✅ **Umgesetzt 2026-11-12** (Bau ok; 13 Stellen → `rt_game_is_kar()`, nur die zwei versionsgenauen `kar458752`-Abfragen in nvn_api_dlss.c bleiben). **Plan:** was: die 15 verstreuten `strncmp(rt_game_target, "kar"…)` durch eine Funktion `rt_game_is_kar()` (host.h, main.c) ersetzen. warum: eine Stelle statt 15 Kopien, später leicht in eigenes Repo/eigene Datei zu trennen. wie: rein mechanisch, Verhalten unverändert; Prüfung = Bau ohne Fehler. |
| 6.4 | README für das Kirby-Ziel: `docs/kirby-running.md` aktualisiert (Was geht, bekannte Lücken, Log-Pfad) | ✅ |
| 6.5 | `tools/check_content.py`: sauber, neue Datendateien enthalten nur Hashes/Patch-Tabelle wie bei DKB | ✅ |
| 6.6 | Lizenzen: FSR1-Eintrag deckt jetzt auch `kar_upscale.comp` ab | ✅ |
| 6.7 | Umzug in dein eigenes Repo (du committest) | ⬜ |

## 7. Mehrspieler und Spielmodi

| # | Punkt | Art | Status |
|---|---|---|---|
| 7.1 | Lokaler Splitscreen (2–4 Spieler): mehrere Ansichten, mehrere Controller | SPIEL | ⬜ |
| 7.2 | Online/LAN abschalten bzw. sauber als „nicht verfügbar“ melden (kein Absturz beim Öffnen) | OHNE + SPIEL | ⬜ |
| 7.3 | City Trial: Stadt mit vielen Objekten (Performance, fehlende Effekte) | SPIEL | ⬜ |
| 7.4 | Top Ride (Draufsicht-Modus) und Road Trip (Story) einmal komplett | SPIEL | ⬜ |
| 7.5 | Zwischensequenzen und Videos (Film-Decoder `ov_movie2.c`) in allen Modi | SPIEL | ⬜ |
| 7.6 | Replays ansehen und speichern | SPIEL | ⬜ |

## 8. Eingabe

| # | Punkt | Art | Status |
|---|---|---|---|
| 8.1 | Tastatur-Belegung dokumentiert (`docs/kirby-running.md`) | OHNE | ✅ |
| 8.2 | Tasten umbelegen im Port-Menü: gibt es schon (Steuerungs-Tab) | — | ✅ |
| 8.3 | Xbox-, PlayStation- und Switch-Pro-Controller (Tastensymbole passend zum Pad?) | SPIEL (du) | ⬜ |
| 8.4 | Mehrere Controller gleichzeitig für Splitscreen | SPIEL (du) | ⬜ |
| 8.5 | Gyro/Bewegungssteuerung, falls das Spiel sie nutzt | SPIEL (du) | ⬜ |

## 9. Audio

| # | Punkt | Art | Status |
|---|---|---|---|
| 9.1 | Ton funktioniert (AudioOut → SDL3, 6 Kanäle) | — | ✅ |
| 9.2 | Knackser/Aussetzer bei Rucklern (Puffer 250 ms) messen | SPIEL | ⬜ |
| 9.3 | Lautstärke-Regler im Port-Menü wirkt jetzt auch bei Kirby (AudioOut) | OHNE + SPIEL | 🔧 |
| 9.4 | Ton läuft bei Alt-Tab/minimiert weiter oder pausiert sauber | SPIEL | ⬜ |
| 9.5 | Ton-Tab: „Im Hintergrund stumm“ (`TOTK_MUTE_UNFOCUSED`, sofort): AudioOut- und Renderer-Pegel × 0, solange das Fenster keinen Fokus hat. Warum: heute läuft der Ton bei Alt-Tab voll weiter (9.4). Hilft: übliche PC-Option, auch für 9.4 | OHNE + SPIEL | 🔧 gebaut, im Menü sichtbar (menutest4); Tastendruck/Gerätewechsel noch von Hand testen |
| 9.6 | Ton-Tab: „Ausgabegerät“ (`TOTK_AUDIO_DEVICE`, Gerätename, Standard = Windows-Standard, nach Neustart): Liste aus SDL3 `SDL_GetAudioPlaybackDevices`, beim Öffnen gesuchtes Gerät statt Default. Warum: Headset/Lautsprecher wählen ohne Windows umzustellen | OHNE + SPIEL | ✅ getestet (`d7`): unbekannter Name → Log „not found: system default“, Wiedergabe auf dem Standardgerät |

## 10. Spielstände und Daten

| # | Punkt | Art | Status |
|---|---|---|---|
| 10.1 | Speicherort, Journal-Speichern und 3 Backups in `docs/kirby-running.md` dokumentiert | OHNE | ✅ |
| 10.2 | Absturz während des Speicherns: Spielstand bleibt heil (Journal-Prinzip testen) | SPIEL | ⬜ |
| 10.3 | Profil-Ordner portabel (neben der Exe) für Release: vorhanden, Paket-Skript für kar.exe angepasst (Plan P4) | OHNE | ✅ |
| 10.4 | Falsche Spielversion: Hash-Prüfung gab es, Meldung nennt jetzt die passende Version | OHNE | ✅ |

## 11. Fenster und System

| # | Punkt | Art | Status |
|---|---|---|---|
| 11.1 | Vollbild/Fenster wechseln (Alt+Enter), Größe ändern, mehrere Monitore | SPIEL | ⬜ |
| 11.2 | Alt-Tab, Minimieren, Energiesparmodus/Standby | SPIEL | ⬜ |
| 11.3 | Andere GPUs: AMD und Intel (bisher nur NVIDIA getestet) | SPIEL (andere PCs) | ⬜ |
| 11.4 | Mindestanforderungen festlegen (RAM, VRAM, GPU) nach Messungen | OHNE nach SPIEL | ⬜ |
| 11.5 | Windows 10 prüfen | SPIEL (anderer PC) | ⬜ |

## 12. Testautomatik (damit nichts wieder kaputtgeht)

| # | Punkt | Art | Status |
|---|---|---|---|
| 12.1 | Ein Skript für „N Rennen, Screenshots, Log auswerten“: `tools/kar_race_test.ps1` (startet nicht, solange Wardogs läuft) | OHNE | ✅ |
| 12.2 | Automatischer Regenbogen-/Schwarzbild-Check: `tools/kar_shot_check.py`, Median-Helligkeit pro Lauf, an 10 bekannten Läufen geeicht (10/10 richtig) | OHNE | ✅ |
| 12.3 | Validation-Layer-Lauf als fester Test vor jedem Release | OHNE + SPIEL | ⬜ **Lauf 2026-11-13 (val1, Rennen mit 16.x-Standard):** nur VUID-…-08600 (Satz-Kompatibilität bei Bindless/INDEPENDENT_SETS, bekannte Layer-Eigenheit) und …-08114 („VkDescriptorSet 0x0“ = Push-Satz, bekannte Layer-Eigenheit, siehe vk_exec_draw.c), je 10× gekappt; **1× VUID-vkDestroyImage-01000 beim Start** (Bild in einer noch aufgezeichneten Befehlsliste zerstört, direkt nach dem ersten Pool-Import) → **behoben (val2/val3):** das Bild des Ladebildschirms wurde freigegeben, während sein letzter Present-Blit (eigene Zäune in gfx_present.c, nicht im Zeitstrahl der Zerstörungsliste) noch lief; jetzt `vk_present_wait_idle()` davor, Validierung ohne 01000. Keine neuen Fehler durch Bindless/dyn. UBO/Abbild-Rückschreibung. |
| 12.4 | Testskripte für jeden Modus (Menüpfade für frischen und alten Spielstand) | OHNE + SPIEL | ⬜ |
| 12.5 | Langzeit-Soak über Nacht mit Speicher- und fps-Protokoll | SPIEL | ⬜ |

## 13. Verteilung

| # | Punkt | Art | Status |
|---|---|---|---|
| 13.1 | Ablauf für Nutzer: eigener Dump → Extraktion → Build → Start: `docs/kirby-running.md` „From your own dump to a running game“ (Plan P5) | OHNE | ✅ |
| 13.2 | Installer/Paket ohne Spieldaten (DKB hat `tools/package_build.py`) | OHNE | ⬜ **Befund 2026-11-12:** `tools/package_build.py` packt kar.exe bereits (portabler Ordner, enthält aber ExeFS = Spielcode → nur privat). Paket *ohne* Spieldaten = Installer, der beim Nutzer aus seinem Dump hebt und baut: `installer/` kennt nur Bananza (bananza.py 842 Zeilen: Prüfen → Daten → Übersetzer → Lift → Bau). Für Kirby nötig: Spielerkennung (Titel 040077201BE50000, Update 458752), `extract_nup`, gen.py mit `--patches recomp/patches-kar458752.txt` + `--nro effectscript=…EffectScript.nro`, gepacktes RomFS (kar458752-packed). Aufwand ~1 Tag, rein OHNE; laut Reihenfolge erst bei Release (6.x/13.x). |
| 13.3 | Shader- und Pipeline-Cache beim ersten Start mit Fortschrittsanzeige vorbauen | OHNE + SPIEL | ⬜ |
| 13.3a | **Ladebildschirm „Pipelines laden“ beim Start** wie Hookshot/NX2navi (Nutzer-Foto 2026-11-12: Logo, Version, Balken „Loading pipelines – Loading the shader cache“). Heute läuft unser Vorbau unsichtbar (Grafik-Vorbau aus `pipeline-used.bin` ~0,1–3 s, Compute-Prefetch im Hintergrund aus `cs-used.bin`). Wie (Nutzer: **kein ImGui, alles nativ**): eigener Startbildschirm direkt im Vulkan-Präsentationspfad (gfx_present.c): Hintergrund/Logo als beim Build erzeugtes Bild, Balken und Text über eigene Rechtecke/Glyphen-Atlas (eigener kleiner Shader), mit Fortschritt aus vk_pipeline.c prewarm und gfx_shader.c Prefetch; beim ersten Start (leerer Treiber-Cache) wichtig, da dann Minuten **Umgesetzt (ungetestet, 2026-11-12):** gfx_present.c Abschnitt „boot screen“: Thread zeichnet auf der CPU (Verlauf, Titel, Statuszeile, Balken, Prozent; Text nativ über GDI `plat_text_draw` in platform/win32.c), Upload über die Upload-Batches, Anzeige mit `vk_present_image`; Fortschritt aus `gfx_shader_prewarm_progress` (Shader) und `vk_pipeline_prewarm_progress` (Pipelines); Start/Stopp in boot.c um den Vorbau; `TOTK_BOOT_SCREEN=0` aus | sichtbarer Fortschritt, Spiel startet danach ohne Ruckler | OHNE + SPIEL | 🔧 |
| 13.4 | Changelog: `docs/kirby-changelog.md` | OHNE | ✅ |

## 14. Video-Einstellungen im Spiel-Menü (Wunsch)

Ziel: ein Tab „Video“ im Einstellungsmenü (Esc/F1), in dem alles Bildrelevante an einer Stelle steht, jede Option
mit deutscher Beschreibung und dem Hinweis, ob sie sofort wirkt oder einen Neustart braucht. Ein Teil existiert schon
im DKB-Tab „Bildschirm“, wirkt aber bei Kirby nicht überall oder ist nicht geprüft.

| # | Option | Heute | Art | Status |
|---|---|---|---|---|
| 14.1 | Tab „Video“ anlegen, Kirby-relevante Optionen dorthin, Rest unter „Erweitert“ | Tab heißt auf Deutsch „Video“; Umsortieren offen | OHNE + SPIEL | 🔧 |
| 14.2 | Ausgabe-Auflösung (Liste der Monitor-Auflösungen + eigene) | `OUTPUT_RES`, Neustart | OHNE + SPIEL | ⬜ |
| 14.3 | Fenstermodus: Fenster / randloses Vollbild / exklusives Vollbild | `FULLSCREEN` (randlos), F11 | OHNE + SPIEL | ⬜ |
| 14.4 | V-Sync an/aus/adaptiv (Present-Modus fifo/mailbox/immediate verständlich benannt) | `VK_PRESENT` | OHNE + SPIEL | ⬜ |
| 14.5 | FPS-Limit 30/60/120/144/Monitor (Spiel läuft intern mit 60: höhere Werte prüfen) | `FPS_LIMIT` | SPIEL | ⬜ |
| 14.6 | Interne Auflösung (Spiel rendert 1536×864 für DLSS): Stufen 67 %/80 %/100 % | fehlt | OHNE + SPIEL | ⬜ |
| 14.7 | Upscaler: FSR 1 / bilinear, Schärfe-Regler | `UPSCALER`, `SHARPNESS` (wirkt jetzt auch bei Kirby, 🔧) | SPIEL | 🔧 |
| 14.8 | Anzeige-Auflösung, die das Spiel sieht (heute fest 2560×1440, `KAR_DISPLAY_RES`) | nur Umgebungsvariable | OHNE + SPIEL | ⬜ |
| 14.9 | Helligkeit/Kontrast/Sättigung/Gamma | `COLOR_*` (DKB), bei Kirby prüfen | SPIEL | ⬜ |
| 14.10 | Seitenverhältnis/Ultrawide, HUD-Position | `ASPECT`, `ULTRAWIDE`, `HUD_POSITION` (DKB), bei Kirby prüfen | SPIEL | ⬜ |
| 14.11 | Leistungsanzeige (fps, Frametime) | `PERF_OVERLAY`, F10 | SPIEL | ⬜ |
| 14.12 | GPU-Auswahl bei mehreren Grafikkarten | `GPU_INDEX` | — | ✅ |
| 14.13 | Voreinstellungen „Leistung / Ausgewogen / Qualität / Benutzerdefiniert“ (`TOTK_VIDEO_PRESET`): setzt interne Auflösung, Upscaler und Schärfe auf einmal; Ändern einer der drei Zeilen macht „Benutzerdefiniert“ (wie das Farbprofil). Warum: ein Klick statt drei Zeilen für Spieler ohne Grafikwissen | `menu.cpp tab_screen` | OHNE + SPIEL | 🔧 gebaut, im Menü sichtbar (menutest4); Tastendruck/Gerätewechsel noch von Hand testen |
| 14.16 | Bildschirmfoto per F12 (`TOTK_SCREENSHOT_KEY`, An/Aus im Video-Tab): Swapchain-Bild als PNG nach `screenshots\kar-<Datum>.png` neben der exe (Capture-Puffer aus gfx_present.c existiert schon für den Menütest). Warum: Bilder ohne Overlay/Snipping-Tool, auch für Fehlerberichte | `gfx_present.c`, `menu_win.cpp` | OHNE + SPIEL | ✅ getestet (`d7`, Test-Schalter `KAR_SCREENSHOT_AT=<frame>`): `screenshots\kar-20261111-220206.png` 3440×1440 geschrieben |
| 14.14 | Menütexte auf Deutsch | `ui_text.c text_de`, Menüsprache Deutsch (auto bei deutschem Windows), im Spiel geprüft | OHNE | ✅ |
| 14.15 | Port-Menü im Kirby-Farbstil (Übergang, bis 15 fertig ist) | `menu.cpp style_init`, nur kar* | OHNE | ✅ |

## 15. Echtes Einstellungsmenü im Spiel (Wunsch, Ziel)

**Vorgabe Nutzer (2026-11-12): kein ImGui, alles nativ – einzige Ausnahme die Leistungsanzeige (F10).** Betrifft das Port-Menü (F1, heute `ui/menu.cpp` auf ImGui), den Ladebildschirm (13.3a) und jede neue Oberfläche. **Zweite Vorgabe: Design 1:1 wie Kirby** (Schrift, Farben, Formen, Animationen aus dem Spiel; eigene Bildschirme wie der Ladebildschirm übernehmen den Stil des Spiels, der heutige rosa Ladebildschirm ist ein Platzhalter). Ziel bleibt das echte Spielmenü (15.1–15.9); wo das nicht reicht, eigene native Darstellung im Vulkan-Präsentationspfad (eigener Shader, Schrift-Atlas aus dem Build), kein ImGui.

Ziel: Die Port-Einstellungen (Auflösung, Fenstermodus, V-Sync, FPS-Limit, interne Auflösung, Upscaler, Schärfe,
Farbe, Lautstärke, Steuerung) erscheinen **im eigenen Optionen-Menü von Kirby Air Riders**, mit der Schrift, dem Layout,
den Animationen und der Auswahl-Logik des Spiels, nicht als Overlay darüber. Das ImGui-Overlay (14) bleibt nur als
Notfall-/Entwicklermenü (Taste) erhalten.

Aufwand: Wochen, nicht Tage. Risiko: ein Spiel-Update kann Hooks und Layouts brechen (wir unterstützen aber nur 1.3.3).
Keine Spieldateien im Repo: Layouts/Texte werden zur Laufzeit gepatcht oder aus dem Dump des Nutzers erzeugt.

| # | Schritt | Art | Status |
|---|---|---|---|
| 15.1 | Optionen-Bildschirm des Spiels finden (Szene/Layout, zuständige Klassen) | OHNE (romfs/Code lesen) + SPIEL (Log) | 🔧 Spieldaten in HAL-Archiven (assets.cat Hash-Katalog, Engine „phx“); Anzeige-Seite = Prefab `Mnu_Opt_Disp_OptMode_Cmn`. Nächster Schritt: Zeilentext sichtbar ersetzen; Einbau als abgewandeltes Prefab über die RomFS-Schicht (Schaltfläche „PC-Grafik“ als Kopie von „Anzeigeeinstellungen“) + Code-Haken für ihre Aktion. Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 15.2 | Laufzeit-Hook auf das Aufbauen der Optionsliste (übersetzte Funktion in `generated/` über `recomp`-Patch oder Hook-Tabelle): eigene Einträge anhängen | OHNE + SPIEL | ⬜ |
| 15.3 | Eintragstypen des Spiels nutzen: Umschalter (An/Aus), Auswahl (◀ Wert ▶), Schieberegler; Texte über die Textdatenbank des Spiels (eigene Text-IDs einspielen, de/en) | OHNE + SPIEL | ⬜ |
| 15.4 | Werte an die Port-Einstellungen binden (`cfg_set`/`cfg_value`), sofort wirkend wo möglich, sonst Hinweis „nach Neustart“ im Stil des Spiels | OHNE | ⬜ |
| 15.5 | Neue Unterseite „Video“ (falls die Liste zu lang wird): eigener Menüpunkt im Optionen-Menü, der eine weitere Liste des Spiels öffnet | OHNE + SPIEL | ⬜ |
| 15.6 | Steuerung/Tastenbelegung: zuerst nur ein Eintrag „Tastenbelegung…“, der das Port-Menü öffnet; später eigene Seite | OHNE + SPIEL | ⬜ |
| 15.7 | Controller-, Maus- und Tastatur-Navigation genau wie im Spiel; Spiel speichert die eigenen Optionen weiter, unsere gehen in die Port-Konfig | SPIEL | ⬜ |
| 15.8 | Absicherung: wenn Hook oder Layout nicht passt (andere Version), Einträge weglassen und das Overlay (14) anbieten, nie abstürzen | OHNE | ⬜ |
| 15.9 | Test über alle Menüwege: Hauptmenü → Optionen, Pause im Rennen → Optionen, City Trial | SPIEL | ⬜ |

Zwischenschritt, falls 15 länger dauert: das Overlay wie das Spielmenü gestalten (schräge Banner, Kartenlayout,
Spielschrift aus dem Dump wie beim Tastensymbol-Font) und aus dem Pause-Menü des Spiels heraus öffnen.

## 16. Performance-Architektur (Ziel: sehr performant, auch auf schwacher Hardware wie Ryzen 7 4750G)

Prinzip: die Arbeit pro Draw auf der CPU fast auf null bringen, indem Vulkan so arbeitet wie die Switch-GPU (Texturen
über Handles in einer Tabelle, Puffer über rohe Adressen). Beseitigt zugleich die Fehlerklasse „zu kleine Arrays /
geratene Puffergrenzen“. Jeder Schritt wird vorher und nachher gemessen (Profil Render-/Spiel-Thread, fps,
Replay-CPU-Zeit pro Frame).

| # | Schritt | Kriterium | Art | Status |
|---|---|---|---|---|
| 16.0 | **Referenz SwitchRECOMP** (`refs/switch2recomp`) | GPU-LLE-Ansatz (NVN-Treiber läuft mit), für uns nicht übertragbar | OHNE (Bewertung) | ✅ bewertet. Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.1 | Baseline messen: Profil (`TOTK_PROFILE_RENDER`), fps, Replay-CPU-ms pro Frame im Rennen; später auch auf dem 4750G | Zahlen in dieser Tabelle | SPIEL | 🔶 |
| 16.1a | **Replay-Spitzen (300–650 ms)** gemessen mit `KAR_REPLAY_PROF=1` | Spitzen-Frames: disk-Zeit ≪ 1 ms je Shader; 1% low steigt | OHNE + SPIEL | 🔶 Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.2 | **Bindless-Texturen** + Replay-Beschleunigung | Replay kürzer, Bild unverändert | OHNE + SPIEL | ✅ Stand: Bindless (`KAR_BINDLESS=0` aus) und Abbild-Rückschreibung (`KAR_WB_SHADOW=0` aus) **Standard an** → Rennen ~105 fps (vorher 65). Bild = Replay ~7,6 ms seriell (5 Job-Threads) + Sync-Warten 1,7 ms; GPU ~3,1 ms. Paralleles Replay: Wochen, hohes Risiko → zurückgestellt. Werkzeuge: `KAR_REPLAY_PROF`, `KAR_SYNC_PROF`, `KAR_DRAW_TS`, `TOTK_BIND_STATS`, `KAR_WB_DIFF`, `TOTK_FT_PERIOD`. Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.3 | **Buffer Device Address** für alle globalen Speicherzugriffe (Plan P3a–P3f) | keine Kandidaten-Hilfsfunktionen mehr (stubscan), Bild unverändert | OHNE + SPIEL | ⬜ |
| 16.4a | **Messung vor dem Umbau (2026-11-12, Rennen ~66–70 fps):** kein Thread ausgelastet | fps Rennen ↑, Bilder sauber | SPIEL | 🔶 Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.4 | **Replay parallel**: Befehlspuffer des Spiels auf mehreren Threads übersetzen (Secondary Command Buffers) | Replay-CPU-Zeit pro Frame sinkt mit Kernzahl | OHNE + SPIEL | ⬜ |
| 16.5 | **Speicher direkt teilen** (Import-Modus) statt spiegeln, sobald 16.2/16.3 stehen | kein Spiegel-Abgleich im Profil; gleiche Bilder | SPIEL | ⬜ |
| 16.6 | Feinarbeit nach Profil (Shader-Vorbau 13.3, heiße Spiel-Funktionen) | 60 fps stabil auf 5950X + RTX 4090 | SPIEL | ⬜ |
| 16.7 | **Frame Generation** (AMD FSR 3, Open Source) als Menüoption; Spiellogik bleibt beim echten Takt | 120 fps angezeigt, Eingabe ok | OHNE + SPIEL | 🔧 FidelityFX SDK 1.1.4 gebaut (`tools/build_ffx.py`, Clang-Anpassungen inkl. 16-Byte-Ausrichtung). DLSS-Pfad des Spiels in 1.3.3 unerreichbar (Fabrik baut die DLSS-Klasse nie) → keine Spiel-Bewegungsvektoren. B1 `KAR_FG=1` (runtime/vk_fg.cpp: Optical Flow + Interpolation, eigenes Pacing) läuft, echte fps unverändert, aber Zwischenbild ≈ Überblendung; Optical Flow bei der Bewegungsunschärfe nur Rauschen (Debug `KAR_FG_DEBUG=1`, Dump `KAR_FG_DUMP=<n>`). **Nächster Schritt B2:** Kamera-Bewegungsvektoren aus Tiefe + aktueller/vorheriger Kamera-Matrix (Kamera im Code finden), danach B3 HUD-loses Bild. **Plan B2:** was: Kamera-Bewegungsvektoren; warum: Zwischenbild sonst nur Überblendung; wie: f_7005384740 schreibt je Kamera einen Konstantenblock (x1, 0x210 Byte: sechs 4×4-Matrizen ab +0x60, Rendergröße +0x1e0, Near/Far +0x1e8); Hook vor dem `ret` (Set `karfg`, `@call:3c0`) merkt sich den Block; erst Diagnose `KAR_FG_CAMLOG=<Bild>` (welche Kamera = Szene, welche Matrix = View/Proj/Vorbild), dann Compute-Pass Tiefe → Weltpunkt → Vorbild-Bildpunkt = Bewegungsvektor in vk_fg.cpp. **B2 Stand (gebaut, mv1):** Hook `karfg` (kar_fg_cam.c), Szenen-Kamera-Matrizen m2=VP, m3=inv VP (m0 Projektion umgekehrte Tiefe, m4/m5 = m2/m3, kein Vorbild im Block); vk_fg_mv.c + shaders/kar_fg_mv.comp rechnen im Replay nach dem Upscale Bewegungsvektoren aus der Szenentiefe (d32s8, vk_exec_depth_newest). Offen: Kamerawahl trifft teils eine zweite 1536×864-Kamera (FOV 0,34) → Auswahl je Bild verbessern; Zwischenbild noch mit Doppelkonturen; y-Konvention prüfen. **Update:** Kamerawahl = breitester FOV > 0,5 je Bild, neueste + vorige Kamera; Tiefenprüfung nach NX2navi `dlaa_mv.comp` (MIT): mitgeführter Fahrer → Vektor 0; y-Konvention „NDC +1 oben“ (wie NX2navi). **Ergebnis mvF:** Zwischenbild so scharf wie echte Bilder (Schärfe 1,00–1,08 statt 0,79), Kirby ohne Doppelkontur. Neu: `fg: shown … fps (real …)` alle 5 s. **Messung Limit 144 (fg144c/d, Haiku-Agent, Skript `fresh_race144.txt` = fresh_race ×2,4, Fahrschul-Fahrt):** mit FG angezeigt **220–227 fps** bei echten 111–113 (1 % low 82–89); ohne FG echte 94–101. → 144 Hz werden mit FG voll bedient, echte fps nicht schlechter. **Plan B3:** was: HUD-loses Bild an die Interpolation; warum: HUD wird sonst mit verschoben/verschmiert; wie: die DLSS-Ausgabe (Szene ohne HUD, linear, vor dem Composite bc440c9e = sqrt(scene·ui.a + ui)) im Replay direkt nach dem Upscale in den Ring-Platz kopieren (vk_fg_mv.c); zur Present-Zeit Compute-Shader kar_fg_hudless.comp: Abtasten auf Ausgabegröße + sqrt → rgba8 (= Kodierung des fertigen Bilds ohne HUD) → `currentBackBuffer_HUDLess` und Optical-Flow-Eingabe. **B3 umgesetzt (hud1, mvlog):** HUD-loses Bild trifft das echte Bild außerhalb des HUD auf 1,2/255 (Wurzel stimmt; `KAR_FG_HUDMODE` 0/1/2, -1 aus); im Rennen 94–97 % der Bilder mit Kamera-Bewegungsvektoren + HUD-losem Bild (`KAR_FG_MVLOG=1`), Menüs ohne (keine Szenen-Kamera). Limit 60: angezeigt 101–114 bei echten 51–57. **Nutzertest (2026-11-13, Limit 72):** Bild gut, aber Ton stotterte (auch im Menü, ohne FG sauber). Ursache: Present-Thread blieb während Interpolation + Zwischenbild-Blit/Flip „busy“ → Spiel wartete bei jedem Submit (vk_present_drain) mehrere ms. **Fix:** Spiel freigeben, sobald die Kopie des Spielbilds abgegeben ist; Zwischen- und echtes Bild aus FG-eigenen Bildern (`vk_fg_current`). Nutzer: Ton sauber. `KAR_FG_PACE` 0/1/2 (Diagnose). Offen: Bewegung bewegter Objekte (pro Objekt), Pacing bei FIFO/VRR prüfen, Menüoption. Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.9 | **Zielsystem (Nutzer, 2026-11-12): Ryzen 7 4750G mit 32 GB RAM oder weniger** (iGPU teilt den RAM). Budget: Prozess privat ≤ 8 GiB, Arbeitsspeicher ≤ 6 GiB, kein Feature darf RAM gegen Glätte tauschen ohne Deckel (z. B. Compute-Prefetch 16.1a: SPIR-V nach Pipeline-Bau freigeben). Heute: 15,3 GiB privat / 9,8 GiB Arbeitsspeicher (rprof14) → größte Posten per Speicher-Zensus (4.7) finden und senken Beobachtung Soak-Anfang (soak1, mehrere Rennen, Frame 9000–12600): privat 17,4–18,5 GiB bei Arbeitsspeicher 7,6–7,9 GiB → privat wächst über Rennen (Commit für VRAM/Pipelines), prüfen | Budget eingehalten im Rennen | OHNE + SPIEL | ⬜ |
| 16.10 | **Realistische Ziele + Hebel (Messung rmem2, 2026-11-12)** | Werte je Hebel gemessen | OHNE + SPIEL | 🔶 Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.11 | **Hohe Bildraten ohne Frame Generation** (Weg für 144 fps, Vorbild DKB `docs/dkb-frame-rate.md`) | Rennen bei 144 Cap: Spielzeit/Echtzeit 1,00 ✅, 144 fps erreicht ⬜ | OHNE + SPIEL | 🔶 Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 16.12 | **Hinweise aus Fritchii `NX2_NOTES.md`** (Eden-Fork für Switch-2-Titel, gelesen 2026-11-12): (a) #111/#127 das Spiel liest den Systemtakt (CNTPCT) beim Warten auf die GPU millionenfach/s; auf AMD ohne CPUID 0x15 ging jeder Lesezugriff über QueryPerformanceCounter (~10–23 % des Hauptkerns) → unser `rt_ticks()` messen (wichtig für 4750G); (b) #97 Detailstufen über Task-/Mesh-Programme, abgewiesen = Pop-in/Flackern → prüfen, ob Kirby solche Programme hat; (c) #58 bedingtes Rendern über zwei Abfragezähler bestätigt (bei uns über NVN-API umgesetzt) (a) Stand: `rt_ticks()` = QueryPerformanceCounter je Zugriff (main.c), Zugriffszahl im Spiel noch nicht gemessen | je Punkt gemessen | OHNE + SPIEL | ⬜ |
| 16.8 | Linux (RADV) + AMD-APU-Test (Ryzen 7 4750G), siehe 11.3 | läuft; Zielwerte nach Messung | SPIEL (anderer PC) | ⬜ |

## 17. Feature-Ziele (Liste vom Nutzer, 2026-11-12)

Abgleich mit dem Plan: „teilweise in“ = verwandter Punkt existiert, deckt aber nicht alles ab. „Code da?“ = Stichwortsuche im Runtime-Code (DKB-Erbe), Wirkung bei Kirby noch nicht geprüft.

| # | Feature | Bezug im Plan | Code da? | Art | Status |
|---|---|---|---|---|---|
| 17.1 | Nativer Code statt Emulation | Grundlage des Projekts | ja | – | ✅ |
| 17.2 | 60 fps Standard; Limit 30–240 fps, Monitor-Bildrate oder unbegrenzt | teilweise in 14.5 | `FPS_LIMIT` | OHNE + SPIEL | ⬜ |
| 17.3 | Breitbild und Ultrawide (21:9, 32:9), HUD, Menüs und Filme bleiben 16:9 | teilweise in 14.10 | `ULTRAWIDE` | OHNE + SPIEL | 🔧 Ausgabe 3440×1440, Kameras breit, UI-Ebene 2576×1080 mit mittigen 16:9-Rechtecken → HUD unverzerrt. **Fix 2026-11-13 (Nutzer: Rennen wirkt gestreckt):** die Renn-Projektion (f_70053839b0 → Kamera+0x150) nimmt das Seitenverhältnis von Kamera+0x208, nicht von der +0x318-Kopie → Szene war 16:9 projiziert und auf 21:9 gezogen (34 % zu breit). Neuer Hook `persp_hook` (karwide, `@call:3d0` an 0x5383bc0/0x5383c80) setzt nur das geladene s10 auf das Zielverhältnis (Speicher bleibt 16:9); wide53: Projektion 2,39, Kirby rund, mehr Sicht seitlich. **Offen:** Figuren-/Maschinenwahl kaputt (3D-Figuren in den Rahmen abgeschnitten/verschoben, Nutzerbild 2026-11-13); Banner „LOS GEHT'S!“ endet an der 16:9-Grenze; reine UI-Seiten mit schwarzen Seiten; 3D-Szene intern 16:9 (Schärfe zurückgestellt, Plan M/N gescheitert). Verlauf: RELEASE_PLAN_ARCHIV.md. |
| 17.4 | Auflösungen 1080p bis über 1440p: 1800p, 4K, 5K oder Fenstergröße; scharf bei High-DPI | teilweise in 14.2, 14.6, 14.8 | teilweise | OHNE + SPIEL | ⬜ |
| 17.5 | Hochskalierung/AA: DLSS (DLAA, Quality, Balanced, Performance, Ultra Performance; RTX), FSR 3.1 (Native AA + Modi; jede GPU), FSR 4 (RX 6000/7000/9000); 3D-Szene in Modus-Auflösung, HUD scharf; alternativ FSR 1 oder bilinear mit Schärfe; schwarze Balken oder strecken | teilweise in 5.1, 14.7 (FSR 1, bilinear, Schärfe) | DLSS-Erwähnungen, FSR 3/4 nein | OHNE + SPIEL | ⬜ |
| 17.6 | Frame Generation: DLSS FG (RTX 40/50, Reflex) 2x, auf RTX 50 3x/4x; FSR 3.1 FG (jede GPU); HUD ausgenommen; bei anderer Fensterform nur im Bild (Balken sauber); Limit zählt Spiel-Frames; Limit an RTSS übergeben | teilweise in 16.7 (nur FSR 3) | nein | OHNE + SPIEL | ⬜ |
| 17.7 | HDR, zwei Arten: Szene auf Spitzenhelligkeit tonemappen (Mittelgrau/Ton des Spiels, echtes Schwarz) oder natives HDR des Spiels; Spitze, Spiel- und HUD-Helligkeit im Menü | neu | HDR-Code vorhanden | OHNE + SPIEL | ⬜ |
| 17.8 | Bekannte Shader beim Build vorbereitet, im Spiel gefundene Pipelines bei jedem Start im Hintergrund neu gebaut; nie auf dem Zeichen-Thread kompilieren (Worker) | teilweise in 4.2, 13.3, 16.1a (Compute-Prefetch) | Grafik ja, Compute nein | OHNE + SPIEL | 🔶 |
| 17.9 | Einstellungsmenü im Spiel (F1 oder Minus+Plus am Pad), Leistungsanzeige (F10) | teilweise in 14.11, 15 | Port-Menü ja | SPIEL | ⬜ |
| 17.10 | Tastatur, Maus, Gamepads mit Umbelegung; Nintendo-/Xbox-/PlayStation-Layout; Vibration, Gyro; Spiel folgt dem zuletzt gedrückten Pad; DualShock 3 direkt (abschaltbar) | teilweise in 8.2–8.5 | Rumble, Gyro, DS3 | SPIEL (du) | ⬜ |
| 17.11 | Tastensymbole Xbox, PlayStation, Steam Deck, Tastatur/Maus im Spieltext, beim Build gezeichnet; Tastatur-Symbole folgen der Belegung | teilweise in 2.1, 8.3 | Prompt-Code vorhanden | OHNE + SPIEL | ⬜ |
| 17.12 | Stereo oder 5.1 auf wählbarem Ausgabegerät; Standard-Mikrofon als Mikrofon-Eingang des Spiels | teilweise in 9.1, 9.6 (Mikrofon neu) | Ausgabe ja | OHNE + SPIEL | ⬜ |
| 17.13 | Wahl des Monitors; direkt im Vollbild starten, wenn Vollbild an | teilweise in 11.1, 14.3 | Monitor-Code vorhanden | SPIEL | ⬜ |
| 17.14 | Discord-Status mit Originaltitel, Original-Icon und Spielzeit (Standard an, abschaltbar im Spiel-Tab und im Launcher) | neu | nein | OHNE + SPIEL | ⬜ |
| 17.15 | Konsolenmodus TV oder Handheld wählbar | neu | Docked/Handheld-Code vorhanden | SPIEL | ⬜ |
| 17.16 | Texturen bleiben komprimiert (BC3, halber VRAM, gleiche Geschwindigkeit), außer bei 20 GiB VRAM und 48 GiB RAM (Windows rechnet VRAM aufs Commit-Limit) | neu, betrifft 4.1, 4.7 | BC3-Code vorhanden | OHNE + SPIEL | ⬜ |

## 18. Anregungen aus Longshot (MagicBeanDev, Schwesterprojekt; Doku gelesen 2026-11-13)

Longshot (git.magicbean.dev/MagicBeanDev/Longshot, docs/) ist ein PC-Port derselben Bauart (statische Recompilation, Vulkan, Spiegel/Abbild, Pipeline-Cache). Übernommen werden nur Ideen, kein Code (Lizenz GPL-3.0, eigene Prüfung nötig).

| Nr | Aufgabe | Herkunft | Test | Status |
|---|---|---|---|---|
| 18.1 | **Flush nicht-kohärenter Upload-Speicher**: CPU-geschriebene Staging-/Scratch-Bereiche (Textur-Staging, UBO-/Vertex-Scratch) vor der GPU-Nutzung flushen, Slice-Starts an `nonCoherentAtomSize` ausrichten. Auf NVIDIA meist kohärent (daher bei uns unsichtbar), auf AMD/Intel sonst veraltete Texturen/Konstanten | docs/texture-staging-publication.md, draw-stream-visibility.md | OHNE + 11.3 | ⬜ (nur 5 Flush-Stellen im Code; prüfen, welche Speichertypen VMA wählt) |
| 18.2 | **Texel-Puffer (Buffer-Texturen) nachbilden**: bei uns noch Dummy-View (vk_exec_draw.c „buffer textures are not replayed yet“). Longshot: Views je (VkBuffer, Format, Offset, Bereich) gecacht, mit dem Import-Puffer freigegeben; Speicher-Schreibzugriffe melden `gpu_wrote` | docs/texel-buffer-ownership.md | SPIEL | ⬜ (erst zählen, ob Kirby sie nutzt) |
| 18.3 | **Vom GPU gezeichnete Daten, die nie im Spielspeicher ankamen** (bei Longshot: Terrain-Kacheln → verwaschene Bodentexturen). Kandidat für unseren seltenen Fahrzeugkarten-Müll: Render-Ziele, die das Spiel später als Textur aus seinem Speicher liest | worklist #2545 | SPIEL | ⬜ |
| 18.4 | Rückschreib-Beobachtung: bei uns mit `KAR_WB_DIFF` + Abbild-Rückschreibung (16.7) bereits weiter umgesetzt | docs/writeback-observations.md | – | ✅ (bei uns gelöst) |
| 18.5 | **Speicherwachstum über lange Sitzungen** ist dort der Hauptfehler (RAM/VRAM bis OOM); Pipeline-Cache hält eine volle Heap-Kopie. Bei uns 60-min-Test stabil (3.4); 12.5 Übernacht-Soak und Pipeline-Cache-Speichern ohne Vollkopie prüfen | worklist, docs/pipeline-cache-storage.md | OHNE | ⬜ |
| 18.6 | Frame-Logik > 60 fps (bei Longshot: zählt Bilder, Ereignisse kaputt); bei Kirby prüfen, ob Zähler/Timer an Bilder gebunden sind (Spiel ist selbst getaktet, 16.x) | worklist #1612/#1742 | SPIEL | ⬜ |
| 18.7 | Robustheit: Swapchain-Neuaufbau nach Fehler, Geräteverlust beim Present melden, Retry für fehlgeschlagene Image-Views, Sampler-Fehler vor Deskriptor-Veröffentlichung abfangen | docs/swapchain-recovery.md, presentation-device-loss.md, image-view-retry.md, sampler-admission.md | OHNE | ⬜ |
| 18.8 | Intel: Abstürze beim Pipeline-Bau → Cache-Speichern vom Bauen trennen; AMD: VK_EXT_device_fault (haben wir) | worklist #2456/#2117 | 11.3 | ⬜ |

## Reihenfolge

1. **Jetzt dran** (Tabelle oben): D8 Schattenkarte (1.11) zurückgestellt (Konsolenbild nötig), 1.8 Bloom ✅ gemessen.
2. (Nutzer 2026-11-12: Absturzprüfungen ans Ende, siehe 7.) Performance zuerst.
3. Performance-Architektur 16.1–16.6 (größter Leistungshebel, beseitigt auch die Fehlerklasse „zu kleine Arrays/Puffer“).
4. Optionen/Menü 5.3, 14.x, dann Modi 7.x, Audio 9.x, Fenster 11.x.
5. Feature-Ziele 17.x (zuerst die mit vorhandenem Code: 17.2, 17.3, 17.7, 17.15, 17.16 prüfen).
6. Release 6.x, 13.x; Wunsch 15.x (echtes Spiel-Menü) und 16.8 (Linux) zuletzt. **16.7 Frame Generation vorgezogen (Nutzer 2026-11-13)**, nach den Absturz-/Langzeittests, vor 15.x.
7. Ganz am Ende (Nutzer, 2026-11-12): Absturzprüfungen 3.2 (alle Modi; Skripte vorbereitet: `E:/kirby/tools/mode_top.txt`, `mode_city.txt`, `mode_road.txt` = Hauptmenü → Modus → A alle 150 Bilder, Aufruf `kar_race_test.ps1 -Script … -Shots 1200-6000:600 -ShotCount 9 -Env 'TOTK_FPS_LIMIT=60'`), 3.3, 3.5, 3.6, 12.x, dann 15.x Rest (Stand (2) in 15.1: Lebenszeit der Anzeige-Seite erkennen, Kurz-String-Fix testen, Werte 15.3/15.4), dann Langzeittests 3.4 (60 min) und 12.5 (Nacht-Soak). Vorbereitet: `tools/soak60.txt`, `kar_race_test.ps1 -MaxMinutes`.
