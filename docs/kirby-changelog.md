# Kirby Air Riders Recomp: changes

Newest first. The detailed per-fix log with causes is `E:\kirby\KIRBY_RECOMP_PLAN.md` (fixes #1-37); the open list is
`E:\kirby\RELEASE_PLAN.md`.

## 2026-11-11

- Dark 3D picture: still open. A try that left the DLSS stand-in's output sRGB-encoded matched the console's
  brightness (start line mean 132 → 182, console 199) but washed the colours out, so it is off again
  (`KAR_UPSCALE_DECODE=0` shows it).
- NVN 2 compute shaders: texture and image handles are found again (the count bytes +0xb4/+0xb5 are included,
  `nvn2_lower.cpp`); depth pyramid and light culling no longer read stand-ins.

## 2026-11-10

- **Rainbow fixed.** The game clears its float vertex-data textures with an integer zero clear, which the
  replay skipped; those textures then kept old heap bytes in about half of the starts. A zero clear now always runs
  (`vk_exec_xfer.c clear_tex`). 8 of 8 test starts clean.
- Diagnostics from the hunt, all behind environment switches: per-draw pixel history (`TOTK_DUMP_DRAWS`), texture
  and clear logs (`KAR_ZERO_TEX`, `KAR_DIAG_CLEAR`, `KAR_DIAG_UNWRITTEN`), in-run A/B switches (`KAR_ALT_*`).
- SMAA blit dither words are zero for Kirby, as on the console (Lichii PR #56 finding).
- FSR 1 RCAS sharpening for the DLSS stand-in, only when the player sets the port menu's sharpness (off by default:
  the table's default of 80 made the picture visibly harsher).
- The port menu's volume slider now works for Kirby, whose sound goes through AudioOut.
- A clearer error when the ExeFS is from another game version.
- Memory census lines show the process's private memory and working set.
- `KAR_C0_TEX=1`: experiment for 82 compute shaders whose texture handle lies outside their tables.
- Test tools: `tools/kar_race_test.ps1` (N races with screenshots) and `tools/kar_shot_check.py` (flags rainbow or
  black runs).
- Log file `logs\kar.log`; quieter log (diagnostics behind `KAR_DIAG`).

## 2026-11-09

- Compare samplers over colour textures read a depth stand-in (removed the Vulkan validation error 06479). The
  rainbow runs it was meant to fix still occur in about 3 of 5 runs.
- 41 shaders with "Direct" texture pool arrays get their real textures.
- The upscale fallback fills only the DLSS output, no longer unrelated textures.
- Boot pipeline prebuild off for Kirby: about 5 GB less memory.
- Button symbols in tips (stand-in font, or your own dumped `FontNintendoExtended.ttf`).
- German and km/h by default.
- FSR 1 EASU with the linear decode the game's composite expects (fixed the washed-out picture).
- Sound through AudioOut and SDL3.
- Races in full 3D, menus, attract demo; program renamed `kar.exe` with its own icon.
