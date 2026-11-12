# Kirby Air Riders – nativer PC-Port (statische Rekompilierung)

Ein nativer Windows-Port von *Kirby Air Riders* (Switch 2, Update 458752 = Version 1.3.3). Der Spielcode (ARM64) wird
**statisch in C übersetzt** und mit Clang zu einer normalen `kar.exe` gebaut. Grafik (NVN → Vulkan), Ton, Eingabe,
Dateisystem und Systemdienste stellt eine eigene Laufzeit bereit. Es gibt **keinen Emulator** und keinen JIT.

Das Projekt baut auf dem *DKB Recomp* (Donkey Kong Bananza) auf und erweitert ihn um alles, was Kirby Air Riders braucht.
Die ursprüngliche README des Unterbaus liegt unter [`docs/README-dkb-upstream.md`](docs/README-dkb-upstream.md).

> **Keine Spieldaten in diesem Repository.** Spielcode, übersetzter („gelifteter“) C-Code, RomFS, Shader-Caches,
> Bildschirmfotos und Bauergebnisse bleiben auf deinem Rechner. Du brauchst einen eigenen Dump des Spiels.
> `.gitignore` und `tools/check_content.py` (Pre-Commit-Hook/CI) verhindern, dass solche Dateien eingecheckt werden.

---

## Stand

| Bereich | Stand |
|---|---|
| Start, Titel, Intro-Demo, Tutorial-Video | läuft |
| Menüs, Charakter-/Fahrzeugwahl, Rennen (3D + HUD) | läuft, ca. 60 fps im Rennen (CPU-gebunden im Replay) |
| Ton (AudioOut über SDL3) | läuft |
| Deutsch + km/h als Standard | läuft (`TOTK_LANG` ändert die Sprache) |
| Hochskalierung FSR 1 statt DLSS | läuft |
| Start mit Ladebildschirm, Pipeline-Vorbau | ~1 s Start mit warmem Cache |
| Speicher | ~6,7 GiB Arbeitsspeicher, ~10,6 GiB privat (Ziel < 8 GB) |
| Eigene PC-Optionen im echten Kirby-Optionsmenü | in Arbeit (Text-Haken läuft, siehe unten) |
| Ultrawide 21:9 / 32:9 | in Arbeit |
| Lokaler Mehrspieler, City Trial / Top Ride / Road Trip vollständig geprüft | offen |

Die vollständige, priorisierte Aufgabenliste steht in [`kirby/RELEASE_PLAN.md`](kirby/RELEASE_PLAN.md),
das laufende Arbeitsprotokoll in [`kirby/HANDOFF.md`](kirby/HANDOFF.md).

---

## Voraussetzungen

- Windows 10/11, GPU mit Vulkan 1.3 (bisher auf NVIDIA getestet)
- MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin` im `PATH`)
- .NET 10 SDK (für den Shader-Übersetzer)
- Python: die beiden Umgebungen `.venv` und `.venv-py314` (siehe [`docs/building.md`](docs/building.md))
- Ein eigener `.nup`-Dump von Kirby Air Riders mit Update 1.3.3 (keine Schlüssel nötig)

## Vom Dump zum laufenden Spiel

```bat
:: 1. Spielcode (ExeFS) des neuesten Updates entpacken
.venv-py314\Scripts\python tools\extract_nup.py <dein>.nup --select latest --only exefs --out build\extracted\kar458752

:: 2. Spieldaten (RomFS, ~20 GB) in eine Datei packen
.venv-py314\Scripts\python tools\extract_nup.py <dein>.nup --select latest --only romfs --pack --out build\extracted\kar458752-packed

:: 3. Shader-Übersetzer einmal bauen
.venv\Scripts\python tools\build_xlate.py

:: 4. Spielcode nach C übersetzen (mit Kirby-Patch-Tabelle und dem EffectScript-Modul)
.venv\Scripts\python recomp\gen.py --exefs build\extracted\kar458752\exefs --out generated\kar458752 ^
    --modules main,subsdk0,subsdk1,sdk ^
    --nro effectscript=build\extracted\kar458752\romfs\Bin\Module\EffectScript.nro ^
    --patches recomp\patches-kar458752.txt

:: 5. Bauen
.venv\Scripts\python tools\build_game.py --version kar458752 --expected tools\data\expected-kar458752.json --romfs build\extracted\kar458752-packed\romfs
```

Ergebnis: `build\kar458752\kar.exe`. Der erste Start übersetzt und kompiliert die Shader, spätere Starts sind schnell.
Beenden immer über das Fenster schließen, nicht den Prozess abschießen (sonst geht der Pipeline-Cache verloren).
Während `kar.exe` läuft, schlägt das Linken fehl.

Eine portable Ordner-Variante (nur für eigene Rechner, enthält Spielcode):
`.venv\Scripts\python tools\package_build.py --version kar458752 --extracted build\extracted\kar458752-packed`

Ausführliche Anleitung mit allen Schaltern: [`docs/kirby-running.md`](docs/kirby-running.md).

---

## Wie es funktioniert

```
 .nup-Dump ──extract_nup──► ExeFS (main, subsdk0/1, sdk) + RomFS
                              │
                     recomp/gen.py  (ARM64 → C, Patch-Tabelle)
                              │
                     generated/kar458752/*.c   (nur lokal!)
                              │
       runtime/*.c  ──────────┼──── Clang ───► kar.exe
  (Kernel, Speicher, NVN→Vulkan, Audio, HID,
   Dateisystem, nn::-Dienste, Port-Menü)
```

- **Lifter** (`recomp/`): übersetzt jede Gastfunktion in eine C-Funktion `f_<Adresse>`. Gastadressen = Hostadressen
  (Module ab `0x7000000000`). Indirekte Sprünge laufen über eine Dispatch-Tabelle (`runtime/dispatch.c`).
- **Patch-Tabelle** (`recomp/patches-kar458752.txt`, Format in `recomp/patches.py`): ersetzt einzelne Spielbefehle
  beim Generieren, zur Laufzeit per Schalter an/aus. Neu für Kirby: Ersatzwort `@call:<Versatz>` ruft eine
  Host-Funktion auf der Patch-Seite auf (ohne die 128-MiB-Reichweitengrenze von `bl`).
- **Grafik** (`runtime/nvn_*`, `runtime/vk_*`): NVN-Aufrufe des Spiels werden aufgezeichnet und als Vulkan-Befehle
  abgespielt („Replay“); Shader (Maxwell/Ampere) übersetzt `build/xlate/totk_xlate.exe` nach SPIR-V.
  Kirby-spezifisch: FSR-1-Hochskalierung statt DLSS (`runtime/vk_kar_upscale.c`, Shader in `runtime/shaders/kar_*`),
  Compute-Prefetch, Nutzungslisten über Sitzungen (`cs-used.bin`, `pipeline-used.bin`) für schnellen Start und wenig
  Speicher.
- **Systemdienste** (`runtime/ov_*.c`): `nn::oe`, `nn::hid`, Audio, Netz (offline), Film-Decoder (`ov_movie2.c`),
  `nn::ro` (EffectScript-NRO) usw.
- **Kirby-Optionsmenü** (`runtime/kar_menu.c`, Patch-Set `karmenu`): hängt sich in die Textsuche des Spiels
  (`f_70011302c0` → `f_7000001300`) und in das Setzen von UI-Texten ein, damit eigene PC-Einstellungen im
  Original-Design des Spiels erscheinen. Ziel: Reiter „PC-Grafik“ auf der Seite *Optionen → Anzeige*, umschaltbar mit
  L/R. Stand und Befunde: `kirby/RELEASE_PLAN.md`, Abschnitt 15.
- Alle Kirby-Sonderfälle der Laufzeit fragen über `rt_game_is_kar()` ab (`runtime/main.c`).

## Wichtige Schalter (Umgebungsvariablen)

| Schalter | Wirkung |
|---|---|
| `TOTK_FPS_LIMIT=60` | Bildratenbegrenzung (Kirby läuft nach Wanduhr; Testskripte brauchen 60) |
| `KAR_DISPLAY_RES=WxH` | Anzeige-Auflösung, die das Spiel sieht (Standard 2560x1440) |
| `TOTK_OUTPUT_RES`, `TOTK_WINDOW_SIZE` | Ausgabe-/Fenstergröße |
| `KAR_MENU=0` | Haken im Optionsmenü aus |
| `KAR_MENU_LOG=<Teilstring>` | protokolliert Textsuchen/Textsetzen mit Bildnummer und Aufrufkette |
| `KAR_CS_PREFETCH=0`, `KAR_PIPE_PREWARM_ALL=1` | Compute-Prefetch aus / alle Pipelines vorbauen |
| `KAR_REPLAY_PROF=1` | Zeitmessung des Replays je Bild |
| `KAR_FNCOUNT=<Bilder>`, `KAR_TRACE_FRAMES=a-b,…` | Aufrufzähler / Aufruf-Trace für Offline-Analyse |
| `TOTK_SHOTS=lo-hi:step`, `TOTK_SHOTS_FULL=1` | Bildschirmfotos in Serie (volle Größe) |

Vollständige Liste: [`docs/kirby-running.md`](docs/kirby-running.md).

## Testen

`tools/kar_race_test.ps1` startet `kar.exe` mit einem Eingabeskript, macht Bildschirmfotos und sammelt das Log:

```powershell
powershell -File tools\kar_race_test.ps1 -Runs 1 -Script kirby\scripts\toRace.txt -Shots 2300-3900:800 -ShotCount 3 -Env 'TOTK_FPS_LIMIT=60'
```

Eingabeskripte (`kirby/scripts/*.txt`, Zeilen `<Bild> <Taste> <Dauer>`):

| Skript | Weg |
|---|---|
| `toRace.txt` | Titel → Rennen |
| `menu_opt.txt`, `menu_anz.txt`, `menu_disp.txt` | Hauptmenü → Optionen / Anzeige / Barrierefreiheit |
| `menu_anz_r.txt` | Anzeige-Seite + R (PC-Modus) |
| `mode_top.txt`, `mode_city.txt`, `mode_road.txt` | Top Ride / City Trial / Road Trip |
| `soak60.txt` | Langzeittest |

Analyse-Werkzeuge in `kirby/tools/`: `kar_assets.py` (HAL-Archiv `assets.cat`, „PARC“ v4), `kar_msbt.py`
(MSBT-Texte), `trace_*.py` (Aufruf-Traces), `spv*.py`/`csvariants.py` (Shader), `shotmean.py`/`kirbymean.py`
(Bildvergleich). Weitere Kirby-Werkzeuge liegen in `tools/kar_*.py`.

## Repository-Aufbau

| Pfad | Inhalt |
|---|---|
| `recomp/` | Lifter, Generator, Patch-Tabellen |
| `runtime/` | Laufzeit (C/C++), Kirby-Dateien `kar_*`, `vk_kar_upscale.c`, `ov_movie2.c`, `audout.c`, … |
| `tools/` | Bau, Paket, Test, Kirby-Werkzeuge `kar_*` |
| `docs/` | Doku (Kirby: `kirby-running.md`, `kirby-changelog.md`) |
| `kirby/` | Aufgabenliste, Arbeitsprotokoll, Eingabeskripte, Analyse-Werkzeuge |
| `build/`, `generated/`, `refs/` | **nur lokal**, nie eingecheckt |

## Rechtliches

Dieses Repository enthält ausschließlich eigenen Code und Code des Unterbaus (Lizenzhinweise in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)). *Kirby Air Riders* ist eine Marke von Nintendo / HAL Laboratory.
Der Port ist ein privates Forschungsprojekt; für die Nutzung ist ein legal erworbenes Spiel nötig.
