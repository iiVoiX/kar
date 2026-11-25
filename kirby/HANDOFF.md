# Handoff: Kirby Air Riders Recomp (read this first in a new session)

Kept short on purpose (token use). Full history: `HANDOFF_ARCHIV.md`; plan and status per item: `RELEASE_PLAN.md`
(row history in `RELEASE_PLAN_ARCHIV.md`). Read those only for a specific item.

## Repo and commits
- Code: `E:\kirby\dkb-pc-recomp`, branch `kirby`, pushed as `git push kar kirby:main` (github iiVoiX/kar). Never assets.
- Commit as `git -c user.name=iiVoiX -c user.email=iiVoiX@users.noreply.github.com commit`, message ends with the
  Co-Authored-By line. Before: copy RELEASE_PLAN.md + HANDOFF.md into `dkb-pc-recomp/kirby/`, run
  `.venv/Scripts/python tools/check_content.py --staged`.
- After each step: `tools/kar_status_page.py` (run from E:\kirby) → republish artifact
  https://claude.ai/artifact/1bNfNYTDBCw5dnjp5tXvsn

## Build
`.venv/Scripts/python tools/build_game.py --version kar458752 --expected tools/data/expected-kar458752.json --romfs build/extracted/kar458752-packed/romfs`
(from dkb-pc-recomp; ~1 min for runtime changes). FidelityFX SDK: `tools/build_ffx.py` (only when its patches change;
afterwards touch runtime/vk_fg.cpp so kar.exe relinks).

## Testing (test runs need no OK, standing permission 2026-11-14; never while WardogsClient runs)
- Never while `WardogsClient*` runs (`tasklist | grep -i wardogs`); the test script refuses then.
- Race: profile `build/profile-kar-fresh` (delete its save/, save-backup/, cache/ first), script
  `kirby/scripts/fresh_race.txt`, `TOTK_FPS_LIMIT=60`, race from frame ~3600:
  `powershell -NoProfile -ExecutionPolicy Bypass -File tools/kar_race_test.ps1 -Runs 1 -Script kirby/scripts/fresh_race.txt -Profile build/profile-kar-fresh -Out E:/kirby/<dir> -Shots '4400-5200:400' -ShotCount 3 -Env 'TOTK_FPS_LIMIT=60;...'`
- `toRace.txt` (old save) no longer reaches a race. `RAINBOW` im Shot-Check = Median-Helligkeit > 0,62, schlägt bei hellen Strecken fälschlich an. Shots are 480x270 unless `TOTK_SHOTS_FULL=1` (ultrawide looks
  squeezed in small shots).
- Log lines: `TOTK_FT_PERIOD=5` (fps per 5 s), `TOTK_TRACE=present`, `KAR_PRESENT_PROF=1`.

## State (2026-11-13)
- Race ~105 fps at 3440x1440 (bindless + shadow write-back on by default). Limit 60 shows ~20 frames > 25 ms per 5 s
  with and without frame generation (own open finding).
- Frame generation (16.7): `KAR_FG=1` runs (runtime/vk_fg.cpp, FFX optical flow + interpolation, own pacing), but the
  interpolated picture is ~a blend: the game renders no motion vectors (its DLSS class is never built in 1.3.3) and
  optical flow is noise under the game's motion blur. Tools: `KAR_FG_DUMP=<n>` (+`KAR_FG_DUMP_DIR`), `KAR_FG_DEBUG=1`.
- Open bugs: Fahrerwahl im Breitbild 2026-11-18 wie 16:9 (ein Bildpaar); Maschinenwahl nicht verglichen; Streifen oben links im Menü offen; empty speech bubble 2.6 (`KAR_POOL_BL=1` 2026-11-18: kein Unterschied); Top Ride card garbage 1/10; City Trial: schwarzer Kirby in Lava-Höhlen (3.2).

## Next
- 16.7 B2: camera motion vectors from depth + current/previous camera matrices (find the camera in game code; the
  projection hook is kar_wide.c `project_hook`, f_7005384740, aspect at +0x318, fov +0x31c), fed to
  `ffxFrameInterpolationPrepare` in vk_fg.cpp; then B3 HUD-less picture (DLSS stand-in output before the composite).
- Test `KAR_POOL_BL=1` (menu_mail2.txt, bubble at 2300).

## Stand 2026-11-17 (16.3 P3a ✅, P3b Runtime ✅)
- **P3a verifiziert:** Census Frame 1800 zeigt 14 BDA-Einträge bei 121 Import-Pools.
- **⚠ P3b verliert in einem Rennlauf das Gerät (~12 s, siehe Plan 16.3) → hinter `KAR_BDA=1`, Standard aus.**
- **P3b Runtime gebaut:** GPU-Puffer (VMA CPU_TO_GPU) mit uint32-Count-Prefix + sortierte Tabelle,
  Deskriptor-Set 3 (eine SSBO-Bindung), Pipeline-Layouts auf 4 Sets erweitert, Bindung pro CB analog Bindless.
  Testlauf der anderen Session (bis Frame 2400, Menü): kein Crash; im Rennlauf jedoch Geräteverlust (siehe oben).
- **P3b Übersetzer:** Patch 0027 committet (487db41); BDA bleibt Standard aus (`KAR_BDA`).
  Dateien: vk.h, vk_memory.c (GPU-Puffer + Set), vk_pipeline.c (Layout), vk_exec_draw.c (Bindung).
- Rücksprung: `git reset --hard pre-16.3-bda`.

## Stand 2026-11-14 (kurz)
- Testläufe von kar.exe ohne Nachfrage (Dauer-OK), nur nicht wenn WardogsClient läuft.
- Bild-gezählte Skripte (menu_*, mode_*) brauchen `TOTK_FPS_LIMIT=60`; Hinweis-Dialoge aktivieren „OK“ erst nach Scrollen bis zum Ende.
- Neu standardmäßig an: karpace (16.6a), HUD am Rand (17.3b, `hud_position`/`KAR_HUD_EDGES`/`KAR_HUD_CUTS`), Compute-Vorbau nach Treiberwechsel mit Ladebildschirm (13.3, `KAR_CS_COLD`), Codec-Event-Wiederverwendung (7.2).
- Diagnose-Schalter: `KAR_SLEEP_LOG`, `KAR_HUD_COLS`, `KAR_UBO_LOG`/`KAR_UBO_PROG`, `KAR_MUTEX_LOG` (an), `KAR_BOOT_DEMO`/`KAR_BOOT_SHOT`.
- Nächste Schritte (advisor): Dauerlauf inkl. Menü-Leerlauf und Wiederholung (3.4/12.5), Validation-Lauf (12.3, beantwortet 2.6), HUD-Schalter ins Spielmenü (15.x).


## 15.4 PC-Menü Mechanik (2026-11-17)
- Anzeige-Seite hat EINEN Wähler bei Seite+0xb90 (Index +0x260 = Seite+0xdf0); Fokus lädt ihn aus Zeile+0xc (`f_700300c5f4`).
- Weg ohne Wirkung: +0xc zuerst schreiben, dann Setter `f_7002df08c0(Wähler, idx)` (row_show). Weg mit Wirkung (Spieloption live): Setter ohne Vorab-Schreiben, dann `f_7002df110c(Wähler,1)` (row_effect).
- Sonde für Live-Wirkung: Vollbild lokal auf Zeile 0 (Schriftgröße) legen (scratchpad probe0.py), kein Commit.
- Testskripte: Eingaben zählen Ticks, Shots zählen Frames; bei >60 fps laufen Frames schneller. Lauf endet nach letztem Shot.

## Performance messen (2026-11-17)
- Rennlauf: `kar_race_test.ps1 -Script kirby/scripts/fresh_race_ms.txt -Profile <Ordner nur mit account.uid aus build/profile-kar-fresh>` (Limit aus ini, 144), Shot bei Frame 17000 als Rennbeleg; `frametime: period`-Zeilen (Minute 2 = Rennen).
- Listen/Caches liegen neben der exe (`build/kar458752/shader-cache/cs-used.bin`, `pipeline-cache.bin`), nicht im Profil. `cs-used.bin` vor Experimenten sichern und danach zurück.
- Diagnosen: `KAR_REPLAY_PROF=1` (+ `KAR_REPLAY_PROF_MS=25`), `KAR_FT_SLOW_MS=25`; „compute not indexed“ = Shader nicht im Prefetch-Index.

## Stand 2026-11-18 (kurz; Verlauf: HANDOFF_ARCHIV.md)
- **HUD am Rand (17.3c, Standard bei hud_position=edges, Air Ride):** Elementweg statt Bandschnitte, nur wenn das
  Renn-Anker-Programm (57d04d70800e9af4, `KAR_RACE_KEY`) im Bild zeichnet. Seite aus Blockwörtern: exakt 1,0,−1,0 →
  links (Platz), andere schmale → rechts (Tacho/Runde), volle Höhe mit Breite ≠ 1 (Banner) → Mitte; Karte/Liste per
  genauem Block links; alles andere ganz mittig. `KAR_HUD_ELEMENTS=0` = alte Bandschnitte. Andere Modi: kein Anker → mittig.
- **17.17 ✅:** `kar_wide_size` rechnet jede Wunschgröße auf 1440 Zeilen um (1080er Ultrawide war Bild in der Ecke).
- **13.3:** Gebrauchsliste `CSU_MAX` 2000 (Air Ride + City Trial ~1800, ~0,9 MB je vorgebaute Pipeline). Erstbesuch
  City Trial wartet; Kirby hat keinen Installer-Weg für vorbereitete Shader.
- **16.6:** 141 fps vom 17.11. nicht mehr erreichbar, Code ausgeschlossen (alte Runtime/alter Übersetzer 111–124).
- **17.18 (vertagt):** `KAR_UI_FULL=1` (aus) streckt flache Vollbild-Overlays (gelesene Slots per Reflexion);
  Zielblitz voll, weiße Wischblende nur graue Streifen → nicht Standard. Pause-Dimmer = 17.19 (geparkt, siehe Plan).
- **18.1-Fix 4cc67a6** (Use-after-free der anderen KI behoben). Andere KI arbeitet evtl. weiter: vor Commits fetch/status.
- **Test-Rezepte:** Rennen `fresh_race_ms.txt`, mit Pause `fresh_race_pause_ms.txt`, Top Ride `top_ms.txt`, Fahrschule
  `school_ms.txt`, City Trial nur bildgezählt `mode_top.txt` (TOTK_FPS_LIMIT=60, landet nicht immer). Profil: Ordner mit
  nur `account.uid`; `cs-used.bin` vor/nach jedem Lauf aus Sicherung (`E:/kirby/backup/cs-used.bak`) zurückspielen.
  Ein Lauf endet nach dem letzten Shot. Andere Auflösung: `TOTK_OUTPUT_RES`/`KAR_DISPLAY_RES`. Alte Runtime: export +
  `build_game.py --runtime … --out …` + `-Exe` (Junctions mit `cmd /c rmdir` entfernen).
- **Diagnose-Schalter:** `KAR_UI_INV`, `KAR_UI_ANCHOR`, `KAR_UI_DUMP(_AUTO)`, `KAR_UI_SKIP=<f>:k<key>`, `KAR_UI_PROBE`,
  `KAR_UI_FULL_LOG`, `KAR_HUD_COLS`, `KAR_CS_LOG`, `KAR_CS_PF_LOG`, `KAR_CS_USED_MAX`.
- **Validation (1.9, 2026-11-18):** VUID-08600 – Set 2 (Bindless) bei Draws/Dispatches nicht kompatibel gebunden, ab Menü (unabhängig von 4f37ae8). Gegenlauf KAR_BINDLESS=0 brach früh ab → wiederholen, Bind-Stellen lesen.
- **Nächst:** (2026-11-19) 18.9 ✅ (7dae53c, Validation Air Ride + City Trial sauber). 18.10 ✅ (730a2de + Dyn-UBO-Bit; kalter Test: pipeline-used/keys.bin kurz beiseitelegen, sonst kaum GPL-Links). 17.19 ✅ (`KAR_UI_BAND`: Pausen-Verlaufsstreifen über volle Breite); 2.6 geparkt (Stand in der Planzeile; nächster Schritt nativer Ampere-Code). 18.11 ✅ c2-Bank Standard an (`KAR_C2=0` aus). Dienst-Inventur: `tools/kar_service_audit.py build/extracted/kar458752/exefs/main [ns]` (3.11); Amiibo = stiller Stub (3.10). **SPIR-V-Override:** Dumps sind vor `spirv_set` (Set 0); Override-Dateien für fs mit eigenem Set brauchen `DescriptorSet 1`; 17.20 + Seitenstreifen = Nutzerentscheid. Läufe nur ohne WardogsClient. Geparkt: schwarzer Kirby (3.2). Module: `E:/kirby/backup/lack/`.

## Stand 2026-11-20 (Loop: Liste abarbeiten)
- **Loop-Regel (Nutzer):** erst kritisches Gameplay, dann Extras; vor jeder Umsetzung advisor; `get_usage` je Tick, Stopp bei 90 % weekly (ab ~87 % nichts Neues), dann Zusammenfassung + Push. Chat extrem knapp.
- **Neue Host-Stubs (ov_system.c):** nn::ldn (lokal aus, Suche leer), nn::friends (leere Listen), account LoadProfileImage (profile_image.h) + Netzwerkkonto „keins“, nn::album (MovieWriter init ok, Speichern schlägt fehl; Replays laufen), nn::ngc (nichts maskiert), nfp-Aufrufe geloggt.
- **Menü-Test-Rezept:** Profil `build/profile-kar-usercopy3` (für Modus-Einstiege Kopie nehmen), `TOTK_FPS_LIMIT=60`, Skript `500 A,560 A,700 A,950 B`, dann 4× DOWN (1700–1820) = Leiste; Hinweise: DOWN halten bis OK aktiv. Skripte `kirby/scripts/menu_*.txt`; Bildraster `tools/kar_shot_grid.py <run> <png>`; Dienste `tools/kar_service_audit.py`.
- **Weiter mit 17.21** (Nutzer wählte Variante 2: Streifen/Hintergründe in Menüs, nicht Longshot 17.20): Messung und Plan stehen in der Planzeile; Pool-Arrays jetzt bindless (`KAR_POOL_BL`, 2.7 ✅), `KAR_AMIIBO=1` = Leser ohne Figur.

- 2026-11-20: Alle 🔧-Zeilen aufgelöst (✅ belegt, ⬜ Rest offen, ⏸ wartet auf Nutzer: 9.5, 14.13). audout.c loggt Gain-Wechsel.
- 2026-11-20: 3.2 ✅ (Road Trip gespielt). 7.6 Speichern-Einträge im Spiel ausgeblendet, Bedingung offen (Label-Wahl main+0x2ccf5e0).
- 2026-11-20: 7.3 City Trial Ruckler gemessen, Ursache Gast-Sync (siehe Planzeile), nicht Shader/Pipeline.
- 2026-11-20 7.3: Ursache = Gast-Arbeit + Host-Submit; VirtualQuery-Cache + Log-Drossel committed, offen nvn2_lower-Exceptions, disk_load_from im Submit.
- 2026-11-25 7.1: Splitscreen-Eingabe fertig (P2 tritt bei, Rennen läuft), obere Hälfte 3D schwarz → nächster Schritt. Navigation heute: Menü startet auf Fahrschule, Popup schließt B@950, UP→Air Ride; Skriptzeiten = Ticks (~2,4 Frames).
- 2026-11-25 7.1: Splitscreen-Schwarz = Komposit-fs (nvn2 a6791d21435a041b) sampelt nur für y>0,5; Dump E:\kirby\sp20, GLSL sp20\comp_fs.glsl.
- 2026-11-25 7.1: Splitscreen 2P läuft (Lowering-Fix TEX.B-Scratch-Kopien unprädiziert). Offline-Kette: DKB_SHADER_DUMP → build/dkb_lower_test.exe → totk_xlate jobs.json (DMSH_STACK=1 zeigt 'Failed to find handle').
- 2026-11-25: -O2 A/B gemessen (≤10 % Gast, Rauschen) → bleibt -O1; Frame-Zeit = Submit-Wartezeit. EP-Regression nach Lowering-Fix OK.
- 2026-11-25: 7.4 ✅ (Top Ride + Road Trip 1. Etappe, Skripte angepasst).
- 2026-11-25: 18.6 ✅ (Rennuhr = Wandzeit bei 130 fps).
- 2026-11-25: 7.1 ✅ (City Trial 2P, Top Ride 2P getestet); 7.1b Tastatur+Pad als Extra.
- 2026-11-25: 14.5 ✅, 14.10 ✅.
- 2026-11-25: 14.10 nachgeprüft (Vollbild 3440/2560, Top-Ride-HUD mittig → 14.10b); toRace/soak60 an Fahrschul-Menü angepasst.
- 2026-11-25: 13.3 Release-Blocker: frischer Nutzer 4–5 min <3 fps beim ersten City Trial (Compute-Treiberbau 650-KB-Shader, synchron).
- 2026-11-25: 13.3 Seed-Prototyp (KAR_CS_SEED, aus): CS-Übersetzung ~0,8 CPU-s je Shader = Engpass → Übersetzer profilieren.
- 2026-11-25: Patch 0028 (Übersetzer 2–5x schneller bei großen CS, bytegleich) committet; build/xlate nicht neu gebaut.
- 2026-11-25: 13.3 Bind-Seed Standard an: Erstbesuch City Trial 0–78 s <20 fps statt 250–270 s (mit Patch 0028).
- 2026-11-25: 10.2 ✅ (Kill mitten im Commit → Rollback, Spielstand heil).

## Stand 2026-11-25 (Loop: Liste, Weekly ≤ 80 %)
- **Nutzerentscheide:** Einstellungen nur im Port-Overlay (ImGui), keine eigenen Einträge im Kirby-Optionsmenü (`karmenu` aus, `KAR_MENU=1` an); Blöcke 14 (offene Punkte) und 15 gestrichen/zurückgestellt.
- **FSR 3.1** (`upscaler = fsr3`): Jitter per Viewport-Versatz im Replay (vk_exec_draw.c record_fixed → vk_kar_jitter_viewport), Richtung per 40-px-Test bestätigt; Flimmer-Beweis mit stehender Kamera steht aus (B halten). FFX braucht `rec_drain`.
- **DLSS-Zusatz:** `E:/kirby/kar-dlss` (lokales git, MIT), `powershell -File build.ps1 -Sdk E:/kirby/dkb-pc-recomp/refs/dlss -Install <exe-Ordner>`; liegt in build/kar458752 (`upscaler = external`, Overlay „NVIDIA DLSS“). check_content/package_build sperren die DLLs.
- **Bildgenerierung:** Overlay-Schalter (`frame_gen`, Neustart); MV-Abdeckung im Rennen 95 %.
- **DLSS:** SDK v310.9.16 in `refs/dlss` (ignoriert). Lizenz 4e → nie in die GPL-exe. Plan: neutrale Upscaler-Schnittstelle im Repo, DLSS-Zusatz-DLL in eigenem Ordner außerhalb (eigener Code, MIT, Nutzer baut selbst, SDK lädt das Build-Skript). Keine NVIDIA-Dateien/-Namen im Repo; `package_build.py` darf sie nie einpacken.
- **Soak:** `kirby/scripts/soak_loop.txt` (A-Schleife, alle 1800 Ticks DOWN×8/A/B/UP×8).
- **Shader:** Patch 0028 nur in `build/xlate-t`; Umstellen von `build/xlate` = Nutzerfrage (warmer Cache neu).
- **Ausstehende Tests (gebündelt am Ende):** DLSS-Jitter-Richtung (KAR_DLSS_JSX/JSY); Monitor-Platzierung (`monitor = n`); Nachttest `soak_night.txt` + memlog.ps1; Bilder nach Rennende für 7.6 (Speichern-Zeile); 7.1b Tastatur als Spieler 2 mit echtem Pad (Nutzer).
- **Arbeitsliste (Reihenfolge, 2026-11-25):** 2.6 3.9 4.5 4.6 4.7 7.6 7.7 12.5 13.2 16.3 16.4 16.5 16.9 16.12 17.21 17.18 17.6 17.7 17.11 17.14 18.3 · Nutzer: NUTZERTESTS.md · zurückgestellt bleiben ⏸. Validation + Splitscreen erledigt (af00248).
- **Stand 2026-11-25 abends:** 1.12 ✅ (native Geometrie: nvn2_lower.cpp `options.geometry` bei SPH-Typ 4, nur Kirby, LEPC-Adresse fest 0x10000000; Eglantine-Absenkung + SHF.L.U64.HI, FSEL/FMUL/IMAD/I2FP/ISETP/LEA uniform, TEX-Registerzahlen; 62/69, Rest 1.12b ⏸). Diagnose: `nvn2: program … drops stage`, `bind of program … without stage(s)`. Zurückgestellt heute: 2.6, 3.9, 3.10, 4.6, 4.7, 7.6, 7.7, 17.4, 1.12b. Erledigt: 12.3, 12.5 (73 min), 4.5, 7.1b, 17.2/9/10/12/13/15, 11.4.
- **Nächst (Reihenfolge):** 13.2 Installer (Plan in der Zeile), dann 16.3/16.4/16.5/16.9/16.12, 17.21, 17.18, 17.6, 17.7, 17.11, 17.14 (braucht Discord-App-ID), 18.3. Offene Nutzerfragen: Patch 0028 (`build/xlate`), kar-dlss veröffentlichen, Discord-ID.
