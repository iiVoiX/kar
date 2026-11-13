# Handoff: Kirby Air Riders Recomp (read this first in a new session)

Repo: `E:\kirby\dkb-pc-recomp` (DKB Recomp clone). **No commits**; the user tracks changes in their own repo.
Other docs: `E:\kirby\ROADMAP.md` (priorities), `E:\kirby\KIRBY_RECOMP_PLAN.md` (fix log #1-27),
`dkb-pc-recomp\docs\kirby-running.md` (build/run/env vars). Memory: `C:\Users\Luca\.claude\projects\E--kirby\memory\`.

## Build / run

```
cd E:\kirby\dkb-pc-recomp   (PATH needs C:\msys64\ucrt64\bin first)
.venv\Scripts\python tools\build_game.py --version kar458752 --expected tools\data\expected-kar458752.json --romfs build\extracted\kar458752-packed\romfs
```
Exe: `build\kar458752\kar.exe`. Run from that folder with `Start-Process ... -RedirectStandardError bootNN.err`.
**Always close every `kar` process first** (`CloseMainWindow`), never kill. Don't launch while the user games
(check for big processes such as `WardogsClient*`; system commit must have headroom).
Test env: `TOTK_INPUT_SCRIPT=<scratchpad>\toRace.txt` (A at 500,560,700,900,1100,1300, then every 200 to 2500 → race
at ~3000), `TOTK_SHOTS=lo-hi:step` + `TOTK_SHOTS_DIR`, `TOTK_DUMP=<frame>` + `TOTK_DUMP_DIR` (+`TOTK_DUMP_UBO=1`),
`TOTK_AUDIO=none` for tests. Scratchpad: `C:\Users\Luca\AppData\Local\Temp\claude\970a58b68d0f7b20\3b4bfdd7-020d-4cec-acf9-ada5e1f9e555\scratchpad`
(scripts: pass_stats.py, val_all.py, lack_scan.py, dlss_*.py, missing_ov.py).

## State (2026-11-09 night)

Works: boot, menus, attract demo, races in full 3D + HUD, sound (AudioOut → SDL3), 60 fps warm, 11-min soak OK.

## (resolved part) washed-out picture (user screenshot)

Cause found: the game's final composite (fs `bc440c9e68b1a585`) computes `sqrt(scene*ui.a + ui)`, i.e. it expects the
DLSS output **linear**; the DLSS input we blit (1536x864 rgb10a2) is **sqrt-encoded** → encoded twice → washed.
Fix written, built, **not yet tested**:
- `runtime/shaders/kar_upscale.comp` (+ generated `runtime/kar_upscale_spv.h` via `tools/build_shaders.py`): FSR1
  EASU on the encoded input, then `rgb*rgb`, stored to the rgb10a2 output as a storage image.
- `runtime/vk_kar_upscale.c` `vk_kar_upscale_record` (declared in `vk.h`); called from `vk_exec_draw.c upscale_fill`
  instead of the blit; afterwards `B.pipe[1] = VK_NULL_HANDLE`. Env: `KAR_UPSCALE=blit`, `KAR_UPSCALE_DECODE=0`.
Next: run toRace with `TOTK_SHOTS=2700-6300:300`, compare with scratchpad `wash1` captures; if colours now right,
log as fix #28 in KIRBY_RECOMP_PLAN.md and update docs/kirby-running.md (upscale is now FSR1, not linear).
If the picture turns too dark, the input may not be sqrt-encoded everywhere: check composite ubo c3 per scene.

## Colour status after the EASU+decode fix (tested, boot47, scratchpad wash2)

Much better than before (no double sqrt), but still too bright/glowy vs official screenshots (deep blue sky,
shaded colours; game8.co gallery). Lead: the tonemap (fs `428c23c2f32a3b39`) reads `c3[42].xy` = (a,b) and computes
`clamp((sqrt(x)-b)/(a-b))`; in the dump the whole c3 block (0x300 B at e.g. 0x570268900) is all 1.0 → divide by zero,
that term is dead. No GPU pass writes it in the frame → CPU-written. Next: check whether the game updates it through
inline uniform updates (OP_UPDATE_UB / `upd_note` in vk_exec_draw.c) that the dump does not print, or whether its
source (a param file) failed to load; also check the auto-exposure / bloom strength. German is now the default
language (`ov_system.c ov_language`, km/h); in-game language setting is on ROADMAP.

User-reported (screenshot, Air Ride mode menu, German UI works): the big left preview card shows only the
placeholder mode icon instead of a preview image/video → likely movie-to-texture or render-to-texture missing in menus.

## Done this evening (uncommitted, also in the fix log)

- Upscale fallback (`vk_exec_draw.c upscale_fill`, `vk_exec.c` rt_newest / drawn-set): fills a sampled colour image
  not drawn in the last 8 frames from the newest same-format/aspect render target **of the current frame**;
  `vk_exec_rt_forget` (called from `vk_memory.c vk_defer_image`) drops retired images.
- Compute dispatches **wait** for their async pipeline (`compute_pipe`; `KAR_CS_SKIP_PENDING=1` = old skipping):
  skipping lost the GPU device at boot.
- `KAR_DISPLAY_RES` (default 2560x1440), hid `GetPlayerLedPattern`, DLSS proc stand-ins (`nvn_api_dlss.c`; the game
  never calls them), NVN minor 30 for kar*, device props >0x5F answer 0 (0x95→1), `KAR_CS_SPV_MAX` 4 MiB.
- Renamed exe `kar.exe`, title "Kirby Air Riders Recomp", icon `runtime/kar.ico` (`tools/make_kar_icon.py`).

## Button glyphs (done, verified boot51)

Game asks for built-in font 5 (Nintendo extension font, button symbols in the PUA). `ov_system.c builtin_font_path`
loads `fonts/<t>.ttf` first. `tools/make_kar_buttonfont.py` (needs fontTools; scratch venv `scratchpad\fontenv`)
writes `build/kar458752/fonts/5.ttf`: original ring+letter glyphs for E0E0.. (A B X Y L R ZL ZR SL SR, d-pad
E0EA-ED, +/- E0EF/F0, same at E0A0..), sticks E101/E102 (unconfirmed); every other code shows its hex (probe look),
so a new screenshot names any code still missing. E0E1 = B confirmed in the "Turbo-Aufladen" tip.
Capture tip: `TOTK_SHOTS_FULL=1` for full-size 2560x1440 shots. Font is data only: regenerate, no rebuild.
Original Switch symbols: `builtin_font_path` now tries `fonts/<dump name>.ttf` first (FontStandard,
FontChineseSimplified, FontExtendedChineseSimplified, FontChineseTraditional, FontKorean, FontNintendoExtended; the
names console font dumpers such as nx-ttf-dumper write), so the user's own dumped fonts win over 5.ttf / segoeui.
We never ship or decrypt them. Not tested with a real dump (none here).

mph → km/h: solved. The unit was stored in the old save created under en-US. The user said the save is irrelevant;
the old save is moved aside to `build/profile-kar458752/save.old-en` (+ `save-backup.old-en`). The fresh German save
shows km/h (boot53). Note: with the fresh save, `toRace.txt` now lands in the driving school (Fahrschule, lesson 1)
instead of a race; that is fine for HUD tests but needs a new script for real races.

## 2026-11-09 late: rainbow fixed, memory cut, cave glow open

- **Rainbow picture (whole runs psychedelic, user screenshot)**: `upscale_fill` filled *every* never-drawn sampled
  image of a matching format with the newest render target of the same aspect (log showed a 1024x1024 target copied
  into a game texture every frame). Now only DLSS-like outputs (rgb10a2, 16:9, >= 1280 wide; `KAR_UPSCALE_ANY=1`
  old behaviour). Also storage-image binds now count as render targets (`vk_exec.c vk_exec_rt_note_storage`), and
  each source change is logged ("upscale source ..."). Verified boot56: one stable source, clean race.
- **Memory**: boot stage-library prebuild off for kar* unless `TOTK_LIB_PREBUILD` is set (`vk_pipeline.c lb_start`).
  Race: private 22 GB (was 29), working set 11 GB (was 13-16). Same fps.
- `ov_gfx.c ov_hdr_status` fills all outputs (format 0, 100 nits, `KAR_DISPLAY_NITS`); game passed 0s before, so
  no visible change. `ov_hdr_lum` logs luminance metadata (never called so far).
- Test profile with the old (English, mph) save for race scripts: `TOTK_PROFILE_DIR=build\profile-kar-test`
  (toRace.txt reaches a race there; the user's fresh German save goes to the driving school).
- **Cave glow (open, user asked)**: Kirby blown out white in the dark cave (race ~frame 3840). Dump in scratchpad
  `cavedump` (frame 3900, cave). Findings: HDR scene before post (pass 148) has Kirby up to 73-126 (100-nit units)
  while the cave is ~0.01; bloom chain 149-159; 160 adds bloom (fs f0a46cda, c4[40]=13.7, same ~14.7 in dumpO so not
  an exposure-at-max sign); 163 = fs 428c (PQ-encoded 3D LUTs 1632/1664 from cs a0f4, two LUTs blended by fog
  distance c3[42]), output still up to ~10 (=1000 nits); 169 = fs 184cb sRGB-encodes and clips >1 (its c17 is
  unbound but only holds texel offsets + dither). So highlights >1 clip to white. Leads: (a) LUT built for a HDR peak
  (cs a0f4 c3 = [0](0,0,1,2) [1](1,1.3,1,0.5) [2](1.28)); check what the console LUT outputs for SDR; (b) Kirby's
  lighting (pass 139→140 doubles to 73) too strong because cave lights / emissives are missing (35 bindless "Direct"
  shaders sample nothing; null descriptors) so auto-exposure or the light setup differs; (c) compare pass 133
  (768x432 rgba16f, G up to 145).

## 2026-11-09 night (2): Direct pool arrays done; rainbow runs NOT fixed (cause narrowed)

User plan (German): `E:\kirby\PLAN.md`. User wants the console's behaviour reproduced, not guesses.

- **Direct pool arrays (41 fs)**: `gfx_shader.c pool_patch` (after reflect_spirv): finds the c2 handle words the shader
  reads (constant access chains into its c2 UBO) and rewrites `UMin(id,31)` feeding the pool arrays into
  `(id ^ (id>>5)) & 31` (new ids past the bound). `add_res` marks them `GfxShaderRes.pool_arr` (+ masks pool_comb /
  pool_sep / pool_samp); sampler arrays (type 0) are now `VK_DESCRIPTOR_TYPE_SAMPLER` (`vk_pipeline.c
  descriptor_type`); `vk_exec_draw.c pool_fill` puts each candidate's texture/sampler at its element and logs clashes
  (none with the xor fold). Verified boot60: no clashes, no errors.
- **Rainbow runs**: random per run (boot57 clean, boot58 bad, same exe; ~half the runs), then the whole run. NOT the
  upscale (one stable source every run), NOT the LUTs (identical in good C / bad D dumps: scratchpad `rbdumpC`,
  `rbdumpD`, frame 3600), NOT stale CPU textures: new `KAR_TEX_VERIFY=1` (`vk_texture.c verify_stale`, hashes storage on
  every clean write-watch answer) reported 0 stale textures in a bad run (boot64). Pool memory is zero-filled
  (TOTK_FIX_ZERO=1). The divergence is already in the first lit-scene geometry passes (C pass 155: max 19.75; D pass
  181: 1408 everywhere): forward-lit draws. Next: dump with `TOTK_DUMP_UBO=1` in a good and a bad run and diff the
  uniform/storage buffers those draws read (light params, exposure, SH probes); look for a NaN/Inf or a
  GPU-written buffer read before its first write (temporal feedback keeps it for the run). Tools: scratchpad
  (session 295ee153) `align.py` (match passes of two dumps by shader hash), `passdiff.py`.
- **Rainbow narrowed further (late)**: `KAR_ARR_UBO=0` (new diagnostic in `vk_exec_draw.c bind_resources`: material
  arrays from c5..c8 get the dummy texture) gave 2/2 clean runs (scratchpad `arrGH.png`; in one even the cave glow
  was gone), against ~half bad otherwise. So the rainbow comes from the **material texture arrays (fix 18)**.
  `ubo_word`/`ubo_ptr` now read the submit-time UBO copy (`vk_usnap.c vk_usnap_read`, as the shaders see it); the
  counters it logs ("material handle reads ... snapshot N, live 0") show every read already came from that copy, and
  2/3 runs were still bad (`snapIJK.png`), so the handle bytes are as the console GPU sees them. Replay is
  synchronous (TOTK_REPLAY_ASYNC=0). Base offset of every arr_ubo array is 0, stride 8 (matches Ryujinx
  TextureHandleSizeInBytes). Remaining suspects: (1) handle → texture resolution of those ids (texture pool entry,
  type/view of an id the material names), (2) the shader variant/answers for arr_ubo slots, (3) element index > 31.
  **Blind spot**: `vk_dump.c` prints arr_ubo elements as "slot -1: no handle (dummy)"; first extend the dump to print
  each element's handle and resolved texture (format, size, mirrored), then compare a clean and a bad dump.
- **ROOT CAUSE FOUND (validation layer, boot72)**: `VUID-vkCmdDraw*-None-06479`: the material arrays are declared
  twice over the same handle table, 2D and 2D-shadow (`fp_t2ds_cb196613_...`, binding 40), so RGBA8 colour textures sit
  under a compare sampler. Vulkan leaves that undefined; NVIDIA returns run-dependent values → rainbow in ~60% of
  runs. Maxwell compares the red channel of any format. **Fix written and built (kar.exe 2026-11-09 22:25), NOT yet
  verified**: `vk_exec_draw.c shadow_dummy` (1x1 D32 at 1.0, views 2D/2DArray/cube/cubeArray, cleared via
  vk_upload_begin) replaces any colour image or empty slot in a shadow declaration (`tex_info_full`). Next: run with
  `TOTK_VK_VALIDATION=1` (06479 should be gone), then 4-5 normal runs (`toRace.txt`, test profile) for rainbow; if
  some materials now look unshadowed/wrong, use a depth twin of the red channel instead (Maxwell behaviour). Then
  log as fix #28+ in KIRBY_RECOMP_PLAN.md, and decide whether to keep diagnostics `KAR_ARR_UBO`, `KAR_TEX_VERIFY`,
  the ubo_word counters and the dump's `tex arr c5[k]` lines (dump `TOTK_DUMP_TEX=1x1` shows 1x1 param textures).
- Side note: cs reads a 3D noise texture (`lut23053`, 32^3 rgba16f, uploaded) declared as 2DArray.
- Memory now: race peak private ~24 GB, working set ~12 GB (boot58). Lazy heap commit judged not worth it (commit
  charge only, no RAM).
- fps in a cold first race: mean 45-48, 1% low 7.5 (pipeline builds; prebuild off) — see PLAN.md C.

## 2026-11-10: no-game work (built, not run; user was gaming)

User's master list: `E:\kirby\RELEASE_PLAN.md` (German, OHNE = no game needed / SPIEL = needs a run). Done today
without running: review fix in `pool_patch` (output buffer sized exactly); `KAR_DIAG` gates the material-handle
counters and the draw census (census now logs only when draws were skipped); dump resolves split-handle samplers like
the runtime (the old "separate sampler (not replayed)" text was misleading, not a bug); log file `logs\kar.log`;
FSR notice covers `kar_upscale.comp`; `docs/kirby-running.md` refreshed (works / known gaps / log path / switches);
`check_content.py` clean. New experiment `KAR_C0_TEX=1` (`vk_exec_draw.c c0_tex_handle`, `GfxShaderRes.c0_byte`):
82 NVN 2 computes whose texture handle word sits in c0 outside the tables (mostly c0+0xE0) take that word when it names
a pool texture; logs the first 16 values. Findings: no tessellation draw is ever skipped (59 LEPC failures harmless
so far); every non-Direct texture declaration is combined; 889 pixel shaders have shadow-declared material arrays.
**Next game session, in order**: (1) `TOTK_VK_VALIDATION=1` race run, 06479 must be gone; (2) 4-5 races for the
rainbow (fix 34); (3) cave glow; (4) one run with `KAR_C0_TEX=1`, read its log lines; (5) remaining VUIDs.

## 2026-11-10 (2): test block results

- 5-race blocks with `tools/kar_race_test.ps1` (+ `kar_shot_check.py`, run median brightness > 0.62 = rainbow):
  shadow_dummy in: 3/5 bad; with KAR_C0_TEX=1: 2/5 bad. So fix 34 removed VUID 06479 but NOT the rainbow.
  Index overflow ruled out (material array index is clamped UMin 31). Still open; next idea: per-draw A/B, e.g.
  disable material arrays per shader hash to find the draws, or dump a bad frame with the new `tex arr` lines and
  compare element-by-element with a clean one at the same track position.
- KAR_C0_TEX: the c0 words (0x50, 0x98) are 0 at runtime: those slots are really empty; experiment stays off.
- RCAS for the DLSS stand-in works but is now OFF unless TOTK_SHARPNESS is set explicitly (user saw a harsher
  picture with the table default 80).
- One run (block1 run3) showed a flat green checker field instead of the course; unexplained (no FATAL, C0 had no
  effect). Watch for it.
- User wants a Video settings tab: RELEASE_PLAN.md section 14.

## 2026-11-10 (3): compared with Eden (refs/eglantine-gh, sparse: read files with `git show HEAD:<path>`)

- Material arrays: Eden reads up to 1024 descriptors at the shader's stride (texture_pass.cpp
  DynamicDescriptorCount, vk_graphics_pipeline.cpp read_handle). Implemented as `gfx_shader.c arr_patch`
  (KAR_ARR_N, widens the arrays and the UMin clamp); a 128-element run logged no handle past element 31, rainbow
  unchanged (2/5) -> default back to 32 (no-op). Code kept.
- Direct (phi) handles: Eden resolves them at translation (`ResolveConditionalPhi`: the sample inside the same
  condition as the handle choice); our runtime equivalent (pool_patch/pool_fill by id) matched with no clashes.
- Online: nothing specific to Kirby Air Riders on Switch 2 emulators (results only for the GameCube game).
- User: "the rainbow was almost gone earlier", and the picture now looks worse; the RCAS default 80 was one cause
  (now off). User asked NOT to run 5-race blocks repeatedly: batch fixes, test rarely.

## 2026-11-10 (4): Direct pool fill off by default

Rainbow rate per build from all logged runs: after the upscale fix, before pool_fill 1/4 bad; with pool_fill 13/21;
with shadow_dummy too 8/16. The user also saw it as better "earlier". So `vk_exec_draw.c pool_fill` now binds the
dummy texture and default sampler unless `KAR_POOL_ARR=1` (the SPIR-V rewrite stays; harmless with dummies). Built,
not tested. Next: confirm with the user's own play or one block; if confirmed, find why real textures in those 41
shaders go wrong (view type of the resolved image vs the 2D declaration is a candidate).

## 2026-11-10 (5): technical debugging of the rainbow (user asked for real debugging, not play-and-look)

- Pool fill off: still 2/4 bad -> pool_fill not the cause (stays off by default anyway; KAR_POOL_ARR=1 on).
- `TOTK_DUMP_TEX=gpu` (vk_dump.c): reads back every GPU-written 2D texture sampled in the dump frame; images.txt lines
  carry `hash <fnv64>` of the raw texels. `tools/kar_dump_diff.py --good A B --bad C` lists contents stable in all
  good runs but missing in the bad one. Dumps in `E:\kirby\gpudump` (good1, good2, bad3). Result: weak, frames differ
  between runs (only a 4x512 rgba8 ramp and two 128x128 textures, all position-dependent).
- In-run bisection instead: `KAR_ARR_SCHED=<frame>:<0|cbuf|all>,...` (vk_exec_draw.c arr_sched_allows) switches the
  material arrays per constant buffer from a frame on; a bad run keeps its fault, so the shot right after a step shows
  causation. Runner: scratchpad (session 295ee153) `schedruns.ps1` -> `E:\kirbyisect
un<k>` (shots 3100-5200,
  schedule 3500 none, 3800 c5, 4100 c6, 4400 c7, 4700 c8, 5000 all).

## 2026-11-10 (6): RAINBOW SOURCE PINNED: material arrays read from c8

In-run bisection (`E:\kirby\bisect\run2_sched.png`): rainbow with all arrays; gone with none, only c5, only c6, only
c7; back at once with only c8. The 74 c8 shaders (all ps) declare the same c8 handle table twice: a 2D array
(cbuf_slot 8, combined handle in c8) and a cube array (cbuf_slot 0x30008, sampler from c2 slot 0, handle 0x10568000);
c8 is NOT among their declared uniform buffers. Type mismatches already get the dummy (`get_view` returns NULL).
Next: dump a rainbow frame (`E:\kirby\c8dump`, dump frame 3350, TOTK_DUMP_UBO=1) and read the `tex arr c8[k]` lines
(raw handles, resolved textures, which UBO is bound at c8 and from where).

## 2026-11-10 (7): CORRECTION: c8 is NOT the cause

`KAR_DIAG_C8=1` (logs every c8 array element with a handle) logged nothing in 1 clean + 2 rainbow runs
(`E:\kirby\c8log`): c8 arrays are always empty. The (6) bisection was confounded by track position (rainbow only shows
on bright areas; the "none" step fell into the dark cave, the final "all" step was clean). Rule for further tests:
alternate the switch quickly (every 50 frames on/off, shots every 25 frames) so position is constant; a causal
switch makes the rainbow flicker in step. Dump MAX_ENTS is 1024 now (48 cut big passes off).

## 2026-11-10 (8): material arrays ruled out (alternating test)

`KAR_ARR_SCHED` alternating none/all every 50 frames from 3200, shots mid-step (`E:\kirby\alt\run2_alt.png`): in the
rainbow run the rainbow is there with arrays OFF as much as ON (brightness 0.82 off / 0.69 on at 3625/3675). So no
per-draw texture binding of the material arrays causes it; the earlier KAR_ARR_UBO=0 2/2 was chance. The fault is a
state that persists for the run. Next technique (proposed): `KAR_CLEAR_AT=<frame>`: clear every GPU-owned image once
in a bad run. Rainbow gone for good -> a stored, corrupted texture (then bisect which by clearing subsets); comes back
-> something regenerates it each time (then bisect the writers).

## 2026-11-10 (9): clear test: the fault is NOT a stored GPU image

`KAR_CLEAR_AT=<frame>[:<VkFormat>]` (vk_texture.c vk_texture_clear_gpu_at, triggered from vk_exec_draw.c
prepare_draw) cleared 107 GPU-written/non-mirrored images at frame 3500 in a rainbow run (`E:\kirby\cleartest\run1_clear.png`):
the clear worked (speedometer went black) but the rainbow was back in the very next shot and stayed. So it is
produced every frame by something whose own state is fixed per run. Remaining candidates: (1) pipeline/shader
variants chosen by timing at first build (async pipelines, translation answers): test with synchronous pipeline
builds (`TOTK_PIPE_WAIT`/`TOTK_PIPE_SKIP`/`TOTK_PIPE_PREBUILD` settings) or log pipeline key/variant per draw and
diff a good and a bad run; (2) GPU-only buffers written at load (not covered by the image clear).

## 2026-11-10 (10): pipelines ruled out; exposure/effect parameters differ

- 4 runs with TOTK_PIPE_WAIT=draw, PIPE_SKIP=strict, GPL=0 and every memo/skip off (SHADER/PIPE/TEX_MEMO, BIND_SKIP,
  PUSH_SKIP, DESC_TEMPLATE): 3/4 rainbow (`E:\kirby\pipetest`). Pipelines and memos are not the cause.
- Rainbow runs are much brighter (run median 0.77-0.9 vs 0.3-0.6): exposure suspect.
- UBO dump compare, bloom/tonemap pass (fs f0a46cda), `E:\kirby\c8dump` run1 clean vs run2 bad, frame 3350:
  c4 (0x5df200c00) [0].x 0.657 vs 0.133, [6].z 40.4 vs 45.5; c5 at the SAME address 0x5df202800 holds effect
  parameters in the clean run ([6] 1 1 1 50, [7] 60 0.25 1 0) but a different-looking struct in the bad one
  ([1] 0 1/540, [2] 0 0 460 540). Caveat: the dump reads memory after the draw (may be the next frame's data).
  Next: log those words at draw time from the submit-time copy (vk_usnap_read) for fs f0a46cda / 428c in good and
  bad runs; find who writes 0x5df202800 (CPU: watch the guest writes; GPU: copies/compute into that range) and
  whether the game computes exposure from a GPU readback (ReadTexels / query / buffer read) that we answer wrongly
  at the start of a run.

## 2026-11-10 (11): exposure lead was position again

`KAR_DIAG_UBO=<fs hash>:<cbufs>` (vk_exec_draw.c diag_ubo; logs vec4 0..7 of those UBOs every 30 frames from the
submit-time copy; hash = gfx_shader_spirv_fnv = vk_dump's fs name). For fs f0a46cda (bloom combine) c4[0].x steps
between 0.657 / 0.133 / 0.085 / 0.047 inside one run by track section: an area setting, not the fault (3 runs, all
rainbow, `E:\kirby\ubolog`). Also ruled out: focus changes and new translation answers (no correlation over 13 runs).
Lesson: compare good and bad runs only at the same track position (or switch things inside one run).

## 2026-11-10 (12): user's own save also gets the rainbow; next: RenderDoc pixel history

The user said the rainbow was "99% gone" earlier. 3 runs on a copy of the user's own save
(`build\profile-kar-usercopy`, the race is reached there too): 2/3 rainbow (`E:\kirby\usertest\grid.png`). The fault
is decided per start (about 50-60% bad over all builds); a few clean starts in a row look like a fix. The user
allows deleting saves (not needed). Proposed next: RenderDoc (not installed; download from renderdoc.org needs the
user's OK) to capture a rainbow frame and use pixel history on a wrong pixel instead of more hypotheses.

## 2026-11-10 (13): RenderDoc capture fails; stopped here

RenderDoc 1.47 portable in `E:\kirby\tools\renderdoc\RenderDoc_1.47_64` (user approved the 97 MB download). The
runtime's hook `rdoc.c` (TOTK_RDOC=<frame>, TOTK_RDOC_DIR, TOTK_RDOC_DLL) plus VK_ADD_LAYER_PATH=<that folder> and
ENABLE_VULKAN_RENDERDOC_CAPTURE=1: "capture of frame 3450 started" then "capture failed", twice. Not investigated
further (user frustrated). Likely reason: the runtime imports guest memory into Vulkan (host-pointer imports, mirror
pools), which RenderDoc does not capture; a capture mode would need a memory mode without imports. Runner script:
scratchpad (session 295ee153) `rdocruns.ps1`. The rainbow stays open; see entries (5)-(12) for everything ruled out.

## Measured, not yet acted on

Memory: private ~29 GB / RAM 13-16 GB with the boot compute prebuild; `TOTK_LIB_PREBUILD=0` → ~22 GB / 11.5 GB,
same fps. Plan: default prebuild off for kar* (or keep fewer), then lazy heap commit (`memory.c mem_set_heap_size`).

## Other open items

"About Replays" notice after a race (OK greyed; blind DOWN/RSTICK scripting changed menus instead, use
`DKB_LIVE_INPUT` to drive step by step), other modes untested, 35 bindless fragment shaders, 40 c0-handle computes.

## 2026-11-10 (14): blit dither bias (Lichii PR #56) — correct, but NOT the rainbow
- Web lead: zortos293/Lichii PR #56 ("white world"): Kirby's two SMAA neighbourhood-blend fs add c17 words 0x10-0x18
  AS INTEGERS to the output bits (dither, zero on console); Eglantine's BlitConstantsPass puts texel size / 1.0 there.
  Our shader-cache confirms: only those 2 ps read c17[1].xyz, as `out = intBitsToFloat(bits + c17[1])`.
- Fix in vk_exec_draw.c ubo_info (slot 17): kar zeroes words 4..7; KAR_BLIT_DITHER=1 restores the old block.
- Test E:\kirby\dithertest (4 runs): still 2/4 RAINBOW (run1, run4). Rainbow = whole triangles in random colours
  (geometry/material level), not a per-pixel post effect. Kept the fix (matches hardware).

## 2026-11-10 (15): more exclusions (all measured)
- Boot-to-race shots (`E:\kirby\earlytest`): menus incl. the 3D Kirby preview are always clean; the rainbow starts in
  the race. Bad scene = per-channel clipping of far too high HDR values (first forward-lit pass, bad3 pass 149: max
  124/16/160 vs 8.75 clean); large smooth blobs, i.e. an ambient/indirect-like term, plus particles.
- Compute dispatches never skip (vk_exec_draw.c compute_pipe waits). Logs of good/bad runs identical.
- `KAR_REUPLOAD_AT=<frame>` (vk_texture.c vk_texture_reupload_at): re-uploading all ~3900 CPU textures (incl. the
  BC6H IBL cube) mid-race: rainbow stays (`E:\kirby\reuptest`). With KAR_CLEAR_AT: no image (CPU or GPU) holds it.
- `KAR_DIAG_SSBO=<fs>` (content hashes) and `KAR_DIAG_UBO_N` (more vec4s): buffers change every frame / alternate
  between double buffers, so cross-run compares are noise.
- `KAR_ALT_SSBO=<frame>[:<slot>]` (vk_exec_draw.c ssbo_info): zeroing fragment storage slot 4 in alternating 50-frame
  blocks: rainbow in on AND off blocks (`E:\kirby\alt4_run1.png`). Slot 5 runs were both clean (no verdict).
  The slot sweep hint (4/5) was position again.
- Next (proposed): own "pixel history": read back the colour target after every draw of the first lit pass in a bad
  run and log the first draw that writes values > ~20; then inspect exactly that draw's inputs.

## 2026-11-10 (16): own pixel history (TOTK_DUMP_DRAWS) — IN PROGRESS
- New: `TOTK_DUMP_DRAWS=<VkFormat>` (vk_dump.c vk_dump_split_draw, hooked in vk_exec_draw.c note_work): in the dump
  frame every draw into a colour target 0 of that format ends the pass -> one "pass" entry + image per draw; also raw
  `.bin` texels. MAX_PASSES 2000. Runner: scratchpad `drawhist.ps1` (TOTK_DUMP=3600, DRAWS=122 = r11g11b10f).
- Result 1 (`E:\kirby\drawhist` bad2/good1, PNG only): in the bad run nearly EVERY forward shader writes too-high
  values (max jumps to 220 at the first c2df457e draw, then all shaders) -> a COMMON input of all lit shaders
  (c4 scene block, c3 camera, shadow map d16 0x5d0a00000, BC6H IBL cube, light SSBOs 0-2), not one material.
- Running: `E:\kirby\drawhist2` with raw .bin; analyse with /tmp/dh3.py-style r11g11b10 decode (per draw: changed
  pixels, median/p99/max) good vs bad, same shader -> then zero/replace each common input in alternation.
- RESULT (drawhist2, raw r11g11b10 per draw, 3 bad + 1 good): NOT all shaders. Broken in bad runs (values up to
  220-3170 vs ~7-21 in good): fs c2df457e, 4a83a643, 0a9439d2, 9e8f8662, a6fa3a11, e5ea52fb. Normal in bad runs:
  7fd5b3ac, f6fcbb15, b3441c0b, 0432bb88, 9eaf75f5. Common to ALL broken ones: fs tex slot 2 declared as BOTH 2D and
  2DShadow (the d16 1024x1024 11-level shadow map at 0x5d0a00000); none of the normal ones has slot 2 2DShadow.
  NEXT: look at how those shaders use slot 2 as plain 2D (raw depth read: PCSS blocker search / VSM?) vs compare;
  check our compare/shadow handling (vk_exec_draw.c shadow_dummy, tex_info_full; sampler compare state; d16 view
  format; LOD/mips of the shadow map) and alternate-test: bind a 1.0 depth dummy only for the plain-2D decl of slot 2.
- User pointed to two related projects to read (not yet looked at): https://github.com/visibou/Switch2Recomp and
  https://github.com/visibou/mkwrecomp — check how they handle shadow-map (2D + 2DShadow on one depth texture),
  NVN 2 depth/compare samplers and d16 views.

## 2026-11-10 (17): more exclusions after the pixel history
- `KAR_ALT_SHADOW=<frame>:<p|c>` (vk_exec_draw.c tex_info_full): the depth shadow map replaced by 1.0 for plain (p)
  or compare (c) declarations in alternating 50-frame blocks: rainbow in both kinds of blocks (`E:\kirby\altshadow*`).
  Run with TOTK_TEX_MEMO=0 TOTK_BIND_SKIP=0 so the switch takes effect. Shadow map ruled out.
- Switch2Recomp (visibou, runtime/nvn/vk/vk_tex.c init_fill) documents a "rainbow sky" from uninitialised GPU images
  of compressed formats in CPU_NO_ACCESS pools. Ported as vk_texture.c init_fill (TOTK_TEX_INIT_FILL, default on for
  kar*): it found ZERO such textures here (ours mirrors them); 4/4 runs still rainbow. Kept (harmless), candidate log
  still in it (remove later).
- Diagnostic log "sampled before any GPU write" (vk_texture.c make_fresh_snap): ~10 non-mirrored textures per run;
  only difference bad/good was a 4096x36 R11G11B10F at frame ~986 (menu), not used in the race dumps.
- `KAR_ALT_GPUBC=<frame>`: every non-mirrored BC texture (GPU-filled BC6H probe arrays) replaced by the dummy in
  alternating blocks: rainbow in both (`E:\kirby\altgpubc_run1.png`). Ruled out.
- Still open. Ruled out overall: CPU textures, GPU images (cleared), GPU BC images, shadow map, SSBO slot 4, material
  arrays, pipelines, memos, SMAA dither, compute skips. Broken shaders (pixel history): c2df457e, 4a83a643, 0a9439d2,
  9e8f8662, a6fa3a11, e5ea52fb; normal: 7fd5b3ac, f6fcbb15, b3441c0b, 0432bb88. Next idea: diff the SPIR-V of a broken
  vs a normal lighting shader for features only the broken ones use (light-loop SSBO reads, cube IBL, specific cbuf
  ranges), then alternate exactly that input; or compare the vertex stage (both share vs).

## 2026-11-10 (18): CPU-side angle and RenderDoc root cause
- Whole-run single core (`kar_race_test.ps1 -Affinity 1`, new param): deterministic crash at input frame ~900 (menu
  video) in all 4 runs: f_7002ecfb00 polls *(obj+0x20)+0x20128 after another thread freed it (guest use-after-free
  exposed by scheduling). With `-AffinityDelay 50` (single core only from ~menu on): 4/4 rainbow -> not a thread race
  during race load (boot-time not tested).
- c4/c3 of fs c2df457e at frame 3600 (submit copies, TOTK_DUMP_UBO now 180 vec4 from vk_usnap_read): no NaN/huge
  values, differences position-dependent (`E:\kirby\ubodump`).
- RenderDoc root cause: the portable layer is explicit -> needs VK_INSTANCE_LAYERS=VK_LAYER_RENDERDOC_Capture (the
  earlier "0 device frame capturers"). With it, RenderDoc hides VK_EXT_external_memory_host and our runtime finds no
  usable device. RenderDoc needs a memory mode without host-pointer imports (vk_memory.c) - substantial work.

## 2026-11-10 (19): FMZ for every stage does not fix it
- Fragment shader c2df457e ends with out += (1/gl_FragCoord.w) * vs-location-5 (per-vertex term from the vertex
  shader: 1/(view depth) based); suspicion only. SPIR-V override test (TOTK_SPV_OVERRIDE, E:\kirby\spvov\c2_noadd.spv)
  did NOT apply: the override hashes the SPIR-V after spirv_fixup/FMZ (vk_pipeline.c ~1265), not vk_dump's name.
  Use TOTK_DUMP_SPV to get the right name first.
- TOTK_FMZ=all (Maxwell 0*x=0 rule in every stage, default vertex only): runs 3/4 still RAINBOW
  (`E:\kirby\fmzall\fmzall_grid.png`); runs 1/2 were the slow first build (no shots). Not the cause.

## 2026-11-10 (20): fresh look (Fable): buffer transport = MIRROR mode, never tested
- vs bc5fc07f (of fs c2df457e): the additive term is height fog: colour c4[19].xyz, amount scaled by per-object SSBO
  s0 (+376), per-object colour s0 (+144..152) -> location 4; divided by exposure c4[0].x. Bad draws are per OBJECT
  (same shader ok for some draws, 1540-3170 for others) -> per-object block suspect, not textures.
- Log: `mem: mode mirror (auto, discrete gpu)`: 12-15 pools get device-local mirrors synced by write-watch, with
  freeze/thaw (TOTK_FREEZE_IDLE) and ~48 MiB/frame write-back. TOTK_MEMORY=import|mirror|verify exists
  (vk_memory.c mirror_mode). NEXT: 4 runs TOTK_MEMORY=import; then verify. Protocol: E:\kirby\RAINBOW_PROTOCOL.md
  (a session cron checks every 30 min).
- TOTK_SPV_OVERRIDE now also matches the shader's own hash and is applied BEFORE spirv_fixup/FMZ (the first try
  after the fixups gave inf). Tests dh_noadd / dh_orig running.
- RESULT (20): TOTK_MEMORY=import: 3/4 still RAINBOW (`E:\kirby\memimport\memimport_grid.png`). Mirror ruled out;
  the GPU reads guest memory directly and it is still wrong -> the guest-side data (or the address the shader
  computes) is wrong. Running: `E:\kirby\objblock` = pixel history + per-draw "vs obj block" line (vk_dump.c: floats
  at +0, +144, +376 of vertex SSBO slot 2) to correlate bad draws with their block's values.

## 2026-11-10 (21): objblock pixel history: big early draws are the clean comparison
- `E:\kirby\objblock` (bad2/good1, frame 3600): per-object block values of c2df457e are sane and equal (colour 1 1 1,
  fog scale 1) in both; its draws were fine in this bad frame. Per-object block NOT the cause.
- Broken in bad2 vs good1, same shaders: fs f7746210 (first big draw, 430-630k px: terrain/sky; vs 0433ea12)
  p99 1120 vs 2.7; 29c24f17 1120 vs 0.8; 9d1dca6a 1120 vs 1.4; ad968ce1 179 vs 1.1. 1120 = the 150 clamp / exposure
  (c4[0].x 0.133): pre-exposure radiance hits the clamp, i.e. incoming light is >500x too high.
- f7746210 inputs: vs texelFetch from a 64x30 R11G11B10F CPU texture (index = id % W, id / W); fs material array c6 with
  1x1 float textures (rgba32f, rg32f, rgba16f: mirrored AND gpu_owned, "written-since 1" -> CPU writes after the GPU
  took ownership are never uploaded, vk_texture.c make_fresh returns early for gpu_owned); BC6H cube; d16 shadow.
- Side note: good1 had every guest address at 0x10xxxxxxxx instead of 0x5xxxxxxxx (different heap base that run);
  other good runs have the low base, so the base is not the predictor.
- Running: `E:\kirby\texcmp` = dumps with TOTK_DUMP_TEX='64x30,4096x36,1x1' (hash/min/max of those textures) good vs bad.

## 2026-11-10 (22): STRONG LEAD: the 64x30 R11G11B10F vertex-data texture holds heap garbage in rainbow runs
- `E:\kirby\texcmp` (TOTK_DUMP_TEX=64x30,4096x36,1x1; 2 good, 1 bad): every 1x1 texture identical. The 64x30
  R11G11B10F texture (vs tex slot 0 of vs 0433ea12 / fs f7746210, texelFetch by index; game heap address, mirrored,
  gpu_owned 0, uploads 1) is ALL ZERO in both good runs (same hash 282e145144b17b25) and garbage in the bad run
  (max 65024 / inf, nan). Re-upload did not help earlier -> the guest storage itself holds the garbage.
- So: the game never writes it on the CPU (or we miss its writer: a skipped GPU clear/copy; clear_tex skips partial
  clears and logs only once). Zeros = fine, reused heap memory = rainbow; allocation order varies per start.
- Running: `E:\kirby\zerotex` 6 runs with KAR_ZERO_TEX=64x30! (vk_texture.c zero_tex_diag: logs nonzero bytes, pool
  and texture flags at first upload, zeroes the storage). 6/6 clean = cause confirmed; then find the proper writer
  (log all clears/copies targeting that texture; check what Horizon guarantees for that memory).
- RESULT (22): `E:\kirby\zerotex` 6 runs: 17 such 64x30 textures per run (pool flags 0x24 = CPU_CACHED|GPU_CACHED,
  tex flags 0x4 = IMAGE: storage images, meant to be written by compute). Runs with all-zero storage (1,2,4,6) = ok;
  runs where 2-3 of them held garbage (3,5) = RAINBOW although those were zeroed. PERFECT 6/6 correlation, but zeroing
  the 64x30 ones alone does not cure -> other never-written IMAGE textures in recycled heap memory hold garbage too
  (e.g. the 4096x36 R11G11B10F seen in bad runs only). Garbage memory = marker of a recycled heap block.
- Running: `E:\kirby\zeroimg` 6 runs KAR_ZERO_TEX=image! (zero every mirrored IMAGE-flag texture with nonzero storage at
  first upload, each logged). Clean 6/6 -> then decide the real rule (IMAGE textures are GPU-written on hardware:
  find the compute writer we miss, or treat IMAGE textures as GPU-owned/zero-initialised instead of CPU-mirrored).

## 2026-11-10 (23): ROOT CAUSE FOUND: skipped ClearTexturei(0) on float textures
- `E:\kirby\zeroimg`: KAR_ZERO_TEX=image! (all nonzero mirrored IMAGE-flag textures zeroed at first upload): 6/6 clean
  (`zeroimg_grid.png`). Hundreds of R11G11B10F (nvn 0x3f) 64xN and 4096xN IMAGE textures per run held heap garbage.
- KAR_DIAG_CLEAR=1 (vk_exec_xfer.c clear_tex, vk_texture.c gpu_written / storage bind logs): the game clears every one
  of them: `clear_tex 64xN / 4096xN fmt 0x3f flags 0x4, full region, mask 0xf, uint 1, value 0 0 0 0`
  (ClearTexturei). clear_tex skipped them: "value kind (uint 1) does not match format" — and warn_once(W_CLEAR) had
  already been used by an unrelated partial clear, so the skip was never logged.
- FIX (vk_exec_xfer.c clear_tex): a kind mismatch is skipped only when the value is non-zero (all-zero bits are the
  same for float and integer); own warning id W_CLEAR_KIND. Running: `E:\kirby\clearfix` 8 runs, no diagnostics.
- Cleanup later: remove/keep-behind-env the diagnostics (KAR_ZERO_TEX, KAR_DIAG_CLEAR, "sampled before any GPU
  write" log, init_fill candidate log, KAR_ALT_*), update docs/kirby-changelog.md and KIRBY_RECOMP_PLAN.md.

## 2026-11-10 (24): RAINBOW FIXED (verified 8/8)
- `E:\kirby\clearfix`: 8 runs, no diagnostic switches, only the clear_tex fix: 8/8 ok (`clearfix_grid.png`, 40 shots).
  Before: 50-100 % of starts rainbow. Fix = vk_exec_xfer.c clear_tex (all-zero value clears regardless of
  integer/float kind). The session cron (RAINBOW_PROTOCOL) is deleted; protocol closed.
- Still open (not the rainbow): Kirby glows white in the dark cave (visible in clearfix shots f03900).
- TODO cleanup, not done yet: diagnostics added during the hunt are all env-gated but still in the code
  (KAR_ZERO_TEX, KAR_DIAG_CLEAR, KAR_ALT_SSBO/SHADOW/GPUBC, KAR_REUPLOAD_AT, KAR_DIAG_SSBO, TOTK_DUMP_DRAWS,
  "sampled before any GPU write" log (unconditional, 400 lines max), init_fill + its candidate log (default on for
  kar, found nothing), blit dither zero (kept: matches hardware)). Docs: add fix #37 to KIRBY_RECOMP_PLAN.md and
  docs/kirby-changelog.md. The user's own save was not re-tested with the fix.
- (24b) User save copy with the fix: 3/3 clean (`E:\kirby\usersave_fix`). KAR_POOL_ARR=1: 4/4 clean
  (`E:\kirby\poolarr_fix`) -> pool_fill is ON by default again (KAR_POOL_ARR=0 turns it off). Cleanup done: init_fill
  removed (found nothing), "sampled before any GPU write" log now behind KAR_DIAG_UNWRITTEN. Docs: fix #37 in
  KIRBY_RECOMP_PLAN.md, changelog, RELEASE_PLAN 1.1/1.2/1.4 done.

## 2026-11-10 (25): cave glow, first pixel history (frame 3900, `E:\kirby\cavehist`)
- In Kirby's screen region the body is drawn by fs 0432bb88 (vs c2cdafed): 6233 px, median 20, p99 55 (HDR, pre
  tonemap); vehicle f6fcbb15 ~7-14; cave background ~0.01-1. So Kirby's own lit colour is 20-50 while the cave is
  dark; with the cave's (dark-adapted) exposure he clips to white. Open question: is Kirby too bright (sun not
  shadowed in the cave: check his 2DShadow compare / shadow cascade) or is the exposure wrong (cave lights missing)?
  Needs a console reference screenshot of that spot. Next test: KAR_ALT_SHADOW-style switch with a 0.0 depth dummy
  (fully shadowed) for fs 0432bb88 only; if Kirby becomes normal, the shadow lookup returns "lit" in the cave.
- Menu: German menu language added (ui_text.c text_de, full translation; cfg MENU_LANG en|de|ru; auto from Windows
  "de"); the Screen tab is called "Video" in German.
- (25b) Menu verified in game (`E:\kirby\menutest3\pages`, TOTK_MENU_TEST=3300:40, TOTK_MENU_LANG=de): German texts,
  Kirby palette (runtime/ui/menu.cpp style_init, kar* only; accent = TabSelectedOverline, so DKB stays gold/green).
  TOTK_MENU_TEST_DIR must exist or the run dies (FATAL cannot write). Not yet checked: every Video option actually
  works for Kirby (render scale, upscaler, FPS limit, colour).

## 2026-11-10 (26): dull picture, first finding + plan section 15
- Colour chain at the end of a frame: fs 428c (two 3D LUTs, PQ-encoded, from cs a0f4) -> 98ea (SMAA edges) -> 184cb
  (exact sRGB encode into 1536x864 rgb10a2 = DLSS input) -> DLSS -> composite bc440c9e (DLSS out + UI rgba8srgb in
  linear light, pow(c, c3[2].y = 1) with a shoulder, then sqrt) -> 2560x1440 rgba8 UNORM -> present.
- Our DLSS stand-in decoded with x^2 although the input is exact sRGB: lifted mid-tones. Now the sRGB EOTF
  (runtime/shaders/kar_upscale.comp, kar_rcas.comp; KAR_UPSCALE_DECODE=sq restores the square, 0 none). Effect is
  modest (simulated on a shot: mean 117 -> 111, `E:\kirby\dull_sim.png`); built, not yet seen live in a run.
- Still to check for "dull": (a) the 3D LUTs are PQ-encoded = built for HDR: does the game think the display is HDR
  (our HLE answer for HDR capability / luminance)? (b) the final sqrt (gamma 2.0) into UNORM shown on an sRGB/2.2
  swapchain: what does the console's display path do with it? Needs a console screenshot of the same spot.
- RELEASE_PLAN: new section 15 "real in-game settings menu" (hook the game's own Options list), 1.10 dull picture,
  14.14/14.15 done.

## 2026-11-10 (27): console reference from YouTube (Shacknews 4K Floria Fields race, youtu.be/3enuICYiVwE)
- Measured in the browser (canvas, 1280x720 stream, middle band of the frame, luma 0-255): open-field frames mean
  ~193, p5 100-130, p95 230-245, saturation 0.37-0.42; cave frame (t=60) mean 114, Kirby plainly pink (NOT white):
  the cave glow is a real bug. `E:\kirby\console_cave_ref.jpg` (small).
- Ours (clearfix f03000/f04200, same track): mean 99-126, p5 41-72, p95 174-225, saturation 0.33-0.58.
  => our picture is darker and its highlights stop early (p95 ~175 vs ~235), i.e. "dull" = too little brightness /
  exposure at the top end, not too little colour. The sRGB-decode fix (26) barely touches highlights.
- Leads for (1.10): auto-exposure / pre-exposure value (c4[0].x in the lighting and bloom shaders) vs console; the PQ
  (HDR) LUTs from cs a0f4 (does the game think the display is HDR? then its SDR path differs); the composite's
  c3[2].y gamma (1 here). Try first: log c4[0].x over a race and compare an open field (console ~193 mean).

## 2026-11-10 (28): exposure / dull picture: tonemapping chain read, display nits tested
- fs 428c: scene (100-nit units) -> PQ encode (x0.01) -> 3D LUT -> PQ decode x100 -> later clipped at 1.0 (184cb).
  LUT built by cs a0f4 from c3: a display-aware tone curve ([0] 0 0 1 2, [1] 1 1.3 1 0.5, [2].x 1.28). c4[0].x
  exposure steps per track section (artist values, not auto-exposure). The 82 texture-less compute shaders are not
  involved (their slots are empty at runtime).
- Our vi QueryDisplayHdrStatus answers SDR, 100 nits (ov_gfx.c ov_hdr_status, KAR_DISPLAY_NITS overrides); paper
  white 203. Tested 200 and 300 nits (`E:\kirby\nits_compare.png`): no clear brightness gain, saturation drops a
  little with more nits; frames are at different track positions, so inconclusive. Default stays 100.
- Needed for a real verdict: a position-matched comparison. Idea: the pre-race flyover camera is scripted; take
  shots every 5 frames there and match them to the console video frame by image similarity, then compare luma.

## 2026-11-10 (29): position-matched console comparison (dull picture)
- Console reference: YouTube 3enuICYiVwE (Shacknews, Global Test Ride demo), measured in the browser canvas; data in
  `E:\kirby\console_ref\start_frames.txt` (t, mean, p5, p95, sat, 16x9 luma hex); matcher `console_ref\match.py`.
- 2D UI matches the console almost exactly (character select pink (252,161,159) vs ours (252,156,155); machine select
  (188,53,52)/(226,118,95) vs (189,53,52)/(226,119,95)) -> composite, sqrt encode and present are correct.
- 3D at the start line (console 35.0 s vs our f029xx, corr ~0.6): console mean 199 p5 150 p95 243 sat 0.38; ours mean
  ~134 p5 ~65 p95 ~228 sat ~0.45. Highlights similar, shadows/mid-tones far darker on ours; the console image is
  hazy/pastel. KAR_DISPLAY_NITS 100/200/400/1000 changes nothing of that (`E:\kirby\startnits*`): not the tone curve.
- Next suspects: something that lifts the whole 3D image on the console and is weak/missing here: bloom/haze/fog
  (bloom combine fs f0a46cda, c4[40]; height fog colour c4[19]; light shafts), sky/ambient light (probe SH, BC6H cube),
  or one of the 82 compute shaders with empty texture slots. Method: per-pass means at the start line in a dump
  (TOTK_DUMP=2915) and see which post pass should add ~+60 luma.

## 2026-11-11 (30): dull / too dark picture FIXED (DLSS stand-in decode) + NVN2 compute handle layout
- nvn2_lower.cpp: the DKC1 texture count is now control[0xb3]+[0xb4]+[0xb5] (Kirby: +0xb3 always 0, textures and
  samplers in +0xb4/+0xb5). All "NVN 2 ... handle unmatched" lines are gone (depth pyramid / light culling now see
  their images). Brightness unchanged by it (`E:\kirby\c0fix`). Separate samplers in +0xb5 still get the default
  sampler (follow-up, cosmetic at most).
- Real cause of the dark picture: measured console = ours^0.37 at p5, mean and p95 alike = a pure gamma. The DLSS
  output (rgb10a2 UNORM 1920x1080) is read by the composite bc440c9e as it is; real DLSS returns the input's sRGB
  encoding. Our stand-in decoded it (sRGB curve since (26), square before). vk_kar_upscale.c: no decode by default
  (KAR_UPSCALE_DECODE=srgb / sq = old behaviour). Start line now mean 182 p5 116 p95 243 (console 199/150/243; before
  132/61/229), the console's hazy pastel look; menu 3D bright too (`E:\kirby\nodec`, `nodec2`,
  `E:\kirby\dull_fix_compare.png`). The checker said RAINBOW on nodec2 (white transition frame / brighter shots):
  false positive, grid `E:\kirby\nodec2_grid.png` is clean.
- Still open: cave glow (Kirby white-hot in the cave, f03900; console: pink) = separate bug.


## 2026-11-11 (31): dull fix REVERTED (washed out); cave glow traced to character ambient light
- (30)'s "no decode" default is reverted: user found the picture pale, and saturation confirms it (0.25 vs console
  0.38, decoded 0.43). vk_kar_upscale.c decodes again by default (KAR_UPSCALE_DECODE=0 = the bright, pale variant).
  1.10 is OPEN again. The console is brighter at about the same saturation = an exposure-like lift in linear light
  before the tone curve (about x6 in shadows), not a gamma at the end.
- Post chain read in clear text (spirv-cross on the cache .spv):
  - fs f0a46cda (cache 4389dbc0...): out = lerp(scene * c5[0].x * vignette, bloom * c3[2].x, c3[0].z). c5[0].x is the
    exposure: 0.047 at the start line, 0.657 in the cave (x14).
  - cs a0f4 (cache 7a4b5ef4...): the LUT builder. Its one texture is only used for textureSize (33) and resolves to
    its own image 0 (from_img), fine. Tone curve from c3 ([0] 0 0 1 2, [1] 1 1.3 1 0.5, [2].x 1.28): linear up to
    0.65 (x100 nits), shoulder to ~1.05 at 1.66, then a straight line of slope ~0.15 with no cap (8 -> 2.0,
    20 -> 3.9). So anything above ~1.0 after exposure clips to white in 184cb. The LUT has no NaN, max PQ 0.82-0.86.
- Cave (frame ~3900, `E:\kirby\cave2`, `cave3`): HDR scene before post has Kirby at ~8 in the cave AND at the start
  line (same value), cave floor 1.4, grass 1.8. With x0.657 Kirby is 5.3 = white. Console (`console_cave_ref.jpg`):
  Kirby is dark, darker than the blue floor. So Kirby's light must drop in the cave and does not here.
- What lights the characters: the 12x12 rgba32f cube in vs slot 0 (light probe). KAR_ZERO_TEX=12x12! makes Kirby and
  all riders dark (`E:\kirby\z12\cmp.png`). One probe for the whole race, uploaded once (~frame 2613); the game never
  rewrites its storage (same hash after a forced re-upload at 3850, KAR_ZERO_TEX now logs the storage hash). Texture
  ids differ per run, do not compare them across runs.
- Prime suspect: fs slot 2, d16 1024x1024 with 11 mips (tex 1472 at 0x5d0a00000 in most runs), compare sampler 576,
  matrix in c4[92..101] (about 200 world units wide, follows the player). It is rendered only 3 times, about every
  32 frames during the intro (13 passes each: level 0 + mips), last ~frame 2676, never during the race
  (KAR_DIAG_WRITE_AT=1024 logs every 1024x1024 d16 write from frame 2000). At frame 3900 it still shows the start
  area's plants (`E:\kirby\cave3\dump\pass_0143_tex1472_*.png`; TOTK_DUMP_TEX=<id> can now read depth textures).
  Likely the top-down "is there a roof above me" map that dims a character's ambient light.
- Ruled out for the missing updates: conditional rendering (KAR_DIAG_COND: no condition is off; KAR_COND_ALWAYS=1
  exists), GPU timestamps (KAR_TS_IDEAL=1: still 3 updates), dropped copies (no warnings), the 57 shaders that do not
  lower ("uses lowering scratch registers" 36, "register-bank copies" 21, plus 60 tessellation stages): the draw
  census shows no skipped draw in the race, they are only preloaded.
- Also checked: the three 1x1 float textures in the c5 arrays are all zero (null parameters).
- Race runs at ~40-46 fps mean here (replay spikes of 300-400 ms).
- Next: find the guest code that renders the 1024 d16 map (log the caller of the render-target set with a 1024x1024
  d16 depth target during the intro), then read what gates it in the race.
- New diagnostics: KAR_DIAG_WRITE_AT, KAR_DIAG_COND, KAR_COND_ALWAYS, KAR_TS_IDEAL, "cs bound tex slots" line in
  dispatch dumps.

## 2026-11-11 (32): cave glow = ambient, not the shadow map; dropped compute passes; register banks in the translator
- Work plan with what/why/how per step: RELEASE_PLAN "Arbeitsplan 1.3" (user asked for a plan before every change).
- Step A (KAR_ALT_SHADOW=<frame>:c:1024 + KAR_ALT_SHADOW_BLOCK=10 + TOTK_TEX_MEMO=0; new 0.0 depth stand-in
  shadow_dummy_at): the 1024 map is a reverse-Z sun shadow map (0.0 = lit). With 0.0 the whole scene brightens,
  with 1.0 it looks like the real run, so the stale map already says "covered" in the race; Kirby stays at ~210
  either way (`E:\kirby\altsh2`). The glow is ambient, in every dark section, not only the cave. The map never being
  redrawn in the race is its own bug (RELEASE_PLAN 1.11).
- Kirby's vs (cache c6272249..., fs ad4fec33...; decompiled in `E:\kirby\kshader`): ambient = per-object factor
  (SSBO s1 at c0 byte 0x60, +272..+280) x max(probe cube, c4[41]); c4[41] = 0.
- Dispatches whose compute stage did not lower were dropped SILENTLY (vk_exec_draw.c op_dispatch / op_dispatch_ind:
  `if (!cs) return`). Now logged once per program. Race: program 12 (48x18x1, every frame from boot) and 4-6 1x1x1
  programs from race start (caller 0x7005826570).
- Lowering fixes (third_party/eglantine lower_to_maxwell.h PrepareRegisterPressure): entries for 0x305 F2I, 0x307
  FRND, 0x807 SEL imm, 0x80b FSETP imm, 0xa10 IADD3 cbuf; 0x981/0x986 take Ra as a pair only with bit 90 (was bit
  72, part of their fixed mode pattern; rejected loads whose Ra ends a quartet). nvn2_lower.cpp logs the failing
  instruction's words and register fields.
- Underneath: 21 programs need register-bank copies (NativeRegisterCopy). New translator patch
  tools/xlate/patches/0024-native-register-banks.patch (banks 1/2 = IR GPRs 256-767: RegisterConsts, Ssa key map,
  InstMovR fields, MovR emit, RegisterUsage skips them); build_xlate.py expects 24 patches. nvn2_lower.cpp accepts
  bank copies, still rejects them together with a CAL. Translator identity f976cda951691299: every shader
  retranslates on the first start.
- Still failing after that (next): 0x386 / 0xf60 / 0x217 "scratch registers", 0x986 "global store size", 0x89a.
- (32 cont.) Result of the bank patch run (`E:\kirby\banks1`): no program fails on bank copies; program 12 runs again.
  Step L (0x217 IMNMX and 0x386 STG table entries; data register at bit 32 counted for 0x386/0x986 in the
  requires_shadow check): no dispatch is skipped for a missing stage any more (`E:\kirby\stepL`). Left: one 6.3 MB
  SPIR-V compute over KAR_CS_SPV_MAX (4 MB, own guard), 6 programs with 0xf60 (bindless TEX, not dispatched in the
  race), 0x89a, 60 tessellation stages (0x34e).
- Cave glow is NOT fixed by any of that. Facts so far:
  - One light probe for the whole race (KAR_DIAG_PROBE=1 + TOTK_TEX_MEMO=0 logs probe changes).
  - Exposure is artist-set per area, not auto (KAR_DIAG_UBO=f0a46cda889c917f:5 KAR_DIAG_UBO_N=1): 0.0472 outside,
    0.1334 forest, 0.6568 cave and intro, short blends (`E:\kirby\expo`).
  - Our cave is darker than the console's (mean 67 vs 118) while Kirby is brighter.
  - Dump line "vs ssbo c0+0x180 = slot 1 ... size ... +272": s1 is 0x274 bytes at the start (+272 = 0 0 0), only
    0xe4 bytes in the cave (the shader reads past the bound end; memory there 0 20000 1). On the console LDG has no
    bound, so out-of-range reads return memory; our Vulkan binding returns robust 0. Unclear whether the block is
    GPU-written (CPU copy stale) or the field mapping is still wrong.
  - Next: RELEASE_PLAN O1 (how NVN 2 SSBO sizes are taken; bind the rest of the page?) and O2 (SPIR-V override that
    outputs the factor as colour).
- Diagnostics added this session (all off by default): KAR_ALT_SHADOW size filter + 0.0 stand-in +
  KAR_ALT_SHADOW_BLOCK, KAR_DIAG_PROBE, skipped-dispatch log (always on, once per program), lowering failure register
  fields, dump "+272" line.

## 2026-11-11 (33): cave glow narrowed to Kirby's body surface light term
- (32)'s "s1 0xe4 bytes" was a dump bug: the dump used the descriptor index, not the NVN slot (c0 table entry, vertex
  table at 0x110: c0+0x180 = slot 7). Real s1 = 0x33c bytes, +272 = 1 1 1 at the start and in the cave. Dump fixed.
- Light zones (fs slot 5 table, slot 4 records x 320) are CPU-built and say "no zone" (0x10000) for everyone; the
  per-object local light records (vs slot 6, 128 bytes at index s0+460, 8 x {rgb, weight}) are CPU-filled and look
  sane (cave: blue or zero). Nothing GPU-writes them (KAR_DIAG_SSBO_WRITE=kirby logs writers of that buffer and the
  record every 60 frames at the first draw of fs 0432bb88).
- fs 0432bb88 / vs c2cdafed is the WARP STAR, not Kirby. Kirby's body = vs 736d6c16 / fs b3441c0b (cache
  97d8f8ef...spv, decompiled E:\kirby\kshader\body_fs.glsl / body_vs.glsl). SPIR-V overrides work by replacing the
  cache .spv (originals in the scratchpad *.spv.orig; restore after testing!). Shot split overrides
  (E:\kirby\bodysplit, bodysplit2): pre-fog colour = surface + per-vertex inscatter (black); the glow is in the first
  surface term (albedo light _2531 x material +144 x c4[15] x _2517), the _2123 x (...) term is black.
- c4 logs (KAR_DIAG_UBO=0432bb881b7a70ab:4 KAR_DIAG_UBO_N=56, E:\kirby\c4log): exposure c4[0].x 0.047 -> 0.657,
  zone bit c4[6].x = 3, cave globals c4[42..54] differ (c4[43].y 1e4 -> 1e5, c4[44] tint 1).
- New diagnostics: KAR_SSBO_MIN=<bytes> (bind storage buffers at least that large; no effect on the glow),
  KAR_ALT_SSBO_ST=<nvn stage> (stage for KAR_ALT_SSBO, default fs), dump "zone:" lines, KAR_DIAG_SSBO_WRITE.
- Frame numbers do not map to the same track spot across runs with TOTK_DUMP (amb4100 was not in the cave).
- (33 cont.) KAR_ALT_SSBO=3800:1 / :3 (fs light buffers off): Kirby still glows (caveat: guarded SSBOs are exempt
  from KAR_ALT_SSBO). KAR_ALT_SHADOW=3800:p:1024 (plain-depth reads of the sun map): floor goes white, Kirby unchanged.
  Body fs splits bodysplit3..7: glow sits in the green light sum _2439 (direct _382 x (...) and the ambient blend around
  _2093 = _995 [sun colour from fs s4 +112 x shadow _1349] + _1659 [clustered loop]); splits 6 and 7 disagree because
  Kirby's spot differs per run and the star underneath glows too. Next (RELEASE_PLAN R2): read numbers, not shots
  (override writes terms as colour, dump reads Kirby's pixel from the HDR r11g11b10f target as float).
- All shader-cache overrides are restored (cmp-checked against the scratchpad *.spv.orig).

## 2026-11-11 (34): R2-R5 readback; port menu additions
- R2-R5 (TOTK_DUMP_DRAWS=122 + body-fs overrides writing terms into even/odd pixel columns, analysis scratchpad
  `r2an.py`, runner `r2run.ps1`): Kirby's glow = vs light colour `_238/_240/_290` (= vs `_48.w/_50.x/_50.y` =
  s1+256 x max(_704..708, c4[41])) = 18/31/22 constant over Kirby in the cave; the other vs light (s0+256 x _103.xyz)
  is a sane 0.3/0.5/1.0. Next: R6 (`kshader/body_fs_r6.spv` ready) and a vs override printing the _704..708 sum terms
  (local light records vs s2 + idx*128 + 112, weight _270^3). Shader cache restored (cmp-checked).
- Port menu (built, tabs shown in `E:\kirby\menutest4\pages`): Sound tab "Mute in background"
  (TOTK_MUTE_UNFOCUSED, audout.c audio_menu_gain also used by audio_mixer.c), "Output device" (TOTK_AUDIO_DEVICE,
  audio_pick_device, after restart); Screen tab "Screenshot key (F12)" (TOTK_SCREENSHOT_KEY, gfx_present.c
  gfx_request_screenshot -> screenshots/kar-<date>.png), "Graphics preset" (TOTK_VIDEO_PRESET writes RENDER_SCALE,
  UPSCALER, SHARPNESS; menu.cpp rows_video_preset). Not hand-tested yet: pressing F12, switching device, alt-tab mute.
- (34 cont.) CAVE GLOW FIXED: the body vs reads its local-light records with LDG at c0-table base + (index loaded from
  s0+460)<<7 behind phis; legacy GlobalToStorage (graphics stages) found no candidate and emitted a helper returning 0
  (`_81` in kshader/body_vs.glsl), so weight 0 -> sky probe -> glow. New translator patch
  tools/xlate/patches/0025-multi-target-deep-candidate-search.patch (multi-target path walks the whole address
  expression for c0 slot-0 even-word candidates when the shallow phi walk finds none; runtime range check picks the
  buffer); build_xlate.py expects 25 patches; translator id 9d32a2f5a1881381 (all shaders retranslate once).
  Result `E:\kirby\r11fix`: Kirby pink in the cave, cave floor lit blue, dark in dark sections.
  Also: KAR_DIAG_SSBO_WRITE=kirby:<fs hash> picks the logged draw; tools/ovrun.ps1 = one-call override experiment.
- (34 cont.) After patch 0025: validation race run clean (pool SAMPLER size added; one benign "no colour attachment"
  warning); 1.10 start line unchanged (~130 vs console ~195, `dull3`); shadow map 1024 still never drawn in the race but
  reads "lit" there (KAR_ALT_SHADOW blocks in `shw4`: real 108, all-lit 112, all-shadow 69) so it is not the dull
  cause; 2.2 grey square and 2.3 placeholder preview card no longer seen (`hud1`, `menu5`) -> asked the user to confirm.
  kar_shot_check says RAINBOW on bright/pink menu shots (false positive). tools/shotmean.py: mean luma of shots.
- (34 cont.) Dark Kirby outdoors after patch 0025: his local-light records (vs slot 6, 128 B each, index s0+460) are
  mostly {0,0,0,1} in the race. Writer = one guarded compute program bound at the record base with 0x80 bytes (the c0
  table extent); in the cave idx 0 holds blue values, other indices mostly 0. Ruled out: shadow maps (`shk1`), guarded
  full / min size (`gfull` crashed early, `gmin1` no change), big write-back (`wb1`), submit snapshot of SSBOs
  (`snap1`, KAR_SSBO_SNAPSHOT=1 diagnostic), the 6.3 MB compute (`r13`), texture writes there (`tw1`), CPU writes via
  that address (new KAR_WATCH_WRITE=<frame> page-protect watchpoint, win32.c plat_diag_watch_write: no fault).
  tools/stubscan.py: 0 zero-returning global helpers left in the 8202 shaders of the new translator (2178 before).
  New diagnostics: KAR_DIAG_SSBO_WRITE=<addr>:<len> (+ "guarded" in the log), KAR_GUARDED_FULL=1, KAR_GUARDED_MIN.
  Next: decompile that compute program and see which record index each dispatch writes.

## 2026-11-11 (35): no-game work plan (RELEASE_PLAN section 0, P1-P6)
- P1 built, untested: separate sampler handles of NVN 2 compute texture arrays from the c0 texture table
  (GfxShaderRes.samp_tbl; gfx_shader.c arr_ubo branch, samp_cb 0 + has_c0; vk_exec_draw.c takes samp[] or tex[] of that
  entry). Kirby's local-light compute (cs 8e80afef..., handle 0x10090000 -> c0 byte 0x90) had sampler 0.
- P2 paper analysis of that compute (kshader/rec_cs.glsl): no translation gap; zeros come from its sums (_47[2..5],
  condition _2134) -> needs measured values (P6).
- P3 plan for buffer device address written (RELEASE_PLAN P3a-P3f).
- P4 10.3: portable folders already in main.c; tools/package_build.py now takes kar.exe and keeps its name.
- P5: E:\kirby\tools\fullRace_replay.txt for 3.1; docs/kirby-running.md "From your own dump to a running game".
- Also this session: KAR_SSBO_SNAPSHOT is now opt-in (=1), KAR_WATCH_WRITE, KAR_GUARDED_MIN/FULL diagnostics.
- Nothing run since the pause; P6 = one combined test run when the user allows it.
- (35 cont.) DARK KIRBY FIXED, measured end to end (RELEASE_PLAN "Aktuelle Spur" table): the local-light compute
  indexes c4 handles past element 31, the translator's arrays had 32 elements (UMin(i,31) -> dummy) so the probe atlas
  read 0. gfx_shader.c arr_patch (Eden-style widening) now defaults to KAR_ARR_N=512 (KAR_ARR_SCAN=1 measured handles
  up to element 416 in c4/c5/c6, 32 KB buffers; 1024 exceeded descriptors/memory). Needed with it: MAX_DESC 4096,
  bigger descriptor pools, material arrays in the descriptor-skip signature (arr_sig, cached), and vk_usnap keeps a
  system-memory copy of the snapshot (U.shadow) because reading handles word by word from the host-visible arena cost
  61% of the render thread (profile E:\kirby\prof1). Result `arr512`: Kirby pink, cave as nintendo.com, 40-48 fps.
- (35 cont.) P7 fixed: translator patch 0026-texture-query-size-missing-component (TXQ of a component the declared type
  lacks returns 1); build_xlate expects 26 patches; translator id fb1db5fc87630c78; spirv-val over 8589 new shaders: 0
  invalid. 1.10 re-measured with KAR_ARR_N=512 (`dull4`): start line mean 178-199 vs console 188-199 -> fixed (p5 ~15%
  darker). RELEASE_PLAN has a "Detailplan" (D1-D8) and new section 16 "Performance-Architektur" (bindless textures,
  buffer device address, parallel replay, direct memory, frame generation, Linux/4750G).
- (35 cont.) D8/1.11 shadow map: real bug (c4[92..95] follows player position/heading in the race, mat1) but the pass
  that draws 0x5d0a00000 (f_7005823d40 via f_7005827c20 at 0x7005827cac/0x7005827d0c) runs every intro frame and never
  from race start (fnw3). Pass and job objects are reallocated per frame (fnw4/5). Next: decompile the frame setup
  around 0x7000e90348 (Ghidra) to read what enqueues the shadow job. New tools: KAR_FNWATCH (FN_ENTER now passes x1;
  cpu.h change rebuilt all generated C, 17 min), KAR_DIAG_RT1024 (filters the map at 0x5d0a00000, logs frame/graph/pass).


## 36. 2026-11-12: Ruckler im Rennen gelöst (16.1a), Speicher-Aufschlüsselung (16.10)
- Messwerkzeug `KAR_REPLAY_PROF=1` (vk_exec.c record_impl/walk; Unterzeiten op_dispatch, shader_for, gfx_shader_get, disk_load_from): Spitzen = erster Dispatch eines Compute-Programms lädt ~1 MB SPIR-V von Platte + baut Pipeline im Replay.
- Fix: Compute-Prefetch (gfx_shader.c `gfx_shader_prefetch_cs`, Aufruf nvn_api_obj.c prog_set_shaders, Rückruf vk_pipeline.c `cs_prefetch_sink`), SPIR-V nach Pipeline-Bau frei (`gfx_shader_drop_spirv`, Nachladen in `gfx_shader_spirv`), Puffer `KAR_CS_PREFETCH_MB` 128, `KAR_CS_PREFETCH=0` aus. Wachsende Warteschlange (vorher verwarf sie Programme über 1024).
- Eigener VkPipelineCache für kar* aus (`KAR_APP_PIPELINE_CACHE=1` an). Zensus erweitert (vk_memory.c: Heaps, Shader-Cache, Gast-Speicher memory.c `mem_guest_committed`).
- Ergebnis rmem4: Rennen 1% low 51,3 fps, Mittel 60, 0 Frames > 25 ms; privat 16,7 GiB. Ziel 16.9: < 8 GB. Nächste Hebel in RELEASE_PLAN 16.10.
- Offline-Werkzeuge: E:\kirby	ools\csvariants.py, spvops.py. Pack `shader-cache.pack` (14,5 GB) gebaut, half nicht; Loose-Dateien behalten (--keep).
- Aufräumen offen (Nutzer fragen): ~20 GB `pipeline-cache.bin.*.tmp` Reste in build/kar458752.
- Nachtrag 2026-11-12 (später): VRAM-Vorab-Blöcke für kar* aus (vk_headroom.c), Grafik-SPIR-V nach Vorbau frei (`gfx_shader_drop_all_spirv`, Aufschub 5 s), Nutzungslisten `shader-cache/cs-used.bin` und `pipeline-used.bin` (nur in den letzten 8 Sitzungen Genutztes wird vorgebaut/vorgeladen). Diagnose: `KAR_MEM_MAP=1` (Speicherkarte), `KAR_MEM_MAP_DUMP=<dir>`, Zensus mit Pipelines gebaut/benutzt, `KAR_CS_WORKERS`. Stand rmem15/16: privat 10,6 GiB, Arbeitsspeicher 6,7 GiB, Rennen glatt. 144 fps: docs/dkb-frame-rate.md (DKB_REALTIME-Patch) als Vorbild → RELEASE_PLAN 16.11.
- 16.11 (2026-11-12): Kirby rechnet Spielzeit mit Echtzeit (Rennuhr = Wanduhr bei 30 und 144 fps), wartet aber pro Bild auf das 60-Hz-Vsync-Ereignis. Umgesetzt: frame_rate.c `frame_rate_self_timed`/`frame_rate_vsync_hz`, nvn_api_obj.c present_period, gfx_present.c vsync_main (Rate = Limit, Neustart bei Wechsel), Skript-Uhr über 60 fps = Echtzeit. Limit 144: Menüs 86 fps, Rennen ~66 (Replay-CPU). Limit 60 unverändert (lim60b). Hinweis: build/kar458752/dkb.ini des Nutzers hat fps_limit = 144 (greift jetzt). Diagnose: KAR_PACE_HZ, KAR_VSYNC_HZ, KAR_PACE_FROM; Screenshot-Zeitstempel im Log.
- 16.4a (2026-11-12): Engpass-Analyse Rennen bei Limit 144: kein Thread ausgelastet (`KAR_THREAD_CPU=1`), GPU 6–8 ms, Replay läuft auf dem Render-Thread des Spiels. `TOTK_REPLAY_ASYNC=1` schlechter. Umgesetzt: Material-Arrays per Block lesen (`ubo_block`, `KAR_ARR_BLOCK=0` alt) → Replay 12,1 → 9,6 ms/Bild. Descriptor-Satz-Cache (`KAR_DS_CACHE=1`) verursacht Bildmüll in der Fahrzeug-Vorschau → aus. Neue Messzeile mit `KAR_REPLAY_PROF=1`: „replay X ms per frame, bind_resources Y“ pro 600 Frames. Nächster Schritt: 16.2 Bindless.
- D8 (2026-11-12, spät): Schatten-Recorder gefunden (`f_7005859540`, über `f_70058497c0` aus `f_7005856740`), läuft im Rennen weiter, setzt aber kein 1024er-Ziel mehr. Trace-Aufnahme E:/kirby/trace.bin (1,2 GB) liegt vor für Offline-Auswertung. Vorgaben Nutzer: kein ImGui außer Leistungsanzeige; Ladebildschirm 13.3a nativ; weniger Einzeltests, einmal groß messen und offline auswerten. Fritchii NX2_NOTES-Hinweise → RELEASE_PLAN 16.12.

## 37. 2026-11-12: Ladebildschirm nativ, Spielmenü-Einbau (15.x) begonnen
- Ladebildschirm (13.3a) nativ in gfx_present.c (GDI-Text `plat_text_draw`, Upload + `vk_present_image`), läuft (7 Bilder/486 ms bei warmem Cache), Bild E:/kirby/boot_screen.png. Vorgabe Nutzer: Design 1:1 wie Kirby → heutiger rosa Bildschirm ist Platzhalter.
- Nutzerwahl: echter Einbau ins Spielmenü (kein eigenes Menü). Weg zu den Optionen per Skript: E:/kirby/tools/menu_opt.txt (Hauptmenü → Leiste → Zahnrad), menu_disp.txt (öffnet „Barrierefreiheit“).
- Asset-Format: assets.cat „PARC“ v4 (64-Byte-Einträge, zstd), Werkzeuge E:/kirby/tools/kar_assets.py, kar_msbt.py (MSBT-Texte, deutsch 4231). Seite „Barrierefreiheit“ = Prefab „Pfab“ v4 (E:/kirby/options.pfab). Prefab-Lader `f_70048deb40`/`f_70048e000c`. Details RELEASE_PLAN 15.1.
- Neuer Bildfehler 2.6: leere Sprechblase in der Hauptmenü-Leiste. Langzeittests 3.4/12.5 ans Ende verschoben (Nutzer).
- Diagnose neu: `KAR_LOG_ASSET_READS=1` (romfs.c), `KAR_BOOT_SHOT=<png>`.
- 15.x Stand (Dauer-Modus /loop läuft): Einbauort Seite „Anzeige“ (Skript tools/menu_anz.txt). Liste fest 5 Zeilen in `f_700300d3c0`; Zeilen = Prefab-Entities; Texte MSBT. Weg B (PC-Modus über Textumlenkung) gewählt; Textsuche `f_7000001300`/Hash `f_7000001600`. Details RELEASE_PLAN 15.1.
- Text-Haken (15.x Schritt 1): Patch-Set `karmenu` (KAR_MENU, Standard 1) in recomp/patches-kar458752.txt, beide `bl f_7000001300` in `f_70011302c0` → Host `text_lookup` in runtime/kar_menu.c (Patch-Seite 0x70093c0000 +0x300, Texte ab +0x400). Da die Seite > 128 MiB entfernt liegt, neues Ersatzwort `@call:<Versatz>` (recomp/patches.py, gen.py: Aufruf ohne ARM-Kodierung). patches.py liest die Tabelle jetzt mit `utf-8-sig` (Datei hatte BOM → Generator brach ab). Diagnose `KAR_MENU_LOG=<Label-Teilstring>` (jede passende Suche mit Bildnummer, max. 3000). Probe: „Mnu_Opt_Disp_TextSize“ → „Auflösung“.
- Bau mit karmenu ok; Log zeigt Haken aktiv (`patches: karmenu on`, Textsuchen laufen jedes Bild, z. B. `Mnu_MatchUp_Rider`). Menüskript menu_anz.txt läuft bei FPS-Limit 144 (dkb.ini) aus dem Takt (auch mit KAR_MENU=0, also nicht der Haken) → Tests mit `-Env 'TOTK_FPS_LIMIT=60'`. Bildprüfung „Auflösung“ steht noch aus (WardogsClient lief).
- Offline-Runde (Nutzer: „schau was offline geht“): 6.3 ✅ `rt_game_is_kar()` (host.h/main.c, 13 Stellen ersetzt, Bau ok). 13.2 bewertet (Kirby-Installer fehlt, ~1 Tag, später). 15.x statisch: Reiter „Allgemein“ = `Mnu_Opt_Disp_OptMode_Cmn` (Prefab), Controller-Seite mit L/R-Modi `f_70030155e0` als Vorbild; Zeilen-Code in `f_700300d3c0` ist ECS-Code mit Typ-Hashes → ohne Messung nicht sinnvoll weiter. Nächster Spiellauf (wenn WardogsClient aus): `kar_race_test.ps1 -Runs 1 -Script E:/kirby/tools/menu_anz.txt -Shots 1700-2600:150 -ShotCount 7 -Env 'TOTK_FPS_LIMIT=60;KAR_MENU_LOG=Opt_Disp'` → Bild „Auflösung“ + wann die Seite ihre Texte holt.
- 15.x Schritt 2 (L/R „PC-Grafik“) teilweise gebaut, dann auf Nutzerwunsch zurückgestellt; Stand + offene Punkte in RELEASE_PLAN 15.1 „Stand (2)“. Weiter mit Stabilität 3.x/12.x.

## 38. 2026-11-12: eigenes Repo iiVoiX/kar
- Nutzer-Repo https://github.com/iiVoiX/kar: lokaler Zweig `kirby` (im DKB-Klon, vorher `bananza`), Remote `kar`, gepusht als `kirby:main` (Commit 40df6ab, 936 Dateien, keine Assets – geprüft mit `tools/check_content.py --staged` und Pfadliste). Neue README (deutsch) mit Bau, Aufbau, Schaltern, Tests; alte DKB-README → docs/README-dkb-upstream.md. Ordner `kirby/` = RELEASE_PLAN.md, HANDOFF.md, Eingabeskripte, Analyse-Werkzeuge (Kopien aus E:\kirby – vor dem nächsten Commit neu kopieren).
- Ultrawide (Nutzerwahl, vorgezogen): Messung wide2: `KAR_DISPLAY_RES=3440x1440` + Fenster 3440x1440 → Spielbild bleibt 2560x1440 (16:9), Spiel übernimmt die gemeldete Größe nicht von selbst → Kamera/Bildgröße im Spielcode suchen (weiter in 17.3/14.10).
- Ultrawide 17.3 (Stand): `GetDefaultDisplayResolutionChangeEvent` wird bei gesetztem KAR_DISPLAY_RES beim Start ausgelöst (ov_system.c) → Spiel fragt 3440x1440 ab, ändert aber nichts sichtbar. Patch-Set `karwide` (KAR_WIDE=0 Standard, runtime/kar_wide.c, Stelle 0xf344a8) setzt eine Anzeige-Info (war 1280x720, falsches Objekt). Hauptziele 2560x1440 kommen aus dem Render-Graph (Kette in RELEASE_PLAN 17.3). Diagnose `KAR_SIZE_WATCH=<Breite>` (nvn_api_obj.c), Log `oe: GetDefaultDisplayResolution -> …`.
- Ultrawide Plan C: Patch `karwide` Stelle 0x5b75408 (`f_7005b74840`, Render-Graph-Größenklassen) streckt 16:9-Größen auf KAR_DISPLAY_RES-Seitenverhältnis → Render-Graph-Ziele 3440 breit, läuft stabil. Ausgabe noch 2560 (Fenstertexturen über `f_7004c3e0d8`, Breite selbst berechnet). Weiter: RELEASE_PLAN 17.3 „Nächste Schritte“. Testaufruf dort.
- Ultrawide wide14–16: Haken `f_7004c3e180`+0x20 (`@call:360`, kar_wide.c `window_hook`) → Fenstertexturen + Ausgabe 3440×1440. Inhalt noch 2560 breit (Composite-Viewport) – nächster Schritt in RELEASE_PLAN 17.3.
- Ultrawide Stand (Commits de276c3, 0a7ffe9, 5f057a2 in iiVoiX/kar): mit `KAR_WIDE=1;KAR_DISPLAY_RES=3440x1440;TOTK_WINDOW_SIZE=3440x1440` Ausgabe 21:9, 3D-Kamera Hor+; HUD/Menüs noch gestreckt (UI-Ziel/Layout, RELEASE_PLAN 17.3 „Nächste Schritte HUD“). Diagnosen: KAR_SIZE_WATCH, KAR_VP_WATCH, KAR_FIND_F32, Log „kar wide: widen …“.
- Ultrawide Stand 2026-11-13 (Commits bis 227844e): mit dkb.ini `ultrawide = 1` + `output_res` 21:9 automatisch aktiv. Ausgabe/Zuschnitt 3440×1440 ohne Balken, 3D Hor+ (alle Kameras inkl. Seitenverhältnis-1-Kameras der Startszene), HUD/Menüs unverzerrt 16:9 mittig (UI-Ebene 2576×1080). Offen: Banner/UI-Seiten an 16:9-Grenze, Szenen-Schärfe (zwei Versuche verworfen: Ziel-Haken, Basistabelle). Prüfen immer mit `KAR_SCREENSHOT_AT` (echtes Swapchain-Bild).
