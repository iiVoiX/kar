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
- `toRace.txt` (old save) no longer reaches a race. Shots are 480x270 unless `TOTK_SHOTS_FULL=1` (ultrawide looks
  squeezed in small shots).
- Log lines: `TOTK_FT_PERIOD=5` (fps per 5 s), `TOTK_TRACE=present`, `KAR_PRESENT_PROF=1`.

## State (2026-11-13)
- Race ~105 fps at 3440x1440 (bindless + shadow write-back on by default). Limit 60 shows ~20 frames > 25 ms per 5 s
  with and without frame generation (own open finding).
- Frame generation (16.7): `KAR_FG=1` runs (runtime/vk_fg.cpp, FFX optical flow + interpolation, own pacing), but the
  interpolated picture is ~a blend: the game renders no motion vectors (its DLSS class is never built in 1.3.3) and
  optical flow is noise under the game's motion blur. Tools: `KAR_FG_DUMP=<n>` (+`KAR_FG_DUMP_DIR`), `KAR_FG_DEBUG=1`.
- Open bugs: ultrawide figure/machine select (3D figures clipped in their frames); empty speech bubble 2.6 (fix behind
  `KAR_POOL_BL=1`, untested); Top Ride card garbage 1/10; dark City Trial scene.

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
- **17.18 (vertagt):** `KAR_UI_FULL=1` wirkt derzeit nicht (strenger Test greift nie; Erst-Slot-Version streckte den
  Zielblitz) → gelesene Slots aus Shader-Reflexion nötig. Pause-Dimmer offen (vermutlich Szenen-Pass).
- **18.1-Fix 4cc67a6** (Use-after-free der anderen KI behoben). Andere KI arbeitet evtl. weiter: vor Commits fetch/status.
- **Test-Rezepte:** Rennen `fresh_race_ms.txt`, mit Pause `fresh_race_pause_ms.txt`, Top Ride `top_ms.txt`, Fahrschule
  `school_ms.txt`, City Trial nur bildgezählt `mode_top.txt` (TOTK_FPS_LIMIT=60, landet nicht immer). Profil: Ordner mit
  nur `account.uid`; `cs-used.bin` vor/nach jedem Lauf aus Sicherung (`E:/kirby/backup/cs-used.bak`) zurückspielen.
  Ein Lauf endet nach dem letzten Shot. Andere Auflösung: `TOTK_OUTPUT_RES`/`KAR_DISPLAY_RES`. Alte Runtime: export +
  `build_game.py --runtime … --out …` + `-Exe` (Junctions mit `cmd /c rmdir` entfernen).
- **Diagnose-Schalter:** `KAR_UI_INV`, `KAR_UI_ANCHOR`, `KAR_UI_DUMP(_AUTO)`, `KAR_UI_SKIP=<f>:k<key>`, `KAR_UI_PROBE`,
  `KAR_UI_FULL_LOG`, `KAR_HUD_COLS`, `KAR_CS_LOG`, `KAR_CS_PF_LOG`, `KAR_CS_USED_MAX`.
- **Nächst:** 17.18 per Reflexion (gelesene Fragment-Slots). 16.6-Ruckler = Erstnutzung; Grafik-Liste `PU_KEEP` 8 Sitzungen als Risiko offen. Sicherungen: `E:/kirby/backup/` (cs-used, pipeline-used, pipeline-keys).
