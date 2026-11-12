# Running Kirby Air Riders Recomp

This guide covers building and running the *Kirby Air Riders* (Switch 2, update 458752 = 1.3.3) port that lives in
this DKB Recomp clone, and the switches added for it. Nothing from the game is in the repository: you build with your
own dump, and the executable, the generated code and the shader cache stay on your machine. Progress notes and the
per-fix log are in `E:\kirby\KIRBY_RECOMP_PLAN.md`.

## What works

- Boot, title screen, attract demo (full 3D through the upscale fallback below) and the tutorial video.
- Menus: course select, character and machine select, "Ready".
- Races, in full 3D with the HUD, about 59 fps once the shader and pipeline caches are warm.
- Sound, through the game's AudioOut on the default playback device (SDL3). `TOTK_AUDIO=none` mutes it.
- German text and km/h by default (`TOTK_LANG` or the port menu's "Game language" changes it; the speed unit is
  stored in the save, so a save made under US English keeps mph).
- Button symbols in tips, from the stand-in font `fonts\5.ttf` or your own dumped `fonts\FontNintendoExtended.ttf`.
- FSR 1 upscaling in place of DLSS (the game's DLSS output is filled by the upscale fallback).

## From your own dump to a running game

You need your own `.nup` dump of the game with update 1.3.3, Windows 10 or 11, a Vulkan 1.3 GPU, MSYS2 UCRT64 Clang,
the .NET 10 SDK (for the shader translator) and the two Python environments from `docs/building.md`. No keys are
needed: the `.nup` container holds the files unencrypted.

1. Extract the game code (ExeFS) of the newest update:
   `.venv-py314\Scripts\python tools\extract_nup.py <your>.nup --select latest --only exefs --out build\extracted\kar458752`
2. Pack the game data (RomFS, about 20 GB) into one file:
   `.venv-py314\Scripts\python tools\extract_nup.py <your>.nup --select latest --only romfs --pack --out build\extracted\kar458752-packed`
3. Build the shader translator once: `.venv\Scripts\python tools\build_xlate.py`.
4. Build the game (next section). The first build lifts the whole game code and takes a while.
5. Start `build\kar458752\kar.exe` (section "Run"). The first race translates and compiles shaders; later starts are
   fast.
6. Optional, a folder that runs from anywhere (exe, DLLs, translator, ExeFS, caches and an empty `profile` folder for
   the saves): `.venv\Scripts\python tools\package_build.py --version kar458752 --extracted build\extracted\kar458752-packed`
   and copy `romfs.bin` into its `romfs` folder (or pass `--with-romfs`). The folder holds game code: keep it for
   your own machines.

## Build

From the repository root, with MSYS2 UCRT64 Clang on `PATH` (`C:\msys64\ucrt64\bin`):

```
.venv\Scripts\python tools\build_game.py --version kar458752 --expected tools\data\expected-kar458752.json --romfs build\extracted\kar458752-packed\romfs
```

A runtime-only change rebuilds in seconds. The result is `build\kar458752\kar.exe` (window "Kirby Air Riders Recomp",
icon `runtime\kar.ico`, drawn by `tools\make_kar_icon.py`). The build fails to link while `kar.exe` is running.

The shader translator is `build\xlate\totk_xlate.exe` (`tools\build_xlate.py`). Changing it changes its identity, so
every cached shader (about 6,600) is translated again on the next start.

## Run

Start `build\kar458752\kar.exe` from that folder. Close it with the window's close button: killing the process loses
the pipeline cache (`pipeline-cache.bin`) and profiles, and the next start compiles everything again.

The game reports docked mode (`TOTK_OPERATION_MODE`) and a 2560x1440 display, which is the size of its window
textures.

The log goes to `logs\kar.log` next to `kar.exe` (the last five runs are kept as `kar.1.log` to `kar.4.log`). A crash
writes its report there too: the faulting address, the lifted function and the guest registers. Copy the log before
starting again after a crash. With `-RedirectStandardError` the log goes to that file instead.

## Controls

Input works as described in `docs/controls.md` (first SDL3 gamepad plus keyboard and mouse, merged into one Switch
controller; Esc or F1 opens the settings menu, whose Controls tab remaps buttons). The keys for the buttons Kirby Air
Riders' tips name so far:

| Switch | Keyboard | In Kirby Air Riders |
|---|---|---|
| Left stick | W A S D | steer |
| B | Left Shift | hold to brake and charge the boost (tip "Turbo-Aufladen") |
| A | E | confirm in menus |
| L | Q | shown in race prompts |
| Plus | Tab | pause |

Only one controller is merged, so local multiplayer with several pads is not supported yet.

## Saves

The save lives in `build\profile-kar458752\save\<user id>\` (`TOTK_PROFILE_DIR` moves the whole profile). Saving is
journaled as on the console: the game writes a working copy in `save-tx\`, and a commit replaces the save in one step,
so a crash while saving leaves the previous save. The three saves before the current one are kept in
`save-backup\<date-time>\`; to go back, close the game and copy one of them over `save\<user id>\`.

The speed unit (mph or km/h) is stored in the save when it is created, from the game language at that time.

For test races, `build\profile-kar-test` holds a copy of an older save in which `toRace.txt` reaches a race; a fresh
German save goes to the driving school first. `tools\kar_race_test.ps1 -Runs 5 -Script <toRace.txt>` runs several such
races and flags rainbow or black runs with `tools\kar_shot_check.py`.

## Switches added for Kirby

| Variable | Default | Effect |
|---|---|---|
| `KAR_DISPLAY_RES=WxH` | `2560x1440` | Display resolution reported by `nn::oe::GetDefaultDisplayResolution`. 1920x1080 leaves the picture in the top-left corner. |
| `KAR_UPSCALE_FALLBACK` | on for Kirby | `0` turns off the missing-upscale fallback. The game upscales with DLSS, which the host does not run. A DLSS-like output (rgb10a2, 16:9, at least 1280 wide) nothing has drawn into in the last 8 frames is filled each frame from the newest smaller render target of its format and aspect ratio, through FSR 1 EASU plus the linear decode the game's composite expects. |
| `KAR_UPSCALE_ANY=1` | off | The fallback fills any never-drawn sampled colour image, as before (it overwrote shadow and minimap targets). Diagnostic only. |
| `KAR_UPSCALE=blit` | FSR 1 | A plain blit instead of FSR 1 EASU. `KAR_UPSCALE_DECODE=0` skips the linear decode (brighter but washed out), `sq` squares instead. |
| `KAR_TEX_VERIFY=1` | off | On every texture freshness check the write watch calls clean, hashes the storage and compares it with the hash at the last upload; logs and re-uploads a texture whose bytes changed unseen. Slow; for finding stale textures. |
| `KAR_ARR_UBO=0` | on | Material texture arrays (handles in c5..c8) get the dummy texture. Diagnostic only. |
| `KAR_DIAG=1` | off | Logs where material texture handles were read from (inline update, submit-time copy, live memory), every 2^20 reads. |
| `KAR_BUILTIN_FONTS=<dir>` | `fonts` | Folder of the built-in fonts: `<dir>/FontNintendoExtended.ttf` (and the other names console font dumpers write) from your own console first, then `<dir>/<type>.ttf` (the button-symbol stand-in from `tools/make_kar_buttonfont.py`), then Segoe UI or Arial. |
| `TOTK_LIB_PREBUILD` | off for Kirby | `1` turns the boot pipeline prebuild back on (about 7 GB more memory for the same race frame rate). |
| `KAR_CS_SPV_MAX` | 4 MiB | Compute shaders with more SPIR-V than this are not built and their dispatches are skipped. `0` means no cap. |
| `KAR_CS_ASYNC` | on | `0` builds compute pipelines on the replay thread, as before. |
| `KAR_FORCE_DLSS=1` | off | Sets the engine's `EnableDLSS` flag (main+0x9381810) at startup. Diagnostic only; the game still made no DLSS call. |
| `KAR_DLSS_WATCH` | off | Logs the `EnableDLSS` flag every 300 frames. |
| `KAR_DLSS_LOG=n` | 400 | Logs the first n calls of the DLSS procs (`runtime\nvn_api_dlss.c`). |
| `KAR_DLSS_STORAGE` | 0x10000 | Memory size the DLSS stand-in reports to the game. |
| `KAR_LACK_DUMP=<dir>` | off | Saves the SPIR-V and logs the reflection of shaders that fail the pipeline binding check. |
| `KAR_HANG_DUMP=<s>` | off | s seconds after the game's main thread starts, logs every guest thread once: what it waits on, its latest SVC, pc and lr. |
| `TOTK_FPS_LIMIT` | 60 | Kirby steps its game by measured time, so limits from 30 to 360 (or `display`) apply without a timing patch: presents and the vsync event the game waits on each frame follow the limit (`runtime/frame_rate.c` `frame_rate_self_timed`). The race timer keeps wall time; above ~66 fps in a race the replay CPU time limits the rate. |
| `KAR_CS_PREFETCH` | on | `0` turns off the compute prefetch: when the game creates a program, its compute shader is loaded and its pipeline built in the background, so its first dispatch does not load ~1 MB of SPIR-V in the race. Only shaders a dispatch used in one of the last 8 sessions (`shader-cache/cs-used.bin`); `KAR_CS_PREFETCH_ALL=1` takes every stored one. `KAR_CS_PREFETCH_MB` (128) caps the SPIR-V waiting for its pipeline. |
| `KAR_CS_DROP_SPIRV` | on | `0` keeps a shader's SPIR-V after its pipeline is built (it is loaded again when needed). |
| `KAR_PIPE_PREWARM_ALL=1` | off | The boot prewarm builds every recorded pipeline, not only those a draw used in the last 8 sessions (`pipeline-used.bin`). |
| `KAR_APP_PIPELINE_CACHE=1` | off for Kirby | Uses the application pipeline cache (`pipeline-cache.bin`); off, the driver's own cache serves (1.1 GiB less memory, no 20 s cache saves). |
| `TOTK_VRAM_HEADROOM=1` | off for Kirby | Allocates the next GPU memory block ahead on a background thread (~0.9 GB of commit for no measured gain here). |
| `TOTK_BOOT_SCREEN=0` | on | Turns off the boot screen that shows the shader and pipeline prewarm's progress before the game starts. |
| `KAR_REPLAY_PROF=1` | off | Replay timing: submissions over 100 ms log their slowest op kinds; every 600 frames the replay time per frame and its `bind_resources` share. |
| `KAR_THREAD_CPU=1` | off | Every 600 frames, the busiest threads with their share of a core. |
| `KAR_MEM_MAP=1` | off | The memory census adds a map of committed memory by kind and the largest private allocations (`KAR_MEM_MAP_DUMP=<dir>` saves samples of them). |
| `KAR_TRACE_FRAMES=a-b,…` | off | Every guest function entry in those frames (address range `KAR_TRACE_RANGE=lo-hi`) to `E:/kirby/trace.bin`, for offline comparison of game phases. `KAR_FNCOUNT=<frames>` writes call counts per function instead. |

## Testing without looking at the window

These come from TotK and DKB Recomp; [dkb-testing.md](dkb-testing.md) has the full list.

- `TOTK_SHOTS=lo-hi:step` with `TOTK_SHOTS_DIR=<dir>` saves presented frames as PNGs.
- `TOTK_DUMP=<frame>` with `TOTK_DUMP_DIR=<dir>` writes every pass of one frame (`passes.txt`, `images.txt`, PNGs).
  `TOTK_DUMP_UBO=1` adds uniform contents and `TOTK_DUMP_TEX` adds sampled textures. Material array elements show as `tex arr c5[k] ... raw <handle>`; `TOTK_DUMP_TEX=1x1`
  adds the 1x1 parameter textures.
- `TOTK_VK_VALIDATION=1` with `TOTK_SHOTS` is the quickest way to find undefined Vulkan use (count the `VUID-` names
  in the log). It found the compare-on-colour bug behind the rainbow runs.
- `TOTK_INPUT_SCRIPT=<file>` presses buttons on given frames, one `frame button hold` line each. For example,
  `A` at frames 500, 560, 700, 900, 1100 and 1300 reaches character select, and A every 200 frames up to 2500
  starts the Test Ride.
- `TOTK_VK_VALIDATION=1` turns on the Vulkan validation layer (needs the Vulkan SDK).
- `DKB_SHADER_DUMP=<dir>` saves each NVN 2 shader stage as `.ctl`/`.bin`. `tools\dkb_lower_test.cpp` lowers such a
  folder offline.

On a warm shader cache, an idle start reaches the live attract demo at about frames 1500-2300.

## Known gaps

The full list with status is in `E:\kirby\RELEASE_PLAN.md`. The main ones:

- Rainbow surfaces in about six of ten runs: colour textures sampled with a depth compare (material arrays declared
  2D-shadow as well). A depth stand-in is built in (`vk_exec_draw.c shadow_dummy`) but not yet verified in game.
- Kirby glows white in dark areas (cave); may share the cause above.
- The HUD item box shows a grey square and the menu's preview card only its placeholder icon.
- 82 compute shaders read a texture handle from c0 outside their declared tables and get the dummy texture;
  `KAR_C0_TEX=1` tries the word there (experiment, off by default).
- First race after a cache wipe stutters while pipelines build (mean 45 to 48 fps, 1% low 7 fps); warm races hold
  60 fps.
- Memory: about 24 GB committed and 12 GB working set in a race. The game's heap is committed in full when it is
  sized, which raises the commit charge but not the RAM in use.
- 59 tessellation stages fail Ampere lowering on LEPC. No draw in the menus or races uses them (draw census: 0 skipped).
- Untested: City Trial, Top Ride, Road Trip, real controller play, sessions over 15 minutes.
