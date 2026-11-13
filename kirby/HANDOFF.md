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

## Testing (needs the user's OK to start the game)
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
