# Development log

Engineering diary of the bring-up, one entry per task (letters C ... FF are task ids), newest at the bottom.
For an overview read `HOW_IT_WORKS.md` and `VERIFYING.md` first.

## 2026-10-12: hosts build and boot to the title screen (pure LLE)

### What exists

| File | Role |
|---|---|
| `runner/tlk_game.c/.h` | `kTlkGameInfo`: the framework's beam-aligned driver (`beam_frame_driver.h`), unchanged. No widescreen, MSU-1 or custom presenter |
| `runner/sdl_main.c` | `tlk_sdl`: shim over the framework desktop host (`snesrecomp_desktop_main`), same as the upstream `tools/new_project` template |
| `runner/headless_main.c` | `tlk_headless`: windowless host, PNG dump, input tape, coverage log |
| `runner/headless_host.c` | host-contract symbols the desktop host would otherwise define (`Die`, `RtlApuLock`, `g_spc_player`, `RtlDrawPpuFrame`, `g_ws_*`, `g_new_ppu`, `MkDir`, `ChangeWindowScale`) |
| `runner/host_contract.c` | empty `debug_on_*` trace hooks (non-trace build) |
| `runner/verified_rom.c/.h` | SHA-256 check for the headless host (strips a 512-byte copier header) |
| `runner/variables.h` | required by `snes/cpu.c`; empty |
| `runner/tlk_interp_targets.txt` | compiled bodies known to diverge (see below) |
| `cmake/rom_identity.txt` | ROM digests for `snesrecomp_rom_identity()` (CRC32 `c8fbfaa8`, SHA-256 `457abe63...3712`) |

Decisions:

- **Upstream snesrecomp, not the DKC3 fork.** DKC3's `runner/` speaks the fork's
  API. TLK pins upstream `RetroPortingToolKit/snesrecomp`, whose scaffold already
  provides the desktop host and the beam driver, so the hosts follow the
  upstream `tools/new_project` templates instead of copying DKC3.
- Both executables compile the runner sources and the generated C themselves
  (`snesrecomp_target_generated_code`); `snesrecomp_game` is untouched.
- `SNESRECOMP_ENABLE_MODS=ON` (the desktop host requires it), mod catalog `NONE`.
- `tlk_headless` links SDL only because `common_rtl.c` includes `sdl_compat.h`;
  it never opens a window.
- No changes to `snesrecomp/` or `recomp/*.cfg`.

### Build and run

```sh
./build.sh          # snesrecomp_game + build/tlk_headless + build/tlk_sdl
R="../Lion King, The (USA).sfc"
build/tlk_headless --rom "$R" --frames 600 --lle --dump-png out.png --log out.json
build/tlk_headless --rom "$R" --frames 3000 --lle --dump-png out.png --dump-every 100
# SDL under Xvfb, frame-600 screenshot written by the framework host:
SNESRECOMP_LLE_BOUNCE=0 SNESRECOMP_RUN_FRAMES=700 \
SNESRECOMP_SCREENSHOT=f600.ppm SNESRECOMP_SCREENSHOT_FRAME=600 \
  xvfb-run -a build/tlk_sdl --no-launcher --rom "$R"
```

Headless flags: `--rom`, `--frames N`, `--dump-png path`, `--dump-every K`,
`--input tape` (u16 LE per frame, `$4218` layout: bit 15 = B ... bit 4 = R),
`--log path` (JSON log, also enables tier-2 coverage capture and writes
`<log>.tier2.json`), `--interp-targets file`, `--lle`.

Log contents: interpreted opcode count and unique PCs (per bank and as runs),
compiled-body entries, tier-downs (`interp_tier_hit_count`), the tier-2 gap
table, unresolved-dispatch abandons and the dispatch log.

### Three execution modes

- **aot** (default): the interpreter bounces into compiled bodies.
- **lle** (`--lle` = `SNESRECOMP_LLE_BOUNCE=0`): the interpreter never bounces into
  compiled code. Interrupt entries (the NMI handler `$C09EC6`, `$C0A0F0`, ...)
  still run compiled.
- **aotx** (`--interp-targets runner/tlk_interp_targets.txt`): aot minus the 18
  bodies that diverge.

### Measurements (headless, Release, 2026-10-12)

| mode | frames asked | ran | interp opcodes | unique interp PCs | compiled entries (unique) | tier-downs | tier-2 bails | result |
|---|---|---|---|---|---|---|---|---|
| aot | 600 | 5 | 2,065,645 | 1,966 | 1 (1) | 5 | 1 | g_fail |
| lle | 600 | 600 | 9,894,995 | 5,336 | 2,879 (8) | 0 | 0 | ok |
| lle | 3000 | 3000 | 48,251,621 | 12,375 | 16,469 (8) | 0 | 0 | ok |
| aotx | 600 | 600 | 9,499,784 | 3,691 | 6,980 (47) | 567 | 0 | runs, picture wrong |
| aotx | 3000 | 2391 | 33,590,802 | 3,785 | 33,810 (48) | 4,143 | 1 | g_fail |

Unresolved-dispatch abandons: 0 in every run.

What the pictures show:

- lle: f100 black; f200 "A Disney Software and Virgin Interactive co-production";
  f300 "Developed by Westwood Studios / Animation by Walt Disney Feature Animation";
  f400 to f500 the Pride Rock intro; **f600 the title screen** (logo, START/OPTIONS,
  copyright lines). f3000: attract-mode demo of level 1 (Pride Lands), young
  Simba, lives counter 2.
- tape test (`--lle`, Start held for frames 620-629): f1200 is in level 1 with Simba at the start.
  Control run without the tape: f1200 is still the title screen. Input works.
- `tlk_sdl` under Xvfb (lle, 700 frames, 53 fps, exit 0): the framework screenshot at
  f600 is the same title screen. It differs from the headless f600 only in the animated
  sun cursor (bbox 62,75-87,99). An X11 root grab of the Xvfb display came out black
  (GL presenter); use the framework's `SNESRECOMP_SCREENSHOT` instead.
- aotx f600: garbled title-screen tiles; f3000: garbage (crashes at f2391).

### Why aot dies at frame 4

Boot runs interpreted from `$C08007` and JSLs `$C22C4E` (compiled). Its callees
`$C218F4`, `$C218FD`, ... are JSL veneers (`BRL` into code that "calls" with
`PER`+`BRL` and returns with `RTS`/`RTL`). After them, the compiled `$C22C4E`'s
`RTL` at `$C22C8F` pops a garbage return address, `$2C:6401` (open bus in
HiROM), and the CPU ends up in a BRK loop at `$70:8013`. tier-2 log: site
`0xC22C8F` -> `0x2C6401`, `indirect_dispatch`, `missing_entry`, bail.

Per-body bisection: each of the 73 non-interrupt bodies hit in the boot path
was run as the only compiled body, everything else interpreted, and the f600
frame hash compared with lle. 55 matched exactly. These 18 differ:

```
C07353 C09695 C0983D C098DD C0ABBB C218F4 C218FA C21906 C21909
C2190F C2191E C22ACA C22B36 C22C48 C22C4E C22CEC C3B817 C465F0
```

The 18 include the `$C218xx`/`$C219xx` veneers and every hand-added
`exit_mx_at` boot function (`$C22C4E`, `$C22ACA`, `$C22B36`, `$C07353`), which
points at the cfg width/stack hints rather than at the host. With those 18
excluded the game still breaks (aotx), so other bodies also go wrong when
combined, or only on paths the single-body runs never reached.

### Open problems

1. The AOT bodies above are wrong. Fix them in the cfg/analysis (task B / next
   step), then rerun the bisection: `tlk_headless --interp-targets <list>`
   against the `--lle` frame hash (f600 lle `frame_sha256=0f1fa1de...50b9`).
2. In lle mode almost everything is interpreted (8 unique compiled entries, all
   from the interrupt path). Treat that as the reference for regression checks,
   not as the recomp.
3. `tlk_sdl` defaults to aot and crashes at boot like the headless host. Use
   `SNESRECOMP_LLE_BOUNCE=0` until problem 1 is fixed.
4. Audio is not checked (SDL ran with a dummy/real device and no errors; nothing
   was listened to).

## 2026-10-12: default (recompiled) mode matches `--lle` (task C)

### Result

`scripts/compare_modes.sh` (default vs `--lle`, `frame_sha256` per frame):

```
$ scripts/compare_modes.sh 600 3000
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
600    MATCH    0f1fa1dea806bc08   0f1fa1dea806bc08   differs     600/600
3000   MATCH    3e9d33c2e94735b1   3e9d33c2e94735b1   same        3000/3000
$ scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 3000 6000
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
1200   MATCH    dd861e164c295e15   dd861e164c295e15   same        1200/1200
3000   MATCH    f370fe92ede1b004   f370fe92ede1b004   same        3000/3000
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
```

Before: default mode died at frame 5. The WRAM hash at f600 still differs
(frames identical; WRAM identical again at f3000); not investigated.

### Causes and fixes

1. **PER+BRL sound library** (`$C218F4-$C21921`, 16 JSL veneers into
   `$C21924-$C2238x`). Bodies call local helpers with `PER *+2 / BRL helper`
   and the helpers return with RTS (54 sites). The analyzer reads PER as a data
   push and the helper's RTS as the veneer's own exit, so the compiled veneer
   leaves through an RTS dispatch on a foreign frame. Fix: `force_lle` on all
   16 veneers (bankc2.cfg). Not a native call ABI.
2. **Wrong / unproven exit widths.** `scripts/check_exit_mx.py` walks each
   declared routine from every entry width the analyzer asks for (manifest call
   demands) and reports exit widths. On the old cfg: 94 ok, 32 WRONG, 76
   UNPROVEN. Almost all WRONG were PHP...PLP routines (exit = entry) declared
   with one broadcast `exit_mx_at` while entered in several widths (e.g.
   `$C0983D` declared 0,0 + 0,1 + 1,1); `$C070F6` (entered M1X1, declared 0,0)
   was plainly wrong. Each WRONG/UNPROVEN line is now commented out in place
   (`# task C wrong|unproven: ...` with the evidence) and replaced by one
   `exit_mx_variant` per entry width the walk proved. Now: 128 ok, 5 UNPROVEN
   (no entry width in the current manifest; their routines are not compiled).
3. **Interrupt waits under a compiled caller.** A compiled body runs an
   interpreted callee in a nested interpreter frame
   (`interp_tier_run_call_frame`). That frame never yields to the frame driver
   (the deadline yield only fires from the top-level scheduler frame), so a
   callee that spins until an interrupt handler writes RAM hits the 2M step cap.
   Two such waits:
   - `$C218FA` (body `$C21987`) spins at `$C21A47-$C21A57` (`LDA $00 / BEQ`,
     D=`$1F00`). Fix: `force_lle` on every compiled function that reaches it,
     computed with `scripts/lle_closure.py` (27 functions, bankc2.cfg block
     `BEGIN/END lle_closure C218FA`; interrupt entries `$C0FEB3`/`$C09E0C`
     stay compiled, they are host-invoked as in `--lle`).
   - `$C0ABBB` object walker calls handlers with `PHK / PEA $ABFD /
     JML [$0014]`; handler `$C537B2` waits on the NMI counter (`CMP $0A0A /
     BEQ`). `force_lle C0ABBB` (bankc0.cfg). Checked: without it default mode
     dies at f572 (step cap at `$C537B2`).
   This is a framework limitation, not a TLK bug; the runtime was not patched.

`runner/tlk_interp_targets.txt` is now empty (comment only); no runtime
exclusion list is needed.

### Compiled vs interpreted

Static (v2_emit): **185 roots, 187 exact AOT variants, 96 LLE variants**
(old cfg: 212 / 345 / 17, but those bodies were wrong). 44 `force_lle`
boundaries: 16 veneers, `$C0ABBB`, 27 closure functions.

Runtime (headless, Release):

| run | interp opcodes | unique interp PCs | compiled entries (unique) | tier-downs |
|---|---|---|---|---|
| lle, 3000 f | 48,252,143 | 12,396 | 16,467 (7) | 0 |
| default, 3000 f | 39,438,692 | 9,111 | 48,589 (73) | 7,265 |
| lle, 6987M tape, 6000 f | 94,987,320 | 17,254 | 34,467 (7) | 0 |
| default, 6987M tape, 6000 f | 80,972,842 | 13,598 | 152,959 (81) | 39,490 |

So default mode still interprets ~82-85% of executed instructions: the main
loop (`$C089AA`, `$C08E62`...) and the level routines that reach the sound
wait are on the interpreter.

### Tools

- `scripts/compare_modes.sh [-t tape] FRAME...`: default vs `--lle`, MATCH /
  MISMATCH per frame; `AOT_ARGS` adds flags to the default run.
- `scripts/check_exit_mx.py [PC24[:M,X]...]`: verify `exit_mx_at` /
  `exit_mx_variant` against the ROM (path-sensitive width/stack walk, follows
  PER+BRL local calls; opaque callees are assumed balanced and tagged).
- `scripts/lle_closure.py --cfg --exclude C0FEB3,C09E0C C218FA <current block>`:
  recompute the closure after cfg changes (pass the current block's PCs as
  extra targets, since forced functions no longer show their callees).

### Open problems

1. Coverage: to compile the main loop, the nested-interpreter frame would need
   to yield on the frame deadline (framework), or the sound library would need
   an analyzer model of `PER/BRL` calls so `$C218FA` itself compiles (its wait
   loop would then yield like any compiled loop).
2. f600 WRAM differs between modes although the frame matches.
3. Only frames 600/3000 (no input) and 1200/3000/6000 on 6987M were compared;
   later tape frames and the other tapes are unchecked. `tlk_sdl` default
   mode was not rerun.

## 2026-10-12: poll-aligned TAS replay in `--lle` (task E)

**Poll definition.** TLK has exactly one pad read in the ROM: `LDA $4218` at `$C0A07C` (then `$4219` at
`$C0A082`, stored to `$005A/$005B`), reached once per game-loop iteration after waiting out `$4212` bit 0.
A *poll* = one execution of the opcode at `$C0A07C`. Lag frames never reach it. `tlk_headless
--input-polled <tape>` arms an interpreter pre-opcode hook there (`interp_bridge_add_pre_opcode_hook`, public
runtime API, no runtime patch) that loads the next tape word into the pad state just before the read. Needs
the read to be interpreted (`--lle`); `polls_seen=` in the summary shows the hook fired. The polled path skips
the runtime's left+right / up+down filter: the TASes press Left+Right (6987M poll 930 reads `$0300`), and
`RtlRunFrame` strips that, so the old per-frame `--input` path also loses those inputs.

The BizHawk lag structure (runs of lag frames between polls) matches the headless poll structure exactly
except for length: boot 104 vs 108, title 7 vs 8, level-1 load 145 vs 149 frames. So per-poll inputs fix the
input drift task D saw.

**Two kinds of polled tape** (`tools/bk2_to_tape.py --polled --lag ...`):
- `--lag bh_lag_X.txt`: movie rows of BizHawk's non-lag frames. Desyncs at 6987M poll 638 (first poll after
  the level-1 load): BizHawk's read there returns a stale `$0000` (no auto-joypad latch in that vblank; the
  game enables `$4200` late in bsnes), while row 941 was `$0100`. In headless the latch did happen, so Simba
  walks one frame early, and X stays 1 poll ahead until the real desync at poll 721.
- `--read-values --lag bh_ram_X.txt`: the word the game actually read in BizHawk on each poll (`$005A` at the end
  of every non-lag frame). This sidesteps latch races, and it's the one to use.

**Results** (`tools/ram_sync_compare.py`, poll-aligned RAM: RNG `$1E00`, health `$2004`, Simba X/Y `$B21B/$B21D`,
level `$1FF9E`, lives `$1FFAA`, pad `$005A`; a field matches if it equals BizHawk poll k-1..k+1, and a desync has
to last 8 polls):

| Tape | Old per-frame `--input` | `--polled --lag` | `--polled --read-values` |
|---|---|---|---|
| 6987M | ~f1012 | poll 721 (f1016) | **poll 1779, f2089 (BizHawk f2096)** |
| 6781M | ~f1012 | poll 721 | poll 1779, f2089 |
| 4735M | ~f1012 | poll 721 | poll 1779, f2089 |
| 2716M | ~f1170 | poll 1770 | poll 1770, f2076 (BizHawk f2085) |

Pixels agree: 6987M headless PNGs every 10 frames vs `bhd/6987M` (`sync_compare.py --win 12`) have mad=0.000
from f950 to f2080 at a +9..+8 offset. The first non-transition DIFF is f2090. None reach the ending: they
desync in Pride Lands part 2 (Simba takes a hit there and BizHawk doesn't).

**Next cause (with evidence): in-game lag from CPU timing, not input.** Starting from the read-values run, the
first lag mismatch after the load is a 1-frame lag in headless at f1782 (after poll 1482) that BizHawk does not
have. Several more ±1 lag frames on both sides follow (polls 1660-1768). `$0A0A` (also `$00E5`, `$E1C9`, `$F930`) is
a per-frame counter that ticks on lag frames too. WRAM diffs at poll 1500 show `$0A0A` +1 and an object table
(`$E2F5`, stride 12) +1, and then `$1F0x` and OAM drift, until the hit at poll 1779. 2716M shows the same
pattern (headless lag at poll 1654). So the runtime's CPU/DMA cycle cost per frame differs from bsnes enough
to change lag frames during gameplay, and the frame counter feeds object timing. Fixing this needs timing
accuracy in snesrecomp's LLE (per-opcode clocks, slow/fast ROM `$420D`, DMA/HDMA stall), not input
alignment. Not attempted: a lag-forcing replay mode (hold the game the extra frame when BizHawk lags) would
also work around it.

Notes: WRAM pages `$0100-$02FF` (stack) and `$0F3E-$128A` differ from early on with no effect. BizHawk
fills uninitialized RAM with a pattern and headless zeroes it. The two hosts also sample RAM at different
points in the frame: BizHawk's frame ends around the auto-joypad latch, headless at v=225 after it. That's
why the compare tolerates ±1 poll.

Repro (BizHawk logs ~15-30 min per movie; mono sometimes segfaults when 4 run in parallel, so run 2 at a time):
```sh
S=build-host/out/sync/ram; R="../Lion King, The (USA).sfc"
cmake -S . -B build-sync -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-D_GNU_SOURCE && cmake --build build-sync --target tlk_headless
LAG_OUT=$PWD/$S/bh_lag_6987M.txt RAM_OUT=$PWD/$S/bh_ram_6987M.txt LD_LIBRARY_PATH=$BIZHAWK/dll:$BIZHAWK \
  xvfb-run -a mono $BIZHAWK/EmuHawk.exe --config=$CFG --movie=6987M.bk2 --lua=tools/bizhawk/ram_log.lua "$R"
python3 tools/bk2_to_tape.py 6987M.bk2 --polled --read-values --lag $S/bh_ram_6987M.txt -o $S/tapes/6987M.read.tape --resets-output /dev/null
./build-sync/tlk_headless --rom "$R" --frames 39208 --lle --input-polled $S/tapes/6987M.read.tape --ram-log $S/hl_read_6987M.txt
python3 tools/ram_sync_compare.py $S/hl_read_6987M.txt $S/bh_ram_6987M.txt
```
Diagnostics in `tlk_headless` (env): `TLK_POLL_TRACE=1` (beam position per poll), `TLK_WATCH=<wram off>` (PC of each
interpreted write), `TLK_WRAM_DUMP=f1,f2 TLK_WRAM_DIR=d` (128 KiB WRAM dumps). The BizHawk side is
`tools/bizhawk/wram_dump.lua` (`DUMP_FRAMES`, `DUMP_DIR`).

## 2026-10-12: coverage loop (task G)

### Result

All compared frames match `--lle` on the final cfg:

```
$ scripts/compare_modes.sh 600 3000
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
600    MATCH    0f1fa1dea806bc08   0f1fa1dea806bc08   differs     600/600
3000   MATCH    3e9d33c2e94735b1   3e9d33c2e94735b1   differs     3000/3000
$ scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 6000
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
```

Also MATCH after every iteration: `build/playtapes/l01_right.tape` f2500/4500/6000,
`l07_random.tape` f2500/5000, `l10_right.tape` f3000/6000. `scripts/check_exit_mx.py`: 135 ok, 0 WRONG,
0 UNPROVEN.

Task H's runtime patch is integrated (`cmake/TlkRuntimePatches.cmake`, `cmake/runtime-patches/`,
`scripts/apply_runtime_patches.py`, 4 lines in `CMakeLists.txt`); the `lle_closure C218FA` block
(bankc2.cfg, `#H`) and `force_lle C0ABBB` (bankc0.cfg) are commented out. `runner/headless_main.c`
gained only H's `TLK_INTERP_HIST=<path>` per-PC histogram (task E's code untouched).

### Metric

`scripts/interp_share.py` (new): level-1 play tape (`build/playtapes/l01_right.tape`), 3000 frames,
`TLK_INTERP_HIST` in default mode and `--lle`; share = interpreted opcodes outside the idle vblank
spins `$C21A47-$C21A57` / `$C22352-$C22387` in default mode, divided by the same in `--lle`
(21,897,450).

| iter | change | roots | exact AOT / LLE variants | force_lle | non-idle interp share |
|---|---|---|---|---|---|
| task C | reference | 185 | 187 / 96 | 44 | n/a |
| it1-2 (earlier task G run) | coverage funcs + observed indirect_dispatch, H integration, `force_lle C084A0, C09BFD` | 461 | 635 / 151 | 18 | 56.0% |
| it2b | `$C1CCFE` exits proven (RAM callee `$7E2006` modelled) | 461 | 641 / 143 | 18 | 55.9% |
| it3 | merge_coverage on 45 play tapes (13 funcs, 19 indirect_dispatch), `force_lle C07189` | 472 | 684 / 171 | 19 | 55.6% |
| it4 | 162 constant `JMP ($00A4)` sites, `data_region C0 AD92 AD94`, `force_lle C0AF22` | 471 | 532 / 165 | 20 | 54.9% |

(it4 has fewer static AOT variants because the VM handlers were only discovered through `$C0AF22`'s
dispatch; at runtime they were interpreted before too, see below.)

### Changes (with evidence; each is commented in the cfg)

- `exit_mx_variant C1CCFE 0 0 0 0` / `0 1 0 1` (bankc1.cfg): the decompressor calls RAM code at
  `$7E2006` that it writes itself (`$C1CD35`: `LDA #$54 / STA $7E2006` = MVN, `LDA #$6B / STA $7E2009`
  = RTL). `check_exit_mx.py` now models that callee as width-preserving (`WIDTH_PRESERVING_RAM`); walk:
  PHP...PLP, M0X0->M0X0, M0X1->M0X1. Not declared for `$7E2006` itself: with it, `$C1CCFE` became
  AOT and default mode diverged at f600, because codegen silently drops a call to an unnamed RAM
  target (`/* Call: target $7E2006 not a valid LoROM code address ... skipped */`).
- it3 coverage: `scripts/coverage_runs.sh build/playtapes build/cov/it3`, all 45 runs completed;
  `tools/merge_coverage.py --apply build/cov/it3/*.json` (observed entries/targets only).
- Constant `JMP ($00A4)` sites (59 in bankc0, 103 in bankc2): every site has the ROM bytes
  `A9 lo hi 8D A4 00 6C A4 00` (`LDA #imm / STA $00A4 / JMP ($00A4)`), so its only target is
  `$bank:imm`. Pattern checked per site against the ROM. Covers `$C2895F` (-> `$C28936`).
- `data_region C0 AD92 AD94`: `$C0AD92 BRK #$AA` is the error trap for script ops `$80-$F6`
  in `$C0AD22`; it poisoned the routine. Now `$C0AD22` compiles.
- `force_lle C07189` (l07_random f5000 MISMATCH, sole bisect culprit), `force_lle C0AF22` (the
  script VM became AOT after it4 and froze the game before f600; it rewrites its caller's frame:
  `$C0AF4D` PLA the JSR return, PHX `#$C0`, PHA = a 3-byte RTL frame for its handlers).
- `$C0ABFB` (`PHK / PEA $ABFD / JML [$0014]`, object walker): tried as `ptrtail` (freeze by f1200,
  culprit `$C0ABBB`) and as `ptrcall return:ABFE frame:3` (run fails at f105). Left LLE-dynamic,
  both attempts commented in bankc0.cfg. Only 2 targets observed (`$C0B3CC`, `$C0FA58`).

New scripts: `scripts/interp_share.py`, `scripts/coverage_runs.sh` (all tapes in a dir, `--log`, in
parallel), `scripts/bisect_interp.py` (splits a run's `compiled_entry_pcs` with `--interp-targets`
until each culprit is a single entry). Play tapes: `tools/make_play_tapes.py` -> `build/playtapes/`
(14 levels x right/random/idle, attract, options, gameover).

### Where the remaining interpreted time goes (l01_right, 3000 f, non-idle)

| owner | share | why it stays interpreted |
|---|---|---|
| sound library `$C21924-$C223xx` (SPC upload/handshake loops at `$C22031`, `$C21EF8`) | ~38% | the 16 veneers are `force_lle` (PER+BRL analyzer gap, task C) |
| `$C1CCFE` decompressor + `$7E2006` | ~21% | calls RAM code; codegen cannot call an unnamed RAM target |
| script VM `$C0AF22` + handlers | ~11% | `force_lle` (frame rewrite); handlers are entered by JMP, never bounced |
| `$C0890A` main loop, `$C0ABBB`, `$C07958` | ~10% | AOT bodies exist, but the interpreter only bounces into compiled code at JSR/JSL; after a compiled body yields at the frame deadline the rest of that invocation (and the main loop, entered by JMP) runs on the interpreter |
| `$C08500` | ~3% | LLE: `BRK` at `$C085B3` (structural poison), unproven exit of `$C0B8B9` |

So cfg work has reached diminishing returns (56.0% -> 54.9%). The large items need framework/runner work:
(1) bounce into compiled code at a resume PC after a deadline yield, (2) a named host stub for
`$7E2006` (MVN + RTL) so `$C1CCFE` can compile, (3) an analyzer model for PER+BRL local calls.

### Open problems

1. The three framework items above.
2. `$C07189`, `$C084A0`, `$C09BFD` are kept interpreted without a root cause (all loading-time code
   around the decompressor; maybe interrupt timing, not proven).
3. WRAM hash differs at some matching frames (f600, f3000 no input; 6987M f1200); frames match.
4. `$C0ABFB` needs a correct call-through-pointer model (PHK/PEA/JML) before it can be authorised.

## 2026-10-12: decompressor `$C1CCFE` and loading loops at native speed (task K)

### Result

```
$ scripts/compare_modes.sh 600 3000
600    MATCH    0f1fa1dea806bc08   0f1fa1dea806bc08   differs     600/600
3000   MATCH    3e9d33c2e94735b1   3e9d33c2e94735b1   differs     3000/3000
$ scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 6000
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
$ scripts/compare_modes.sh -t build/playtapes/l07_random.tape 2500 5000
2500   MATCH    fe8be90285410f4c   fe8be90285410f4c   differs     2500/2500
5000   MATCH    fddd18ac7e5465bb   fddd18ac7e5465bb   same        5000/5000
$ scripts/compare_modes.sh -t build/playtapes/l01_right.tape 2500 4500 6000
2500   MATCH    d2a23a7de3af6bf7   d2a23a7de3af6bf7   differs     2500/2500
4500   MATCH    6d3b5d403468bc1d   6d3b5d403468bc1d   differs     4500/4500
6000   MATCH    8bd135ee33bed260   8bd135ee33bed260   differs     6000/6000
$ scripts/compare_modes.sh -t build/playtapes/l10_right.tape 3000 6000
3000   MATCH    45a873a0988a4ea5   45a873a0988a4ea5   same        3000/3000
6000   MATCH    933917305834fe43   933917305834fe43   same        6000/6000
$ scripts/compare_modes.sh -t build/playtapes/l04_right.tape 4500
4500   MATCH    d2a23a7de3af6bf7   d2a23a7de3af6bf7   differs     4500/4500
```

`scripts/interp_share.py` (l01_right, 3000 f, non-idle interpreted share): **54.9% -> 42.5%**
(default non-idle interpreted opcodes 12,011,951 -> 9,303,681; `--lle` reference 21,897,450).
With only the decompressor loop native it was 43.1%; `$C07189`/`$C084A0` loops add the rest.

### Options considered (evidence)

- `$7E2006` is written by `$C1CCFE` itself on every call (`$C1CD27-$C1CD3D`: `54 bb bb 6B` =
  `MVN bb,bb / RTL`, bb = `$0C` = DB after the `PLB` at `$C1CD26`); only caller `JSL $7E2006` at
  `$C1CDE9`. So "is it modified in RAM" is moot: it is rebuilt each call and only the bank bytes vary.
- **(a)/(b) via the framework's naming mechanism.** `name 7E2006 TlkRamMvnRtl` (bankc1.cfg) is enough
  for codegen to emit the call: with a cfg name the RAM target passes `_is_invalid_lorom_call_target`,
  and with no compiled variants every case becomes `interp_tier_run_call_frame(cpu, 0x7e2006, ...)`,
  i.e. the interpreter runs the real RAM bytes. No stub needed. The name line is kept.
- **But compiled `$C1CCFE` is not frame-exact.** With it compiled, `none f600` and `f3000` MISMATCH
  (title-screen sun cursor phase). Per-frame WRAM log: WRAM equal up to the end of the first
  decompression, then from f415 the frame counter `$F930` runs one ahead: the load finished one frame
  early. Cause: generated blocks charge `cycles x code-region speed` as one lump (FastROM: 6 master
  clocks for every cycle), while the interpreter charges each bus transfer at its region speed (WRAM
  and `$0000-$1FFF`: 8) plus 6 per internal cycle (`interp_bridge.c` step loop). The runtime even
  has `cpu_*_paced` accessors for exact AOT pacing, but this pin's emitter does not call them.
  A decompressor runs for many frames, so the difference accumulates into a lag-frame shift.
- **(c) whole-routine HLE** would have to reproduce that cycle model and yield at the same
  instruction as the interpreter to stay exact; a routine-level C rewrite cannot do that.

### Chosen: native execution of the loop at interpreter timing

`runner/tlk_decomp.c` registers interpreter pre-opcode hooks (public API, as task E's poll hook) at
loop heads. The hook executes the routine's own bytes natively (a small 65816 executor for the ~60
opcodes these loops use, same semantics as `interp816.c`) and stops at the first instruction
boundary where the interpreter step loop could stop (master deadline, IRQ with I clear) or when the
PC leaves the region, then redirects the interpreter there. Bus accesses and the per-opcode tail
(cycle charge, DRAM refresh, beam/coprocessor sync, APU pending and progress flushes, write/dynamic
epochs) go through the bridge's own code, exported by a runtime hunk
(`cmake/runtime-patches/interp_bridge.hunks`: `master-deadline-getter`,
`hook-frame-auto-quiescent-*`, `native-opcode-exports`; no behaviour change for the interpreter).
A first version that charged cycles itself but skipped the bridge's APU bookkeeping was not exact
(sound-driver counter `$1F08` and stack garbage differed from f34), which is why the bridge's own
tail is used.

Regions: `$C1CD43-$C1CE7E` + `$7E2006-$7E2009` (hook `$C1CD75`), `$C07199-$C071D8` and
`$C07209-$C072AA` (hooks `$C07199`, `$C071C0`, `$C07209`), `$C084A0-$C084C2` (hook `$C084A0`).
`$C1CCFE` is now `force_lle` (bankc1.cfg), so all four stay interpreted at their entries.
On by default in recompiled mode, off under `--lle`; `TLK_NATIVE_DECOMP=0/1` forces it.

**Equivalence proof** (`scripts/check_native_decomp.sh`): `--lle` with the native loops forced on vs
off, per-frame WRAM and VRAM hash log (`TLK_FRAME_LOG`, new in `headless_main.c`) and final frame
hash. IDENTICAL on every frame for all 14 `lNN_right` tapes (4500 f), `l07_random` and 6987M (6000 f)
and no input (3000 f); e.g.
`IDENTICAL frames=4500 tape=build/playtapes/l01_right.tape native_ops=4419123 ... wram_frames_differing=0`.

### Re-bisect of `$C07189`, `$C084A0`, `$C09BFD`

All three `force_lle` removed in one build, then pushed back one at a time with `--interp-targets`:
none f3000 MISMATCH needs `$C09BFD` compiled; l07_random f5000 MISMATCH appears with any one of the
three compiled (same wrong hash `593865d7...`, a one-frame shift). Root cause for all three is the
cycle model above, not a translation bug:

- `$C084A0` (32x32-word copy): `SNESRECOMP_CYC_WATCH=C084A8-C084B5` gives 52+40+4x12+18+18 = 176
  master clocks per inner iteration interpreted; the compiled block charges 26 cycles x 6 = 156 (+6
  when the branch is taken). 1024 iterations per call.
- `$C07189` (level-load init): `STZ $2000,X` WRAM clear, 28,671 iterations per call, 40 master
  interpreted vs 36 compiled per store, plus table builders: ~100k master clocks (a third of a
  frame) cheaper per call when compiled.
- `$C09BFD` (called ~once per frame while scrolling, 383 calls / 3000 f): `TLK_FRAME_LOG` shows VRAM
  diverging at f2537 and the next load starting one frame early (`$0A0A`=1 at f2549 vs f2550).
  A lag frame moved.

Fix: `$C07189` and `$C084A0` stay `force_lle` with their hot loops in `tlk_decomp.c`; `$C09BFD` stays
`force_lle` (little time, no loop worth it). The cfg comments carry this evidence.

### Files

- `runner/tlk_decomp.c` (new), `runner/tlk_game.c` (`.initialize` installs the hooks),
  `CMakeLists.txt` (adds `tlk_decomp.c`).
- `cmake/runtime-patches/interp_bridge.hunks`: three hunks appended (exports only).
- `recomp/bankc1.cfg`: `name 7E2006`, `exit_mx_variant 7E2006`, `force_lle C1CCFE`;
  `recomp/bankc0.cfg`: root-cause comments on the three `force_lle`.
- `runner/headless_main.c`: `TLK_FRAME_LOG=<path>` (frame, WRAM FNV-1a, `$0A0A`, VRAM FNV-1a) and
  `native_decomp_ops=` in the summary.
- `scripts/check_native_decomp.sh` (new).

## 2026-10-12: exact cycle pacing for compiled code (task M)

### Result

All gates MATCH with every timing-only `force_lle` removed (`$C1CCFE`, `$C07189`, `$C084A0`, `$C09BFD`,
`$C21EBF`, `$C22013`; only `$C0AF22`, the script VM frame rewrite, stays):

```
$ scripts/check_all_tapes.sh
play tapes @f5000: 45 MATCH, 0 MISMATCH
600    MATCH    8f9b57ae947b411f   8f9b57ae947b411f   same        600/600
3000   MATCH    afdcc57d304f37bf   afdcc57d304f37bf   differs     3000/3000
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
ALL MATCH
$ scripts/check_native_decomp.sh -t build/playtapes/l01_right.tape 3000
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2739553 frame=d2a23a7de3af6bf7/d2a23a7de3af6bf7 wram_frames_differing=0
```

Stronger than frame hashes: with `TLK_HWLOG` (below) the full sequence of hardware-register accesses
(address, master clock, SPC clock at APU-port accesses) is identical between `--lle` and default mode for
5000 frames of `l02_idle` (5,256,082 accesses) and `options`, and 600 frames of `l07_random`.

`scripts/interp_share.py` (l01_right, 3000 f): 41.9% before (tasks G-K) -> 40.7%. Wall time of a 5000-frame
`l01_right` run: 9.97-10.04 s before, 10.19-10.23 s after (3 runs each, +2%).

The `--lle` oracle itself changed (no-input f600/f3000 hashes were `0f1fa1de`/`3e9d33c2`, now `8f9b57ae`/
`afdcc57d`; 6987M unchanged), because three runtime fixes below also apply to the interpreter (SPC pacing after
the first port write, the stable-poll detector, beam sync before register access). They make timing a
function of the master clock alone instead of where interpreter frames start and stop.

### The 14 mismatching tapes (before)

`scripts/first_divergence.py` (fixed: it read nonexistent columns) at f5000, task K build. WRAM differs from
f29 on every tape (stack garbage, harmless), so the first VRAM difference and the first `$0A0A` difference are
the useful columns:

| tape | first VRAM diff | first `$0A0A` diff (lle/aot) | class |
|---|---|---|---|
| l02_idle, l02_random, l02_right | 1473 | 1487 (0000/0001) | timing: level load ends a frame apart |
| l03_idle | - | - | timing (WRAM only by f5000) |
| l04_right | 1532 | 4442 (0ADF/0ADE) | timing |
| l07_right | 1600 | 4073 (0934/0935) | timing |
| l09_idle, l09_right | 1666 | 1692 (0000/0001) | timing: load |
| l11_idle / l11_random / l11_right | 1668 | 4318 / 1798 / 1755 (one frame apart) | timing |
| l12_right | 1691 | 1974 (0000/0001) | timing: load |
| l13_random / l13_right | 1716 | - / 4362 (0A54/0A53) | timing |

WRAM dumps for l02_right: equal at f1300, then the sound driver counter `$1F08` one off at f1400, the
decompressor's progress (`$0012`, `$1003F`..) differing at frame ends during the next load (f1340-1356), then
a lag frame. No logic difference was found; every tape matches after the pacing work.

### What made compiled code inexact (each found by diffing hardware-access logs, fixed in this order)

1. **Cycle model.** Blocks charged `cycles x code speed` as one lump at block entry. Now every instruction is
   charged after its effects with exactly what the interpreter charges: `sum(region speed of each bus byte) +
   6 x max(0, cycles - bus bytes)`, with interp816's cycle counts (D.l, index page-cross, taken branch, MVN per
   byte). `recompiler/v2/aot_pace.py` describes each opcode's bus bytes (fetches, data, pointers, stack,
   vectors) as expressions over the pre-instruction state; `tools/aot_pace/verify.py` checks it against
   `interp816_runOpcode` itself on random states (203,200 samples, 0 mismatches; the cycle table is derived
   from the core). BRA's taken cycle is in its base (it is a Goto, not a CondBranch).
2. **Boundary granularity.** The interpreter, after each opcode, charges DRAM refresh, walks the beam (which
   latches H/V IRQs) and then samples deadline and IRQ. Compiled code now does the same at every instruction
   boundary through `aot_pace_point()` (inline add + compare); the slow path `aot_pace_event()` runs only when
   the master clock reaches the next refresh line, the armed deadline or the next IRQ latch, or the pending SPC
   time reaches its flush threshold. A due deadline/IRQ yields at that boundary (`interp_bridge_lle_yield_unwind`
   to the next PC). Blocks that the emitter fuses are handled: no mid-block yield in NLR/trampoline blocks; a
   yield between `PER` and `BRL` (patch 0001's local call) first pushes what PER pushes; an unresolved indirect
   JMP that the interpreter re-executes is refunded. A beam parked exactly on the IRQ comparator latches on the
   next clock (the framework's `snes_master_clocks_until_irq` says one field later), which the slow path accounts
   for (l02_idle f1489: IRQ taken one instruction late).
3. **SPC pacing.** The interpreter feeds every opcode's master clocks into `s_apu_pending_master` and flushes it
   into the SPC; compiled code did not (SPC 13 cycles behind at frame 4). Now it does (taken branches too), and
   flushes before APU-port accesses. Also: after the first CPU port write the bridge now syncs the SPC to the
   absolute guest clock only (the relative catch-up added on top of an absolute sync depended on flush points,
   which differ when the same code runs in a nested frame: one SPC cycle apart at the same master clock), and
   pending master clocks convert to SPC cycles with an exact integer ratio.
4. **Parking.** In `--lle` the NMI wait `$C537B2` runs in the outermost frame, whose stable-poll detector parks
   the CPU a scanline at a time. In default mode it runs in a nested frame under a compiled caller and spun
   instead (frame end 8 clocks later). The detector now also runs in nested frames (handing the block outward
   like H's deadline hand-off), and it counts repeats against the most recent equal state; it used to match the
   first ring slot, so the number of laps before parking (3 to 32) depended on the frame's step counter.
5. Beam sync before every hardware-register access (it used to lag behind compiled code), with APU flush for
   APU ports.

### Files

- `patches/snesrecomp/0003-aot-exact-pacing.patch` (codegen): `recompiler/v2/aot_pace.py` (model + C
  rendering, `SNESRECOMP_AOT_PACE=0` turns it off), `aot_pace_table.py`, `emit_function.py` (per-instruction
  charge/yield points, no block lump, refund), `codegen.py` (MVN/MVP one byte per step), `emit_bank.py`
  (includes `aot_pace.h`).
- `runner/aot_pace.h`, `runner/aot_pace.c` (new): inline charge, slow path, diagnostics. `CMakeLists.txt` adds
  them; `runner/tlk_game.c` calls `aot_pace_diag_init` / `aot_pace_diag_frame`.
- `cmake/runtime-patches/interp_bridge.hunks` (appended): yield guards + deadline-unwind setter, next-event
  invalidation, shared `s_apu_pending_master` + flush export, exact SPC ratio, absolute SPC after anchor, the
  stable-poll detector (shared outer/nested, most recent match), CYC_WATCH `master=`/`start=`, resume log.
  New: `cmake/runtime-patches/cpu_state.hunks` (beam sync + APU flush + cache clear at each register touch),
  `cmake/runtime-patches/common_cpu_infra.hunks` (next refresh clock).
- `recomp/bankc0.cfg`, `bankc1.cfg`, `bankc2.cfg`: the six `force_lle` lines commented out (`# task M`).
- `scripts/first_divergence.py` (column fix), `tools/aot_pace/` (model check, divergence finder).

### Diagnostics

- `TLK_HWLOG=<path> [TLK_HWLOG_FROM=<frame>]`: one line per hardware-register access (`frame master addr ipc=
  fn= apu=portClock/anchors`), per frame start, per resume/unwind; `TLK_HWLOG_FLUSH=1` adds SPC flushes.
  `tools/aot_pace/hw_diff.py` finds the first access that differs.
- Build with `-DAOT_PACE_TRACE` (`cmake -S . -B <dir> -DCMAKE_C_FLAGS="-D_GNU_SOURCE -DAOT_PACE_TRACE"`) to
  also log every compiled instruction and slow-path event; `tools/aot_pace/pace_diff.sh TAPE FRAMES BIN` runs
  both modes and prints the first opcode whose start clock differs (with `SNESRECOMP_CYC_WATCH`, which now
  prints `master=` and `start=`).

### Open problems

1. The emitter's other cycle paths (the old lump, `_dynamic_charge_lines`) remain for `SNESRECOMP_AOT_PACE=0`;
   with pacing on, `$C1CCFE` and `$C07189`/`$C084A0` are compiled, so `runner/tlk_decomp.c`'s native loops only
   run under `--lle` now (still checked by `check_native_decomp.sh`).
2. Not exact by construction (not observed): an IRQ latched during a JSR/JSL whose callee is interpreted is
   sampled after the callee's first opcode (the interpreter does not check IRQs at step 0 of a frame).
3. The SDL host's audio thread also runs the SPC; the absolute-after-anchor change was not listened to.
4. `--lle` is not pure interpretation: interrupt entries and nested frames still bounce into compiled bodies
   (`SNESRECOMP_LLE_BOUNCE=0` only gates the outer scheduler), so pacing changes also move the oracle.

## 2026-10-12: task I (bsnes timing parity) merged on top of task M (task N)

### Result

```
$ scripts/compare_modes.sh 600 3000
600    MATCH    57ccb8d5b24918fd   57ccb8d5b24918fd   same        600/600
3000   MATCH    89a48d6c4f12621b   89a48d6c4f12621b   differs     3000/3000
$ scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 6000
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
$ scripts/check_all_tapes.sh -s
play tapes @f5000: 45 MATCH, 0 MISMATCH
$ scripts/check_native_decomp.sh -t build/playtapes/l01_right.tape 3000
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 frame=d2a23a7de3af6bf7/d2a23a7de3af6bf7 wram_frames_differing=0
```

Full TAS replay against BizHawk (`--input-polled build-host/out/sync/ram/tapes/<t>.read.tape`, `tools/ram_sync_compare.py`
against `bh_ram_<t>.txt`; logs `build-host/out/sync/ram/hl_lle_<t>.txt`, `hl_aot_<t>.txt`):

| tape | `--lle` | default (recompiled) |
|---|---|---|
| 6987M | in sync through poll 36904 (movie end) | 36904 |
| 6781M | 37093 (movie end) | 37093 |
| 4735M | 38870 (movie end) | 38870 |
| 2716M | **desync at poll 1775** (f2090, x/y) | **desync at poll 1775** |

The no-input `--lle` hashes changed again (f600/f3000 `57ccb8d5`/`89a48d6c`); 6987M's did not.

### What was merged

`TLKRecomp-i/INTEGRATE.md` as written: `apu`, `spc`, `common_rtl`, `snes`, `beam_frame_driver` hunks copied; I's
`interp_bridge` port bus-time hunks appended (its `interp-apu-flush-absolute-timeline` is M's
`aot-pace-absolute-after-anchor`, kept once); I's `dram-refresh-at-beam-position` appended to `common_cpu_infra.hunks`;
`headless_main.c` diagnostics (`TLK_TRACE_OPCODES`, `TLK_ARAM_DUMP`); `tools/compare_opcode_traces.py`,
`tools/compare_lags.py`, `tools/bizhawk/trace_opcodes.lua`. The SPC ratio needed no change in `runner/aot_pace.c`: compiled
and interpreted code share `bridge_apu_flush` and, after the port anchor, the absolute guest clock (I's NTSC ratio).
Before the anchor (IPL boot) the relative path keeps M's exact 1025280/21442080 (same as I's tree's double).

### Fixes needed to keep default mode == `--lle` (each found with `TLK_HWLOG` / `tools/aot_pace/pace_diff.sh`)

1. **Refresh predictor** (`common_cpu_infra.hunks` `refresh-next-export`): `tlk_refresh_next_master()` now predicts I's
   next H=538 crossing (a few clocks early; an early slow path only re-predicts).
2. **Refresh across the short line** (`tlk_refresh_h_at` / `tlk_refresh_to_next`, used by both the predictor and I's
   hunk; `snes.hunks` `short-line-export`). I's hunk projected H from the beam with `% 1364`. The interpreter syncs the
   beam every opcode, so that was exact there; compiled code walks the beam lazily, and across line 240 of a short field
   (1360 clocks) the projection was 4 clocks off and a refresh was skipped (l07_right f608 `$C0B925`: 40 clocks, then
   MISMATCH at f5000). H is now stepped across line boundaries with the real line lengths.
3. **APU-port accesses in compiled code at bus time** (`patches/snesrecomp/0004-aot-apu-port-bus-time.patch`,
   `cpu_state.hunks` `aot-apu-port-bus-time-*`, `interp_bridge.hunks` `aot-apu-port-bus-time-export`). I times an
   interpreted `$2140-$217F` access at its own bus cycle. Compiled code charges after the instruction, so its accesses ran
   at instruction start. The emitter now stores the instruction's bus clocks (fetches + accesses, no internal cycles) in
   `g_aot_bus_mc` before each memory instruction, and `cpu_read8/16`, `cpu_write8/16` offset APU-port accesses by it with
   I's leads (read 4, word read 10, word write 6), unless the access comes from the interpreter.
4. **SPC sync without pending clocks** (`interp_bridge.hunks` `aot-apu-flush-absolute-always`). `bridge_apu_flush`
   returned early when nothing was pending; periodic flushes reset that counter at tier-dependent points, so the SPC
   position seen by a port access depended on flush history (l07_right f40: 692847 vs 692848 at the same master clock).
   Once anchored it now always syncs to the absolute clock.
5. **`force_lle C07624`** (`recomp/bankc0.cfg`). `$C07624` dispatches through a pushed frame (PHA / PHK / PEA $768E /
   PHA / RTS). Compiled, the RTS took the return-to-ancestor path, the frame was lost and the handler's RTL returned to
   `$C389E4` with X=1: BRK at `$C389FB`, default mode only, 6987M/6781M/4735M at level 8 (~f33500). Not a timing issue;
   default mode had never got that far before.

### 2716M poll 1775 (open)

Task I measured 2716M in sync to the end, but in its tree the NMI subroutines (`$C09EC6` and others under `$C09E0C`)
ran compiled even under `--lle`, with the old lump cycle charge. With M's exact pacing they cost what the interpreter
charges (e.g. `JSR $9EC6` at `$C09E8E`: 280 vs 266 clocks), and 2716M desyncs at poll 1775 in both modes. Check: a copy
of task I's tree with `force_lle C09EC6` (plus the NMI functions) also desyncs at 1775, so this is not a merge error; I's
2716M result depended on that inexact timing. Things that did not fix it: DSP start phase 0-31 (with pacing on, 2716M
fails at 1775 for every value tried; over the first 2400 frames 6987M stays in sync only for 4, 23, 25, 26, 27), reverting M's beam sync at register
access, M's stable-poll detector, the pre-anchor exact ratio, and the six task-M `force_lle` removals. Next: BizHawk
opcode traces of 2716M polls 1700-1775 against `TLK_TRACE_OPCODES`. In task N's BizHawk runs `trace_opcodes.lua` logged
`emu.totalexecutedcycles()` = 0 for every opcode, so fix that first (core setting?).

### Other notes

- Regeneration: the emitter caches by input, not by `SNESRECOMP_AOT_PACE`; delete `generated/snesrecomp` when toggling it.
  After a full regeneration banks C1/C4/C5 come out sharded (`*_partNN_v2.c`); harmless.
- `g_aot_bus_mc` costs one store per memory instruction (not timed).

## 2026-10-12: playable SDL host on the headless path (task P)

`tlk_sdl` used the framework desktop host (`snesrecomp/runner/src/desktop/host_main.c`). That host calls
`RtlEnableExtendedFrameTiming()`, which switches the APU to the framework's extended frame clock and so bypasses TLK's
NTSC SPC ratio hunks (`common_rtl.hunks`: they apply only to the non-extended path). It also brings fast-forward,
run-ahead, rewind and mods into the frame path.

Fix: `runner/sdl_main.c` is now a small SDL3 front end over exactly the headless sources. In `CMakeLists.txt` `tlk_sdl`
builds `TLK_COMMON_SOURCES` + `headless_host.c` + `verified_rom.c` + `sdl_main.c`; no desktop host, no mod catalog. One
thread: emulate (`RtlRunFrame`, beam frame driver), draw, queue audio, present, wait. No runtime hunk changed.

- Pacing: wall-clock deadline at 21477272/357366 = 60.0988 Hz (`SDL_DelayPrecise`), or `vsync=1` in the ini.
- Audio: `RtlRenderAudio` resamples the DSP's 32040 Hz output to the device rate (48000 default) with its own ring servo;
  the host queues rate/60.0988 frames per field into an SDL3 audio stream, trimmed by up to +/-0.5% to hold the device
  queue at ~3 fields (~50 ms).
- Proof options, same meaning as `tlk_headless`: `--input`, `--input-polled`, `--lle`, `--interp-targets`, `--frames`,
  `--dump-png`; plus `--hash-at a,b`, `--no-pace`, `--no-audio`, `--wav path --wav-seconds S`.

Results (`xvfb-run`, `SDL_AUDIODRIVER=dummy`, `--no-pace`), frame and WRAM SHA-256 identical to `tlk_headless`:

| run | frame_sha256 (both) | wram |
|---|---|---|
| 6987M f1200 | `dd861e164c29...` | same |
| 6987M f6000 | `946033bd1068...` | same |
| no input f600 | `57ccb8d5b249...` | same |
| no input f3000 | `89a48d6c4f12...` | same |

Real time (pulse audio, 6987M tape, 3600 frames): `wall_s=59.892 fps=60.1079 audio_underruns=0 late_resyncs=0`
(expected 59.90 s; the first frame does not wait). f1200 hash still identical with audio on. WAV of the queued audio,
48 kHz, 2875278 frames (3600 x 798.68): per-second RMS 1000-2800 with peaks 5000-18700, silence only at second 0 and
13-14 (level transition); sample-to-sample diff RMS / RMS = 0.046 (white noise would be ~1.41), so it is tonal, not
garbage. Nobody has listened to it yet.

Not done: savestates, SRAM (TLK has none), rebinding gamepad buttons (keyboard only via ini).

## 2026-10-12: 2716M poll 1775 (task O, not fixed)

### Result

2716M still desyncs at poll 1775 (f2090) in both modes. The other three stay in sync to movie end. No default changed;
only diagnostics were added. Gates (this tree):

```
$ scripts/compare_modes.sh 600 3000
600    MATCH    57ccb8d5b24918fd   57ccb8d5b24918fd   same        600/600
3000   MATCH    89a48d6c4f12621b   89a48d6c4f12621b   differs     3000/3000
$ scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 6000
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
$ scripts/check_all_tapes.sh -s
play tapes @f5000: 45 MATCH, 0 MISMATCH
$ scripts/check_native_decomp.sh -t build/playtapes/l01_right.tape 3000
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 frame=d2a23a7de3af6bf7/d2a23a7de3af6bf7 wram_frames_differing=0
full TAS (ram_sync_compare.py, logs build-host/out/sync/ram/hl_{lle,aot}_<t>.txt), same in --lle and default:
6987M in_sync_through_poll=36904   6781M 37093   4735M 38870   2716M first_diff poll=1775 (f2090, x/y)
```

### Findings

1. **2716M is checked against a different emulator.** Its `.bk2` header says `Core BSNES` with
   `LibsnesCore ... "Profile":"Compatibility"`, so BizHawk 2.11.1 replays it on bsnes v0.87 (libsnes), not on
   bsnes v115 (`BSNESv115+`) like 6987M, 6781M and 4735M. Every timing value fitted to the other three (DSP phase 25,
   SPC rate) is fitted to v115. 2716M needs v0.87's timing, which nothing here models yet.
2. **`trace_opcodes.lua` clock fixed.** `emu.totalexecutedcycles()` is 0 on both SNES cores. The libsnes core exposes
   `V` and `H` (H in dots 0..340) via `emu.getregister`. The script now builds the clock from them: 4 clocks per dot
   (6 for dots 323/327), deltas summed, field wrap +357366. Resolution is 4 clocks. `TRACE_RAW=1` logs `PC V H`.
   I did not check whether the BSNESv115+ core exposes V/H.
3. **Opcode costs match.** Over polls 1650-1660 and 1766-1776 the per-opcode clocks match to the 4-clock resolution
   (streaming upload `$C21F7C` pair loop: 948-949 clocks per pair on both sides). Headless `g_cpu.master_cycles`
   leaves out DMA time that the beam walks. NMI-to-NMI is about 356,920 master clocks in headless and 357,366 in
   the BizHawk beam clock, so compare clocks inside a frame, not across NMIs.
4. **Where it splits.** BizHawk's poll counter runs one ahead of headless in these windows (BizHawk frame i+1 =
   headless frame i). Per frame, the CPU-SPC command handshake (`$C22352` spin) waits about the same number of
   iterations (342/342, 320/319, 329/330) until one frame: 369 in headless vs 401 in BizHawk (polls ~1770). Lag frames
   land one poll earlier in headless at polls 1658, 1697, 1712 and 1734 (`compare_lags.py`). WRAM dumps
   (`TLK_WRAM_DUMP` vs a System Bus/APURAM dump script) are equal apart from the long-standing stack and uninit
   areas through f2006. At f2007 (poll 1697 lag) a game timer `$0AA9` is set one frame earlier in headless (set at
   `$C0FB94`, decremented in NMI at `$C09EB9`). It stays one frame off, gameplay WRAM splits at f2048, and the hit
   at poll 1774 follows. So the cause is SPC response latency, not CPU opcode timing.
5. **Knobs tried, all fail at 1775 in `--lle`:** `TLK_SPC_HZ` 1025280 (v115: 32040x768/24) and 1025296
   (v0.87: 32040.5x768/24), each with `TLK_DSP_PHASE` 0..31 (64 runs). The RAM log did not change between the two
   rates. Both env overrides are new and diagnostic only (`common_rtl.hunks`, `apu.hunks`). Defaults are unchanged.

### Next

Model bsnes v0.87's SMP/DSP, or at least find which handshake reply differs: trace both sides around poll 1697 and
compare SPC state. The libsnes core exposes the `APURAM` domain, so ARAM can be dumped per frame (the scratch
dump script used here does that) against `TLK_ARAM_DUMP`. A per-movie timing profile (v115 vs v0.87) is probably
needed. One fitted model is unlikely to cover both cores.

## 2026-10-12: SDL window was black

Owner saw a black window. The emulation was fine (frame dump = title screen); the texture was `ARGB8888` with
default blending and the PPU leaves the top byte 0, so every pixel drew transparent. Task P's checks hashed the
pixel buffer, never the window. Now `XRGB8888` + `SDL_BLENDMODE_NONE`; checked with a screenshot of the window
under Xvfb (x11 driver): title screen visible.

## 2026-10-12: compiled script VM, object walker, `$C07624`, and no more interpreted tails after a yield (task Q)

### Result

`scripts/interp_share.py` (l01_right, 3000 f, non-idle interpreted share) and l01_right 5000-frame wall time
(3 runs each, this machine):

| step | share | wall (5000 f) |
|---|---|---|
| start (task N tree, rebuilt here) | 42.2% | 11.79-11.88 s |
| 1. APU-port polls stay compiled (codegen) | 38.4% | - |
| 2. fibers: a yield no longer drops the compiled chain (runtime) | 16.3% | 5.44-5.46 s |
| 3. script VM `$C0AF22` compiled (cfg + codegen) | 14.7% | 5.40-5.42 s |
| 4. `$C07624` compiled (codegen) | 14.7% (level 8 code) | - |
| 5. object walker `$C0ABFB` as ptrcall (cfg) | **11.7%** | **5.22-5.27 s** |

Every step: `compare_modes.sh 600 3000` and `-t 6987M 1200 6000` MATCH, `check_all_tapes.sh -s` 45 MATCH, and the full
6987M TAS in default mode in sync through poll 36904 (from step 3 on also 6781M 37093 and 4735M 38870, all to movie
end). `--lle` hashes are unchanged (`57ccb8d5`/`89a48d6c`/`dd861e16`/`946033bd`), `check_native_decomp.sh` IDENTICAL.
`force_lle` left in bankc0.cfg: none.

### 1. APU-port poll loops (patch 0005, `emit_function.py`)

`has_lle_memory_poll` made any function containing a 1-2 instruction read+branch loop yield at entry whenever the
scheduler runs, so it was always interpreted. That exists for the stable-poll detector (parking). A loop that only
reads `$2140-$217F` (abs, or long into a system bank) is never parked: APU-port reads are "continuous" reads. Such
functions now stay compiled (`$C21EBF`, `$C22013`, `$C22352`, ...: the SPC upload/handshake).

### 2. Fibers (`runner/tlk_fiber.c`, runtime hunks, patch 0005)

The real cause of most interpreted time: when compiled code reached the frame deadline or an IRQ, the bridge unwound
every compiled host frame and the scheduler resumed by INTERPRETING from the yield PC, so the rest of every routine
on that call chain ran on the interpreter (the SPC upload loop `$C22029` alone was 18%; also the main loop, the
decompressor, ...).

Now `interp_bridge_run_until_quiescent` runs on its own C stack (ucontext, 64 MB). At a due yield, compiled code calls
`tlk_fiber_yield(cpu, pc)` (generated: `if (aot_pace_point(...) && tlk_fiber_yield(cpu, PC)) {old unwind}`, same for
block/function-entry deadline checks and MVN); nested interpreter frames do the same at the task H deadline/IRQ
hand-off and the task M stable-poll hand-off. The fiber publishes the resume PC exactly as before and switches to the
driver, which does what it always did (IRQ/NMI handler, park, end of field) and calls `run_until_quiescent(PC)` again.
If PC, S, PB and m/x/e are what the fiber stopped at, it continues in place; otherwise it is told to abort and its
yield point takes the old unwind path. The machine state at a switch equals the state at the old unwind (same
instruction boundary), so only the tier that runs the continuation changes; exact pacing (task M) makes that
invisible. Interrupt handlers run on the driver stack on top of the suspended chain (as on hardware); the bridge's
per-run statics and the recomp call stack (`g_recomp_stack*`, entry-S slots, tail-call context, `host_return_valid`)
are saved/restored at every switch (`tlk_bridge_ctx_*`, `tlk_infra_ctx_*`). A memory-poll routine called from
compiled code on the fiber runs in a nested interpreter frame (`tlk_fiber_poll_entry`) instead of unwinding.

Needed with it (runtime hunks, `interp_bridge.hunks`):
- **Nested-frame abandon rule**: a nested interpreter frame under the scheduler whose S rises above its entry
  watermark without a return hands the guest to the scheduler frame (unwind sentinel, owner = scheduler depth). The
  object handlers jump straight into the level loader (S back at `$02FF`); before, the next deadline unwind hid it,
  with fibers the nested frame lived on and its next RTS "returned" to the walker (6987M f2530 crash).
- **Fresh-frame reset**: after a switch, interpreter frames below it reset their stable-poll rings and step budget
  (the outer frame now lives for the whole game; the 2M step cap would bail).
- **Boundary settle** (`tlk_aot_pace_settle`): an indirect JMP/JML dispatch is charged without its boundary event; an
  interpreted target used to start its first opcode before a due DRAM refresh (6987M f1394, `$C0777C` -> `$C15B87`,
  40 clocks, desync at poll 1785). Now settled at nested-frame entry and in `interp_bridge_lle_yield_unwind`.

`TLK_FIBER=0` turns fibers off; `TLK_FIBER_OFF=aot,interp,poll` disables single switch kinds; `TLK_FIBER_STATS=1`
prints switch/resume/abort counts; `TLK_FIBER_LOG=<frame>` logs every suspension. Off under `--lle`.

### 3. Script VM `$C0AF22`

Three faults, one cfg model:
- Codegen read the pointer of `JMP (abs)` from `PB:abs` (`codegen.py` ABS_INDIRECT and the value-matched dispatch).
  The 65816 reads bank `$00`; in HiROM bank `$C0`, `$C0:00A4` is ROM. All 162 `LDA #imm / STA $00A4 / JMP ($00A4)`
  sites in the handlers dispatched on ROM bytes (latent until the handlers compiled; this was the f425 freeze).
- `indirect_dispatch AF52 ... ptrcall return:AF18 frame:3` instead of `ptrtail`: `$C0AF4D` turns the JSR frame into a
  3-byte RTL frame back to `$C0AF18`, so the handler call is a long call whose continuation is `$C0AF13`'s epilogue
  (decoded into `$C0AF22`, which leaves through the compiled-ancestor SKIP_1).
- An unlisted pointer on a call-form dispatch used to be skipped (frame unpopped, handler never ran). Codegen now
  runs it on the pushed frame (`cpu_dispatch_call_pc_pushed`: compiled body or interpreter). `$C2888C` was missing
  from the 166-target list (6987M f1394).
- Exit widths: `exit_mx_variant C0AD22 0 1 0 1` (manual: PHP ... PLP/RTL, op handlers all return to `$AD4E`; the
  walker stops at the data-regioned BRK) and `C0AE42 0 1 0 1` (`check_exit_mx.py`).
- `aot_pace_table.py`: WAI/STP cost (3) was missing; a handler contains WAI.

### 4. `$C07624` (pushed-frame RTS dispatch)

`PHK / PEA $768E / LDA $C587BC / DEC / PHA / RTS` calls a handler that RTLs to `$C0768F`. The compiled RTS popped a frame
the routine pushed itself (pre-RTS S below entry S) and called `interp_tier_dispatch_popped_return`, which interprets
only until the target's first return, so the handler's RTL to `$C0768F` ended the frame and the rest of `$C07624` was
skipped (level 8 crash). Codegen: in that case, with a paired caller, `interp_tier_dispatch_balanced` (interpret until
the routine itself returns past its entry S). General for every RTS/RTL through a self-pushed frame to a non-entry.

### 5. Object walker `$C0ABFB`

`indirect_dispatch ABFB 2 ptrcall targets:C0B3CC,C0FA58 return:ABFE frame:3` enabled (failed at f105 / froze before);
with 2-3 above in place it passes every gate. Unlisted handlers run through the new call-form default.

### Where the remaining 11.7% goes (l01_right)

`$C085xx` 2.1% (top-level state code, LLE: BRK at `$C085B3`, reached by JMP), `$7E2006` MVN 1.3% (RAM routine,
interpreted in a nested frame), `$C09E0C` NMI head 0.6% (the vector's `JML` lands interpreted), handler pages
`$C0B1-$C0B7` ~2%, main loop `$C088xx` 0.4%. JMP/JML arrivals still never bounce into compiled code; that is the next
general item (tail entry into a compiled body from the interpreter), plus a host stub for the 4-byte `$7E2006`.

### Files

- `patches/snesrecomp/0005-fibers-dispatch.patch` (codegen: APU-poll exception, fiber yield guards, poll
  entry, JMP (abs) bank `$00`, call-form default runs the live pointer, self-pushed-frame RTS -> balanced, WAI/STP cost).
- `runner/tlk_fiber.c` (new), `runner/aot_pace.c` / `.h` (settle, declarations, trace shows S), `CMakeLists.txt`.
- `cmake/runtime-patches/interp_bridge.hunks` (appended: fiber context export, run_until_quiescent intercept, nested
  yield/quiescent switches, frame reset, abandon rule, scheduler owner depth, settle), `common_cpu_infra.hunks`
  (appended: call-context export).
- `recomp/bankc0.cfg` (`force_lle C0AF22`/`C07624` removed, AF52 ptrcall, ABFB ptrcall, two exit hints).
- `runner/headless_main.c`: `TLK_TRAP_PC=<pc24> [TLK_TRAP_FROM=<frame>]` dumps the interpreter's recent steps, edges and
  resume points (plus registers and stack top) the first time that PC is interpreted.
- `tools/aot_pace/pace_diff_polled.sh`: `pace_diff.sh` for `--input-polled` tapes.

### Open

- `check_exit_mx.py` reports `$C0AD22` UNPROVEN (data-regioned BRK); the hint rests on the manual proof above.
- The abandon rule also fires for rewritten-return continuations that pop above their watermark (correct, interpreted).
- 2716M (poll 1775, task O) not rerun here.

## 2026-10-12: JMP/JML entry into compiled code (task R)

### Result

| step | share (l01_right 3000 f) | wall (l01_right 5000 f, 3 runs) |
|---|---|---|
| start (task Q tree) | 11.7% (2602119 non-idle interp ops) | 5.22-5.27 s (task Q) |
| 1. interpreter enters compiled bodies at JMP/JML targets | 11.7% (2593709) | 5.17-5.19 s (`TLK_JMP_BOUNCE=0`: 5.16-5.20 s) |

Gates after step 1: `compare_modes.sh 600 3000` MATCH (`57ccb8d5`/`89a48d6c`), `-t 6987M 1200 6000` MATCH
(`dd861e16`/`946033bd`), `check_all_tapes.sh -s` 45 MATCH, `check_native_decomp.sh` IDENTICAL, default-mode TAS sync
6987M 36904, 6781M 37093, 4735M 38870 (movie ends). `--lle` hashes unchanged (the bounce is off under `--lle`).

The mechanism works but moves almost nothing: the hot interpreted code is not reached through JMP into a compiled
body, it sits inside bodies that have no compiled variant at all.

### 1. Mechanism

- Runtime (`interp_bridge.hunks` `task-r-jmp-bounce*`): after an interpreted JMP abs/long/(abs)/(abs,X)/[abs] whose
  target has a body for the live (m,x), the interpreter runs it (same bounce bookkeeping as the paired-call bounce)
  with `host_return_valid = 4`. Not inside host-invoked interrupt frames (`stop_on_rti`), off under `--lle`
  (`SNESRECOMP_LLE_BOUNCE=0`) and with `TLK_JMP_BOUNCE=0`. NORMAL return: continue at `g_tlk_tail_ret_pc24` with the
  interpreted-RTS frame-exit test; unwind sentinel and SKIP_N handled like the call bounce; anything else exits 44/45
  with a message (never seen in any gate).
- Codegen (`patches/snesrecomp/0006-jmp-entry.patch`): every RTS/RTL (and the HLE upload RTS) gets
  `if (_hrv == 4 && _ret_s == _entry_s) { g_tlk_tail_ret_pc24 = _rpc24; return NORMAL; }`: the body's own return
  pops the frame of the interpreted routine and hands the popped PC back. (hrv=0 would re-dispatch the popped PC and
  could run a following function compiled inside a host frame that resumes later: the paired-ABI double-execution.)

### Why the share did not move (cfg, still open)

Manifest dispositions of the hot code: `$C08500` (loop `$C08550-$C085A5`, 2.1%), boot/top level `$C08007`/`$C08122`
(`$C08096`, `$C088D2`) and the NMI head `$C09E0C` are `lle_only` with no variant, so nothing can bounce into them.
Tried (reverted, no gain): `data_region C0 85B3 85B5` (assertion BRK) + `exit_mx_variant C0B8B9 0 1 0 1` (PHP/PHB ...
PLB/PLP/RTS). The next blocker is `$C0873F -> $C00096` (M0X1), whose `JSL $C00201` leaves through `JML $C089B7`
(main loop) or `JMP $0355`: top-level control flow that never returns, plus `$C0B97B`'s `JML [$0016]` pushed-frame
dispatch. Other `lle_only` roots in the hot list: `$C191CC`, `$C44BA0` (both: unproven `$C07770`, a `JML [$001F]`
through a pointer passed on the stack), `$C0B4FF` (BRK `$C0B58E` + indirect), `$C09E0C` (`$C0A0F0`, `$C09EC6`: PHP ...
pushed-frame RTS dispatch ... PLP/RTS; manual exit proofs needed). `$7E2006` (1.3%) is untouched.

### Files

- `patches/snesrecomp/0006-jmp-entry.patch` (new), `cmake/runtime-patches/interp_bridge.hunks` (appended).

## 2026-10-12: save states and settings menu in `tlk_sdl` (task S)

### Result

Save states work in both hosts and in both modes, including a load while a compiled routine is suspended on the
fiber (task Q). 6987M tape, frame/WRAM SHA-256 at f6000 (uninterrupted run: `946033bd10683657...` / `87fa6746...`):

| run | f6000 frame / wram |
|---|---|
| `tlk_headless` default, save at f3000, new process `--load`, run to 6000 | `946033bd...` / `87fa6746...` |
| same, save at f1394 / f4500 | same |
| `tlk_headless --lle`, save at f3000 / f1394 / f4500 | same |
| `tlk_sdl` default, save f3000, run on to f3500, load in process (fiber discarded: `aborts=1`), run to 6000 | same |
| `tlk_sdl --lle`, same | same |

No-input f600 is still `57ccb8d5...`. Window checked under Xvfb (x11 driver, `import -window root`): the menu draws over
the paused game (f1200, level 1), and `--load` of the f3000 state shows level 1 at that spot.

### How

`runner/tlk_savestate.c`: file = `TLKSTAT1` header + the runtime's rollback snapshot (`RtlRollbackSaveToMemory`:
guest blob + timing residue: `g_cpu`, interpreter-bridge state, refresh carry, DMA/PPU residue, APU port scheduler).
The plain savestate (`RtlSaveSnapshot`) re-anchors timing and would not resume cycle-exact. Host statics outside both
are in the header: beam frame driver resume PC and WAI halt, the short-scanline field flag (task I), the late-NMI
latch. States are taken between frames only. A fiber suspended mid-routine has a live C stack and is not saved; on
load `tlk_fiber_discard()` resumes it with the abort flag (it unwinds on the old state, which the load then
overwrites), and the driver starts a fresh run at the saved resume PC, same as the pre-fiber path. Exact pacing
(task M) makes that tier change invisible, as the table shows. The rollback blob has no cross-build promise, so a
state only loads in the build that wrote it.

Runtime hunks (new, accessors only, no behaviour change): `snes.hunks` `savestate-short-field-export`
(get/set `s_short_field`, `s_late_nmi`), `beam_frame_driver.hunks` `savestate-driver-export` (get/set `s_resume_pc`,
`s_wai_halted`).

Hosts: `tlk_headless --save-at N file` / `--load file` (frame count and per-frame tape continue from the saved
frame). `tlk_sdl`: F1-F4 save, F5-F8 or Shift+F1-F4 load, slots `tlk_slotN.state` next to the ini; proof options
`--save-at N file`, `--load file`, `--load-at N file` (in-process load), `--menu-at N`. Settings overlay (Tab/Esc;
Esc in it quits): scale, fullscreen, vsync, audio on/off, volume, key remap, save to ini; 8x8 public-domain font
(`runner/tlk_font8x8.h`, same table as snesrecomp's OSD). Emulation pauses while it is open. The audio output rate is
now set once at start, so toggling audio does not change emulation.

### Open

- `--input-polled` position is not in the state (proof uses per-frame tapes).
- The menu and F-keys were not driven by real key presses here (no xdotool); the menu was opened by `--menu-at` and
  loads by `--load`/`--load-at`. Gamepad buttons still cannot be remapped.

## 2026-10-12: top-level state code, boot and NMI head compiled (task U)

### Result

| step | share (l01_right 3000 f) | wall (l01_right 5000 f, 3 runs) |
|---|---|---|
| start (task R tree) | 11.7% | 5.17-5.19 s |
| 1-2. cfg proofs + pushed-frame RTS calls + hrv=4 tail fix | 10.5% | - |
| 3. `$C08122` compiled, REP #$30 JMP entry | **9.2%** | **5.01-5.02 s** |

Gates on the final tree: see the end of this section. `--lle` hashes unchanged. Step 1 alone (first three cfg lines)
crashed default mode at 6987M f914 and was not kept on its own; every gate passes from step 2 on.

### 1. cfg: proofs and evidence (`recomp/bankc0.cfg`, `bankc2.cfg`, `bankc4.cfg`, `bankc5.cfg`)

- `data_region C0 85B3 85B5`: assertion `BRK` in the level loader (`$D4 >= $200`), never taken.
- `exit_mx_variant C00201 0 1 0 1`: PHP ... PLP/RTL at `$C00355` (`JMP $0355` lands on that PLP; R's "never returns"
  was wrong for that path). `JML $C089B7` is the no-return path (main loop) and contributes no exit width.
- `C0B8B9 0 1 0 1` (PHP/PHB ... PLB/PLP; needs `$C0B97B` to return M0, traced), `C0A0F0 0 0 0 0` (PEA/PHP into
  `$C0A0F7`'s body, its PLP/RTS returns via the PEA to the RTL), `C09EC6 0 0 / 1 0` (PHP ... PLP, pushed-frame handler
  returns inside the body), `C07770 0 0 0 0` (`JML [$001F]` to a caller-pushed pointer: evidence only, see below),
  `C45665 0 0` (check_exit_mx ok).
- Wrong blanket hints replaced by per-width `exit = entry` (all PHP ... PLP/RTL, check_exit_mx ok): `C47752`,
  `C22C4E`, `C22D79`, `C22D1F`, `C5383D`. The X=1 entries of the first ones made the main loop decode with a 16-bit X
  (false BRKs at `$C08D33`/`$C08DE6`) and, compiled, leaked x=0 into the main loop (6987M f914 RTS to `$C00000`).
- Trace evidence: new diagnostic `TLK_MX_HIST=<path>` in `runner/headless_main.c` (per interpreted PC, the set of
  widths seen). Collected with `--lle` over the 45 play tapes (f5000) and 6987M/6781M/4735M (f39000): the return sites
  `$C0B8D7`, `$C0011C` only M0X1; `$C09E80`, `$C09E91`, `$C19255`, `$C44D0F` only M0X0.
- New roots `C08259`, `C08258` (body after `$C08122`'s S reset, M0X0).
- `scripts/check_exit_mx.py`: 0 WRONG. UNPROVEN (manual proofs / traced, written next to each line): C0B8B9, C00201,
  C0A0F0, C09EC6 (pushed-frame returns the walker does not model), C07770 (open target set), C0AD22 (task Q).

### 2. Pushed-frame RTS calls (codegen patch `0007-rts-call.patch`)

New cfg line `rts_call <site_pc24> return:<pc24> [frame:3]`: the RTS at the site pops a target the routine pushed
itself on top of a `PEA <ret-1>` (or `PHK / PEA`) frame. Emitted as a paired call on the pushed frame
(`cpu_dispatch_call_pc_pushed`, frame 2 or 3) and a `goto` to the compiled continuation label for the live widths;
if there is no label, `interp_tier_dispatch_balanced` from the continuation. Sites: `$C085E5` (level record +$22
routine, continuation `$C085E6`), `$C08933` (main-loop state routine), `$C08763` (`PHK / PEA $8763`, handler RTLs to
`$C08764`). Before, under a JMP-entered body (hrv=4) these went through `interp_tier_dispatch_popped_return`, ended the
body at the handler's own return and exited 44. `v2_emit.py` exports the cfg dir as `SNESRECOMP_TLK_CFG_DIR`.

### 3. Runtime (`cmake/runtime-patches/interp_bridge.hunks`)

- `task-u-balanced-tail-ret`: when a body entered at a JMP target (hrv=4) ends through
  `interp_tier_dispatch_balanced`, publish the popped return PC (RTS or RTL frame below post-return S) in
  `g_tlk_tail_ret_pc24`. Fixes the continue screen exit 44 (`t_continue_yes/no`, f13794 `$C0AF52 -> $C26A40`, whose
  handler ends in an unlisted `JMP ($00A4)`), and any other jump-entered body that ends that way.
- `task-u-jmp-bounce-rep30`: a JMP target that starts with `REP #$30` and has no body for the live widths enters
  the M0X0 body (the REP makes entry widths irrelevant). `$C08B1F JMP $8122` arrives at M0X1.

### Not done / open

- Main loop `$C089B7` stays `lle_only` (`brk_at_C08DE6`: the analyzer still loses X=1 across the PHP at `$C08DAD` ...
  PLP at `$C08DC8`; not found). Its hot part `$C088D2-D9` is ~0.5%.
- Part of the `$C08550` loop is still interpreted on the first level load of l01_right (the load runs before
  `$C08122` is reached compiled: the `JSL $C22B36` chain returns to the interpreter at `$C082B2`).
- `$C0B4FF` (BRK `$C0B58E` + indirect), `$7E2006` RAM routine (1.2%) untouched.
- Continue tapes added to `build/playtapes/` (47 tapes now).

### Gates (final tree, real output)

```
compare_modes.sh 600 3000:            600 MATCH 57ccb8d5 / 3000 MATCH 89a48d6c (wram differs, as before task U)
compare_modes.sh -t 6987M 1200 6000:  1200 MATCH dd861e16 / 6000 MATCH 946033bd
check_all_tapes.sh -s:                play tapes @f5000: 47 MATCH, 0 MISMATCH
check_native_decomp.sh l01_right 3000: IDENTICAL
save f3000 -> load -> f6000:          frame_sha256=946033bd10683657...
TAS sync (default mode):              6987M 36904, 6781M 37093, 4735M 38870
t_continue_yes / t_continue_no f17000: MATCH 5fd6d1cd / MATCH 3e5eb7db (no exit 44)
check_exit_mx.py:                     148 ok, 9 UNPROVEN, 0 WRONG
```

## 2026-10-12: coverage of what the speedruns skip, ROM accounting (task T)

### Tapes (`tools/make_play_tapes.py build/t_tapes --task-t`, also copied into `build/playtapes/`)

Every target was checked on PNG dumps (`--lle`, looked at) or WRAM dumps.

| Tape | Frames | Reaches (frame) |
|---|---|---|
| `t_options_all` | 4326 | options f900; music OFF f1000, SFX OFF f1100; sound test list f1300-3000 (A plays, Down scrolls to UNDER THE STARS); triggers cycle ROAR/JUMP/SLASH layouts f3400-3600; title f3650; Pride Lands f4050 |
| `t_diff_normal/difficult/easy` | ~4350 | WRAM f1300: `$7FFF92` = 1 / 2 / 0, lives 2 / 0 / 8, continues 1 / 0 / 3; game start from the title (not the debug menu), Pride Lands f1300 |
| `t_pause` | 5266 | `$0A89` = 1 at f1500-1580, f1880-1980, f2280+ (paused), 0 between; Right pressed while paused |
| `t_continue_yes` | 17106 | Stampede idle, 8 lives lost; CONTINUE? YES/NO f13900; A = YES f14100 (continues 3 -> 2); back in Stampede with 8 lives f15000 |
| `t_continue_no` | 17042 | same, Right + A = NO: Disney/Virgin logos f14200, Westwood f14400, title f15000, attract demo f17000 |
| `t_attract` | 12000 | three demos: Pride Lands f3000, Elephant Graveyard f7000, Hakuna Matata f10000, title in between |
| `t_ending.polled` | 36905 polls, run 49000 f | 6987M TAS: Scar fight f39000, Simba on Pride Rock f40000-41500, ending picture f42500-44500, credits f45000-48000, THE END f48000 |

Menu facts found: in Options, A cycles the item under the cursor (Left/Right do nothing); presses closer than
~25 frames are dropped; after the sound test Start returns to the options with the cursor on DIFFICULTY and
ignores input for ~100 frames. Difficulty: NORMAL -> DIFFICULT -> EASY.

The ending is not reachable with a per-frame tape: `--input` drops Left+Right (`RtlRunFrame` filter) and the
TAS presses it on 716 polls; a per-frame tape built from the per-poll frames desynced at f1233. Hence the
`*.polled.tape` convention (scripts pass `--input-polled`).

### Default vs `--lle` (`scripts/check_all_tapes.sh -s -d <group> -f <frame>`)

```
t_diff_difficult MATCH 66df3608d845112c  t_diff_easy MATCH d0dceeb8013e53d5  t_diff_normal MATCH 7c86467921de0c06
t_options_all    MATCH 104cc1d7e5acbef5  t_pause     MATCH d990c99b52134993              (@f4300)
t_attract        MATCH b943468dfe8edf8c                                                  (@f12000)
t_ending.polled  MATCH                                                                    (@f49000)
t_continue_no    MISMATCH 3e5eb7db18dd5385 / none   t_continue_yes MISMATCH 5fd6d1cda59528fb / none (@f17000)
```

**Bug (default mode only, real): the continue screen kills the process (exit 44).**
`[tlk_jmp_bounce] f13794 $C0AF52 -> $C26A40 returned NORMAL without a tail return PC (S=$02E6 pre=$02E3)`.
Same on the old `l03_idle` tape run to f14500. `first_divergence.py`: no picture or lag difference before
it (WRAM differs from f1977 as usual). `--lle`, `TLK_JMP_BOUNCE=0` and `--interp-targets` with `C26A40` all
give the `--lle` frame `c4b6448f...` at f14500.
Cause: the script VM's `JML [$00A4]` at `$C0AF52` enters handler `$C26A40` through task R's JMP bounce
(`_hrv == 4`). The handler is `JSL $C0AE42 / JSL $C0AD22 / LDA #$6A21 / STA $A4 / JMP ($00A4)`. That last
`JMP ($00A4)` is not in the cfg (target `$C26A21` is not a decodable body), so the compiled body ends in
`interp_tier_dispatch_balanced(..., _hrv, ...)`, which interprets until the handler's RTL and returns
`RECOMP_RETURN_NORMAL`. Only the codegen's own RTS/RTL paths (patch 0006) store `g_tlk_tail_ret_pc24`;
the balanced-dispatch tail does not, so the bounce site sees no return PC and calls `exit(44)`.
Fix options (not applied, TLKRecomp owns cfg/runtime): (a) have `interp_tier_dispatch_balanced` set
`g_tlk_tail_ret_pc24` to the popped PC when `_hrv == 4` (general: any JMP-entered body ending in an
unresolved indirect tail has this bug); (b) cfg: `indirect_dispatch 6A4E 1 ptrtail targets:C26A21` in
bankc2 (merge_coverage proposes exactly this site) or exclude `$C26A40` from the bounce.

### Coverage proposals (not applied)

`scripts/coverage_runs.sh build/t_tapes build/cov/t` (continue tapes with `TLK_JMP_BOUNCE=0`), then
`tools/merge_coverage.py build/cov/t/*.json` -> `attic/task-t-cfg-proposals.diff (outside the repo)`: 157 new `func` lines
(C0 63, C1 31, C2 13, C3 1, C4 22, C5 6, C6 21), 24 new `indirect_dispatch` sites, and two grown tables:
script VM `$C0AF52` 166 -> 290 targets, object walker `$C0ABFB` 2 -> 108 targets. Review before applying:
runs like `FN_C0D2B4..C0D31B` (one entry per byte) look like mid-instruction or data entries.

Non-idle interpreted share (`scripts/interp_share.py --tape T --frames <len>`): options 6.1%, attract 9.5%,
continue_no 10.0% / continue_yes 10.3% (with `TLK_JMP_BOUNCE=0`), ending 14.1%, pause 14.9%, each difficulty 15.0%.

### ROM accounting (`tools/rom_accounting.py build/rommap/*.map --md docs/rom_accounting.md`)

Maps: `TLK_ROMMAP` in `--lle` over 57 runs (45 old play tapes, 9 task T tapes, 6781M/4735M/2716M per-poll
tapes; 6987M = `t_ending`). Full per-bank table in `docs/rom_accounting.md`. Totals (3,145,728 bytes):

| code_exec | audio | level | compressed | gfx_dma | dma_other | cpu_data | code_static | fill | unknown |
|---|---|---|---|---|---|---|---|---|---|
| 172,836 (5.5%) | 450,339 (14.3%) | 403,924 (12.8%) | 183,348 (5.8%) | 867,902 (27.6%) | 198,905 (6.3%) | 343,784 (10.9%) | 24,365 (0.8%) | 18,383 (0.6%) | 481,942 (15.3%) |

Code lives in $C0-$C6 only. Executed code bytes: 172,836; analysed-but-never-executed: 24,365 ->
**87.6%** of known code executed at least once; lower bound counting all 83,155 unknown bytes in $C0-$C6 as
code: **61.6%**. By tape set: old 45 play tapes alone 111,087 bytes; + 3 TASes 171,502; + task T tapes
172,836 (+1,334, menus/continue/ending). Limits: compiled interrupt bodies (NMI) are not seen in `--lle`
(they count as static/unknown); a DMA with size 0 counts as 64 KiB; level blobs are counted from the header
table even when not loaded; "audio" = bytes read by `$C21924-$C223FF` (SPC uploads).

### Files

`tools/make_play_tapes.py` (`--task-t`), `runner/tlk_rommap.c` + 6 lines in `runner/headless_main.c` + 1 in
`CMakeLists.txt`, `tools/rom_accounting.py`, `docs/rom_accounting.md`, `attic/task-t-cfg-proposals.diff (outside the repo)`,
`*.polled.tape` support in `scripts/check_all_tapes.sh`, `coverage_runs.sh`, `first_divergence.py`,
`interp_share.py`, `tools/README.md`.

## 2026-10-12: main loop, level-load tail compiled; task T cfg proposals reviewed (task V)

### Result

| step | share (l01_right 3000 f) | wall (l01_right 5000 f, 3 runs) |
|---|---|---|
| start (task U tree; task W binary, same machine) | 9.2% | 5.01-5.04 s |
| 1. `$C0FAC4`/`$C0FAAD` exit hints fixed: `$C089B7` compiles | 9.1% | - |
| 2-4. resume bounce + roots `$C082BB`/`$C08527`, `$C0B4FF` BRK as data, task T proposals | **7.8%** | **4.97-5.00 s** |

Every gate passes on the final tree (end of this section). `--lle` hashes unchanged.

### 1. `$C089B7` decoded `$C08DAD-$C08DE6` with 16-bit X (not the PHP/PLP pair)

The analyzer tracks PHP/PLP correctly. X was already 0 at the PHP: `JSL $C0FAC4` at `$C08D9B` (main loop, M0X1)
had a blanket `exit_mx_at C0FAC4 0 0`. `$C0FAC4` (and its twin `$C0FAAD`) is `PHP / REP #$30 ... PLP / RTL`, so
exit = entry. Replaced by per-width `exit_mx_variant ... m x m x` (all four, check_exit_mx ok). `$C089B7:M0X0` is
now `aot_eligible`. The new M0X1 demands turned up two more wrong blanket hints, fixed the same way: `$C0AF13`
(PHP/PHB ... PLB/PLP/RTS) and `$C2190F` (demanded by the task T roots). Found with a small flow walker over the
main loop that prints the first path reaching `$C08DAD` with X=0 (x drops after `$C08D9B`).

### 2. `$C08550` and the main loop after the first level load: resume bounce (runtime hunk)

Why it was interpreted: during the first level load `JSL $C22B36` (sound upload) yields; the compiled caller chain
is unwound and the scheduler's interpreter frame resumes at `$C082B2`. From there the level loader runs into the
main loop and never returns, and nothing on the way is a call or JMP target, so all of it (the `$C08550` loop, the
`$C088D2` wait, the per-frame main loop) stayed on the interpreter until the next state change. The same happens
when the force_lle decompressor `$C1CCFE` (called at `$C08523`) hands off to the scheduler and RTLs to `$C08527`.

Fix: `task-v-resume-bounce-*` hunks in `cmake/runtime-patches/interp_bridge.hunks`. In the scheduler frame only
(never in interrupt-handler frames, never under `--lle`/`TLK_JMP_BOUNCE=0`; `TLK_RESUME_BOUNCE=0` turns it off),
a compiled body at the current PC is entered exactly like a task R JMP arrival (hrv = 4) right after a yield-unwind
consume, during the next 8 interpreted opcodes (calls excluded, they bounce anyway), and after every RTS/RTL the
scheduler frame interprets. New roots (bankc0.cfg): `$C082BB` (after the loader's PLD/PLP) and `$C08527` (after the
decompressor call), both M0X0 only in `TLK_MX_HIST` (`--lle`).

Not applied: root `$C08860` (main loop head, `$C089A7 JMP $8860`, M0X1). It compiles and all 54 tapes match, but
compiled it adds one lag frame against `--lle` at 6987M f13092 (TAS sync 36904 -> 36901; with `$C08860` in
`--interp-targets` the lag frame is gone). Open lead: the main loop is still interpreted after a JMP `$8860`
(`$C088D2-D9` wait spin, 12,635 ops per 3000 f).

### 3. `$C0B4FF` and `$7E2006`

- `$C0B58E BRK #$00` is an assertion (object field +$30 == 0); the BRK vector `$00FFE6 -> $C0A2E8` is
  `JML $70800B`, a development hook outside the cartridge. `data_region C0 B58E B590`; `$C0B4FF` is now
  `aot_eligible`, its `PHK / PEA $B5AB / JML [$0014]` callback goes to the interpreter (lle_dynamic).
- `$7E2006` (the MVN/RTL the decompressor writes): tried a second native hook at `$7E2006` in
  `runner/tlk_decomp.c`. Safe (guarded by the 54 xx xx 6B check, native-decomp gate IDENTICAL) and share 7.8% ->
  6.7%, but the native run is slower than the interpreter here: 4.97 -> 5.25 s per 5000 frames. Reverted (comment
  left in kRegions). Note: on the task U tree `native_decomp_runs=0` on l01_right (the `$C1CD75` hook never fires in
  default mode any more); the hook still runs in the `check_native_decomp.sh` gate (`--lle`).

### 4. Task T cfg proposals (`attic/task-t-cfg-proposals.diff (outside the repo)`)

The diff was against T's pre-U tree (2 hunks no longer apply), so it was merged by script: new `func` lines added,
grown tables merged as the union with the current targets, new indirect sites added. Review:

- **157 func: all applied.** Every one is either a direct `JSL`/`JSR` target in the ROM (checked by byte search;
  most start right after an RTS/RTL, with `PHP`) or an entry of the `$C0D230`/`$C0D272` word tables
  used by `JSR ($D230,X)` / `JSR ($D272,X)` at `$C0D1D7..$C0D221`. That covers `FN_C0D2B4..C0D358`: they are not
  bogus but a multiply-by-constant routine with one entry per multiplier (`LDA #0 / RTS`, then chains of `ASL A` with
  shared tails), entered mid-chain by design. Entry widths are the traced ones. Many of the new bank C0/C1/C4-C6
  roots are `lle_only` (unproven callee exits); that is no worse than before (they were interpreted).
- **Indirect: 23 of 24 applied.** `AF52` script VM table 166 -> 290 (union), 22 new sites (traced targets),
  including `6A4E ptrtail targets:C26A21` (decodes fine at the traced M0X1).
- **Rejected: `ABFB` object walker table 2 -> 108.** The new targets pull in unproven M0X1 callee exits (`$C09AB8`,
  `$C0B97B`), which make `$C0ABBB`/`$C0ABCD` `lle_only`; the JMP bounce into `$C0B3CC` then exits 44 at f108
  (title screen). Kept the task Q table; comment in bankc0.cfg.

### Still interpreted (l01_right 3000 f, default mode, non-idle)

`$7E2006` MVN (264,640 ops, 15,189 calls), main loop wait `$C088D2-D9` (12,635 each), `$C0D7D1..` (3,082, an
object handler in the rejected walker table), then a long tail.

### Exit-width hints

`scripts/check_exit_mx.py`: 160 ok, 9 UNPROVEN, 0 WRONG. UNPROVEN, unchanged from task U: `C0AD22` (BRK data at
`$C0AD92`), `C0B8B9`, `C00201`, `C0A0F0`, `C09EC6` x2 (pushed-frame returns the walker does not model; traced),
`C07770` (`JML [$001F]` open target set), `7E2006` x2 (RAM code). No new hint is unproven: every task V hint is an
`exit = entry` variant the walker proves.

### Files

`recomp/bankc0.cfg`, `bankc1-c6.cfg` (hints, roots, data_region, T proposals),
`cmake/runtime-patches/interp_bridge.hunks` (`task-v-resume-bounce-*`), `runner/tlk_decomp.c` (comment only).

### Gates (final tree, real output)

```
compare_modes 600 3000 / -t 6987M 1200 6000:
600    MATCH    57ccb8d5b24918fd   57ccb8d5b24918fd   same        600/600
3000   MATCH    89a48d6c4f12621b   89a48d6c4f12621b   differs     3000/3000
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
play tapes @f5000: 54 MATCH, 0 MISMATCH
ALL MATCH
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 frame=d2a23a7de3af6bf7/d2a23a7de3af6bf7 wram_frames_differing=0
save f3000 -> load -> f6000: frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
6987M in_sync_through_poll=36904 (headless f39206, bizhawk f39207)
6781M in_sync_through_poll=37093 (headless f39389, bizhawk f39389)
4735M in_sync_through_poll=38870 (headless f41165, bizhawk f41166)
check_exit_mx.py: 160 ok, 9 UNPROVEN, 0 WRONG
t_continue_yes f17000: MATCH 5fd6d1cda59528fb   t_continue_no f17000: MATCH 3e5eb7db18dd5385
```

## 2026-10-12: nested-frame IRQ entry, main loop head, object walker table (task X)

### Result

| step | share (l01_right 3000 f) | wall (l01_right 5000 f, 3 runs) |
|---|---|---|
| start (task V tree) | 7.8% | 4.97-4.99 s |
| 1. nested-frame IRQ fix + root `$C08860` | 7.4% | - |
| 2. `ABFB` table 2 -> 94 targets + `C22C48` hint | 6.8% | - |
| 3. RTL pushed-frame call `$C0D7F3` (patch 0008) | **6.4%** | 4.99-5.10 s (noise; no change) |

### 1. `$C08860` lag frame: IRQ sampled one opcode late in a fresh nested interpreter frame
HWLOG diff (6987M, `$C08860` compiled vs the same binary with it in `--interp-targets`): first master-clock
difference at f10741. The IRQ was taken at `$C0FD2F` (compiled) instead of `$C0FD2C` (34 clocks late). The main
loop's pushed-frame RTS at `$C08933` (task U `rts_call`) enters the interpreted state routine `$C0FD2C` in a nested
frame. Compiled call instructions charge without their boundary event; a compiled target checks it at entry, but the
nested frame tested IRQ only from its 2nd opcode on (`steps &&`). Fix (general): hunks `task-x-nested-entry-irq*` in
`cmake/runtime-patches/interp_bridge.hunks` also sample IRQ at step 0 in fresh nested frames under the scheduler
(same guard as the task Q settle). After it, per-frame master clocks match the interpreted-head run, and `$C08860` is
enabled in bankc0.cfg.

### 2. `ABFB` object walker table
Cause of the T failure: 14 of the 108 targets are `lle_only` (unproven callee exits). Two of them (`$C0B155` via
`$C09AB8`, `$C0B5FD` via `$C0B97B`) made `$C0ABBB`/`$C0ABCD` and then `$C08860` `lle_only`. Applied the 94 targets
the analyzer proves (left out: C03CAF C0B155 C0B5FD C2D7CA C47870 C48C3A C4CD7E C58EF0 C5978B C5CED6 C5CF3F
C65BF3 C65C8A C65DA0; they still run, as unlisted pointers, on the interpreter). `$C0ABCD` still lost the M0X1 call
`$C0D123 -> $C22C48`; added `exit_mx_variant C22C48 0 0 0 0` / `0 1 0 1` (`JSL $C2190F / SEC / RTL`, check_exit_mx ok).
1075 -> 1154 aot_eligible variants.

### 3. `$C0D7D1` object routine call
`PHK / PEA $D7F3 / PHX / PHA / RTL`: codegen patch `0008-rtl-call.patch` lets `rts_call` name an RTL site
(pops the bank byte too); `rts_call C0D7F3 return:C0D7F4 frame:3`.

### Not done
`$7E2006` MVN (lead 3): no faster safe way found; not attempted again (task V's native hook was slower).

### Still interpreted (l01_right 3000 f, default, non-idle)
`$7E2006/09` (279,829 ops), `$C191CC..` (2,545 each), then a long tail.

### Gates (final tree, real output)
```
600 MATCH 57ccb8d5b24918fd / 3000 MATCH 89a48d6c4f12621b
1200 MATCH dd861e164c295e15 / 6000 MATCH 946033bd10683657
play tapes @f5000: 54 MATCH, 0 MISMATCH
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 wram_frames_differing=0
save f3000 -> load -> f6000: frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
t_continue_yes 5fd6d1cda59528fb == lle; t_continue_no 3e5eb7db18dd5385 == lle
6987M 36904, 6781M 37093, 4735M 38870
check_exit_mx.py: 162 ok, 9 UNPROVEN (unchanged), 0 WRONG
```

## 2026-10-12: gamepad remap, menu on the pad, packaging (task W)

### Result

- `tlk_sdl` Controls page (was "Keys..."): per SNES button a keyboard key and a gamepad button. Enter (or pad
  South) on a row waits; the next key binds the key, the next pad button binds the pad (Esc / Guide cancel). "Reset
  defaults" restores both tables. "Save settings" writes `pad_<button> = <SDL gamepad name>` next to the key lines
  (`pad_b = a`, `pad_y = x` ...). Stick and analog triggers still add d-pad and L/R on top.
- Gamepad drives the menu: Guide or Back+Start opens it; d-pad moves, South selects, East goes back, Start/Guide
  closes. After the menu closes the pad is ignored until fully released, so the Start that closed it never reaches the
  game. Hotplug: a pad connected later is opened (toast), unplug closes it and opens the next one.
- No `--rom` and no `rom=`: every `.sfc`/`.smc` next to the binary, then in the cwd, is verified; the first match is
  used, others are reported with their CRC32, and the error says the USA ROM CRC32 C8FBFAA8 is needed.
- `cmake --install build --prefix DIR`: `bin/tlk_sdl`, `share/doc/TLKRecomp/{README.txt,LICENSE-NOTES.txt,
  LICENSE-snesrecomp.txt}`. `scripts/package.sh` -> `dist/TLKRecomp-linux-x86_64.tar.gz` (stripped `tlk_sdl` + the
  three text files; it refuses to pack anything else, no ROM, assets or generated C). The binary links the system
  `libSDL3.so.0` (README says to install SDL3).
- New proof options: `--selftest-remap` (synthetic SDL key/button events + an SDL virtual gamepad; 29 checks: rebind
  key and pad button, Esc cancel, save/reset/reload ini, pad navigation, hotplug open/close, live state through the new
  binding, Guide and Back+Start open, held Start not leaked) and `--menu-page controls` with `--menu-at N`.

Checks (copy, fresh build dir): `compare_modes.sh 600 3000` MATCH 57ccb8d5b24918fd / 89a48d6c4f12621b;
`check_all_tapes.sh -s` 54 MATCH; `tlk_sdl --no-pace --frames 3000` f600 57ccb8d5..., f3000 89a48d6c... (defaults change
no frame); `--selftest-remap` PASS; Xvfb screenshots of the boot, the settings page and the Controls page; tarball
unpacked to a temp dir runs with `--rom` (screenshot) and finds a ROM placed next to the binary (f600 57ccb8d5...).

### Open

- Real key presses and a real controller were not tested (no input device under Xvfb); the virtual gamepad covers the
  SDL gamepad path but not a specific controller's mapping.
- The tarball depends on the system SDL3; `SNESRECOMP_SDL3_FETCH` (static SDL) was not tried for packaging.

Files: `runner/sdl_main.c`, `CMakeLists.txt` (install rules), `scripts/package.sh` (new), `packaging/README.txt`,
`packaging/LICENSE-NOTES.txt` (new), `README.md`, `.gitignore` (`/dist/`), this file.

Fixes after task W: `--selftest-remap` saved its test bindings (K / L3) into the real `tlk_sdl.ini`;
it now saves to `$TMPDIR/tlk_sdl_selftest.ini`. Controls page shortens "Right X"/"Left X" key names to "R X"/"L X"
("Right Shift" was cut to "Right Shif"). Self-test 29/29 PASS, screenshot checked.

## 2026-10-12: idle run compiled vs `--lle` picture mismatch f7085-~10000 (task Y)

### Cause
`snes.c` `$4212` read keeps a legacy static-recomp polling tick: `snes_advance_beam(64)` (hPos only, not
`beamMasterLast`) whenever `g_interp_apu_driving` is 0, i.e. for reads from compiled code. The beam then stays 64
clocks ahead of the master clock for good, so DRAM refresh, H-blank and IRQ positions move. Under `--lle` the main
thread is interpreted (no tick) and the frame-driver NMI is compiled (tick: H-blank wait `$C0A2EC`, called only from
`$C09EBC`); the calibrated timeline (TAS sync) includes those ticks. Default mode also ticked in compiled
main-thread code: joypad wait `$C0A073` (`$C5017D JSL $C0A06E`, title/demo). Idle run: first hardware access off at
f2557 (`$C22130 STA $2140`, refresh charged one opcode early: beam h=404 vs 340 at the same master clock), traced back
to a single beam walk over a `$4212` read at `$C0A076`; frames then end at different clocks and the demo picture
differs f7085-~10000.

Found with `scripts/bisect_interp.py` (with `TLK_JMP_BOUNCE=0 TLK_RESUME_BOUNCE=0`; it names `$C0A2EC`, a red
herring: any interp-target change moves the chaotic timeline), then `tools/aot_pace/pace_diff.sh` on an empty tape
and temporary prints in a trace build (beam syncs, refresh charges). Introduced by **task X**: premerge-V and
premerge-X trees built in `scratchpad/bisect` pass the idle gate to f10000; main before this fix fails at f7085.

### Fix
Runtime hunks: `snes.hunks` `4212-poll-tick-interrupt-only` (tick only for compiled code inside a frame-driver
interrupt) and `beam_frame_driver.hunks` `interrupt-depth` (`tlk_frame_interrupt_depth` around
`interp_bridge_run_interrupt`). Removing the tick everywhere was tried: pictures stay equal but TAS 6987M desyncs
at poll 1785 (the tick is part of the calibration), so it is kept exactly where `--lle` has it.
Proof: `--lle` HWLOG to f7100 byte-identical to before; default HWLOG == `--lle` HWLOG to f7100 (no access differs).

### Gate
`scripts/check_idle_lle.sh [-n 12000] [-k 5]`: headless, empty tape, PNG every K frames in both modes.

### Gates (final tree, real output)
```
IDLE MATCH frames=12000 every=5 pictures=2400
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
600    MATCH    57ccb8d5b24918fd   57ccb8d5b24918fd   same        600/600
3000   MATCH    89a48d6c4f12621b   89a48d6c4f12621b   differs     3000/3000
frame  result   lle_frame          aot_frame          wram        ran(lle/aot)
1200   MATCH    dd861e164c295e15   dd861e164c295e15   differs     1200/1200
6000   MATCH    946033bd10683657   946033bd10683657   same        6000/6000
play tapes @f5000: 54 MATCH, 0 MISMATCH
ALL MATCH
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 frame=d2a23a7de3af6bf7/d2a23a7de3af6bf7 wram_frames_differing=0
frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
t_continue_yes MATCH 5fd6d1cda59528fb
t_continue_no MATCH 3e5eb7db18dd5385
6987M in_sync_through_poll=36904 (headless f39206, bizhawk f39207)
6781M in_sync_through_poll=37093 (headless f39389, bizhawk f39389)
4735M in_sync_through_poll=38870 (headless f41165, bizhawk f41166)
(save f3000 -> load -> f6000 above; continue f17000 default == --lle)
```

## 2026-10-12: self-contained Linux package, SDL3 bundled (task P2)

### Result

- New CMake option `TLK_STATIC_SDL` (default OFF, dev builds unchanged and still link the system `libSDL3.so.0`).
  ON: `CMAKE_DISABLE_FIND_PACKAGE_SDL3` + snesrecomp's `SNESRECOMP_SDL3_FETCH`, i.e. SDL3 **3.4.10** release tarball,
  sha256-pinned, built from source and linked statically. Forced ON: video, audio, joystick, hidapi, Wayland, X11,
  PipeWire, PulseAudio, ALSA, libudev, D-Bus, KMSDRM, OpenGL; the `*_SHARED` variants are ON, so the backends
  dlopen the desktop's libraries at run time and the binary has no hard dependency on them. The SDL3 `LICENSE.txt`
  (zlib) is installed as `LICENSE-SDL3.txt`.
- `scripts/package.sh` now configures `build-pkg/` with `-DTLK_STATIC_SDL=ON` (`-b` still overrides), packs
  `LICENSE-SDL3.txt` too and fails if `ldd tlk_sdl` mentions any SDL. Tarball 16.9 MB (stripped binary 44 MB).
- `tlk_sdl --list-drivers` prints SDL version and compiled-in drivers. Packaged binary:
  `SDL 3.4.10`, video `wayland x11 kmsdrm offscreen dummy evdev`, audio `pipewire pulseaudio alsa sndio jack disk dummy`;
  joystick: Linux evdev + HIDAPI + libudev.
- `ldd` of the packaged `tlk_sdl`: only linux-vdso, libm, libstdc++, libgcc_s, libc, ld-linux.
- `packaging/README.txt` (nothing to install; driver env vars; `--list-drivers`), `packaging/LICENSE-NOTES.txt`
  (SDL3 3.4.10 statically linked, zlib, see LICENSE-SDL3.txt).

Checks on the unpacked tarball (`$TMPDIR/pkgtest2`, ROM copied next to the binary then deleted): `--no-pace
--no-audio --frames 3000` frame_sha256 `89a48d6c4f12621b...`; `--selftest-remap` PASS (0 failed); audio opens with
`SDL_AUDIO_DRIVER=dummy`, `pipewire` and `pulseaudio` (bogus driver name errors as expected); a real Wayland window
on a desktop session (`SDL_VIDEO_DRIVER=wayland`, 30 frames); Xvfb x11 screenshot of the Westwood/Disney boot screen.

Gates (dev `build/`, fresh configure in this copy, final tree): `compare_modes.sh 600 3000` MATCH 57ccb8d5b24918fd /
89a48d6c4f12621b; `-t build/tapes/6987M.tape 1200 6000` MATCH dd861e164c295e15 / 946033bd10683657;
`check_all_tapes.sh -s` 54 MATCH, ALL MATCH; Xvfb x11 screenshot of `build/tlk_sdl` (boot screen).

### Open

- glibc: the binary needs a glibc at least as new as the build machine's (Arch). An older-distro build would need an
  older build host/container.
- Real controller not tested (as in task W).

Files: `CMakeLists.txt`, `runner/sdl_main.c` (`--list-drivers` only), `scripts/package.sh`, `packaging/README.txt`,
`packaging/LICENSE-NOTES.txt`, this file.

## 2026-10-12: deaths and respawns in every level, function count (task Z)

### Result

No bug found. 17 new tapes lose lives in all 14 levels (66 deaths, 2 game overs); default and `--lle`
`tlk_sdl` picture hashes are identical **every 10 frames over every tape** (13,197 compared frames).
Post-respawn PNGs (default mode, 2 per death) show no garbage. A scrambled screen seen once after a water
death was not reproduced. The only odd picture, a blue-sky palette during the 13-frame fade-in after a death
in Can't Wait to be King, is real game behaviour: BizHawk shows the same frames (below).

### Tapes (`tools/make_play_tapes.py OUT --task-z`, copied into `build/playtapes/`)

Picked from ~120 candidate patterns by `--ram-log` (lives `$7FFFAA`); `TLK_RAM_LOG_DEATH=1` (new) appends
`$7EA6BA` (= `$FFFF` after a fall into water) and continues `$7FFFA8` to each `--ram-log` line.
Levels start from the debug menu with 8 lives.

| tape | frames | deaths (frame lives dropped) | kind |
|---|---|---|---|
| d_00_r7007 | 7266 | 4153, 6964 | enemies, Pride Lands |
| d_01_jump | 7292 | 1883 ... 5664 (8), game over, title, new game | water (all 8 flagged), game over |
| d_01_run | 7292 | 1802 ... 5477 (8), game over | water, game over |
| d_02_idle | 7318 | 2398, 3475, 4552, 5629, 6706 | hyenas, Elephant Graveyard |
| d_02_r7106 | 7318 | 2747, 4070, 5158, 6578 | random play |
| d_03_idle | 7344 | 2718, 4091, 5464, 6837 | wildebeest, Stampede |
| d_03_jump | 7344 | 3588, 5831 | wildebeest |
| d_04_jump | 7370 | 2885, 4441, 5901 | falls, Simba's Exile |
| d_05_r7252 | 7396 | 2097, 2749, 3622, 4798, 5455, 6587 | water (all 6 flagged), Hakuna Matata |
| d_06_l9301 | 9422 | 3555, 6002 | Simba's Destiny (left-weighted random) |
| d_07_jump | 7448 | 2831, 4010, 5275, 6606 | lava / falls, Be Prepared |
| d_08_r7424 | 9474 | 4061 | Simba's Return (only 1 of 32 patterns died) |
| d_09_l9454 | 9500 | 2586, 3811, 5191, 6237, 7577 | Pride Rock |
| d_10..13 | 7526-7604 | 2+4+4+2 | bonus games have no lives: the bonus ends, the game drops to Pride Lands and Simba dies there |

Level 0, 6, 8 and 9 never kill an idle Simba (he stands at a wall or out of reach); random play does.

### Default vs `--lle`

`tlk_sdl --no-pace --no-audio --no-record --frames N --input T --hash-at 10,20,...,N` in both modes, for all 17
tapes: identical at every sample. `scripts/check_all_tapes.sh -s`: 71/71 MATCH.

Fixed on the way: `tlk_sdl --hash-at` copied the list into a 1024-byte buffer, so frames past ~2260 were
silently never hashed. Now the list is any length (`runner/sdl_main.c`).

### Respawn fade in Can't Wait to be King (BizHawk check)

After a water death (d_01_run, f1802) the screen fades in over f1965-1981 with a blue sky, then turns orange
at f1982. Built a bk2 from the tape (header of `6987M.bk2`, input log from the tape) and screenshotted
every frame in BizHawk 2.11 (bsnes core): lives drop on the same frame (1802) and the centre pixel matches
ours within a few colour levels (different RGB conversion) on every frame of the fade, including the switch to orange at 1982. Not a bug.

### Function count

`tools/function_count.py --tables build/rommap/*.map`, method and numbers in `docs/function_count.md`:
manifest 1,244 entry pcs (1,085 compiled); static walk 1,259; ~95 more reached only through pointers ->
**~1,350 known, best guess 1,400-1,500, ceiling ~1,920**. `docs/rom_accounting.md` regenerated with 74 maps
(71 play tapes + 3 per-poll TAS tapes): executed code 172,836 -> 179,821 bytes.

### Files

`tools/make_play_tapes.py` (`--task-z`), `runner/headless_main.c` (`TLK_RAM_LOG_DEATH`), `runner/sdl_main.c`
(`--hash-at` length), `tools/function_count.py` (new), `docs/function_count.md` (new),
`docs/rom_accounting.md` (regenerated), this section. Data (not distributable, not in git): 17 tapes in
`build/playtapes/`, maps in `build/rommap/`.

## 2026-10-12: DSP start phase derived from bsnes v115 power-on (task DSP)

### Result

`apu->cycles = 25` at reset (`cmake/runtime-patches/apu.hunks`, `apu-dsp-sample-phase`) is kept; the value is
now derived instead of fitted. Comment-only change, so every gate is unchanged.

### Derivation (bsnes v115, `bsnes/sfc/...` at tag v115)

1. Power-on: `SMP::power` (`smp/smp.cpp:40`) loads PC from the IPL vector and runs no reset cycles;
   `DSP::power` sets `clock = 0` (`dsp/dsp.cpp:50`); `SPC_DSP::reset` sets `m.phase = 0` (`dsp/SPC_DSP.cpp:872`).
2. Every SMP bus cycle calls `SMP::wait` -> `SMP::step` (`smp/timing.cpp:35`) *before* its access
   (`smp/memory.cpp:16-37`); `step` subtracts 2 from `dsp.clock` and `synchronizeDSP` (`smp/smp.cpp:15`) calls
   `DSP::main` -> `spc_dsp.run(1)` (`dsp/dsp.cpp:12`), one of the 32 phases of `GEN_DSP_TIMING`
   (`dsp/SPC_DSP.cpp:778`). So SMP cycle j runs DSP phase j mod 32: a sample period begins on SMP cycle 0.
3. LakeSnes `apu_reset` sets `cpuCyclesLeft = 7` (`snesrecomp/runner/src/snes/apu.c:61`): our SPC idles 7 cycles
   before the first opcode, so bsnes SMP cycle j is our APU cycle j + 7 (the opcode-on-last-cycle hunk keeps this
   1:1 after that).
4. Our whole-sample `dsp_cycle` runs when `(apu->cycles & 31) == 0`. To land it on DSP phase 0 of the SMP's
   timeline it must fire on our cycle 7 mod 32: (start + 7) mod 32 = 0, start = **25**. This matches the old fit
   exactly (24/26/27/28/31 desync TAS replays, task I/O).

Not changed: the SPC timers (`apu->timer[i].cycles = 0` at reset) count from our cycle 0, not from the SPC's
cycle 0, so they carry the same 7-cycle offset relative to bsnes; untested whether that matters.

### Gates (final tree)

compare_modes 600/3000: MATCH 57ccb8d5b24918fd / 89a48d6c4f12621b. 6987M 1200/6000: MATCH dd861e164c295e15 /
946033bd10683657. check_all_tapes -s: 71 MATCH, 0 MISMATCH. check_idle_lle: IDLE MATCH. check_native_decomp
l01_right 3000: IDENTICAL. TAS sync (default and `--lle` identical): 6987M 36904, 6781M 37093, 4735M 38870.
tlk_sdl 1200 f WAV: peak 9998, non-silent.

## 2026-10-12: compile the remaining known entry points (task AA)

### Result

| | start (task Z tree) | after AA |
|---|---|---|
| manifest entry pcs / compiled | 1,244 / 1,085 | **1,259 / 1,255** |
| nodes `aot_eligible` / `lle_only` | 1,154 / 161 | 1,326 / 4 |
| share (l01_right 3000 f) | 6.4% | **5.1%** |
| wall (l01_right 5000 f, 3 runs, same idle machine) | 4.93-4.96 s | 4.85-4.87 s |

All 159 blocked entries had the same blocker kind: `unproven_callee_exit` (+ `truncated_call_continuation`);
one also had `brk_at_C0B8B5`. Fixed by exit-width hints proven with an extended `scripts/check_exit_mx.py`,
one `data_region`, and 13 of the 14 object walker targets task X left out. No codegen or runtime change.

### 1. `TSC / CLC / ADC #n / TCS` (58 entries)
The game's compiled-C convention pushes arguments (`PEA`/`PEI`) and the caller drops them with
`TSC / CLC / ADC #n / TCS`. The walker now models it (drops n bytes, only bytes this routine pushed).
Hints `C1926F`, `C1A0F2`, `C06B66`, `C4153A` (exit = entry) unblocked 1,085 -> 1,145.

### 2. Mutual recursion (induction, 72 entries)
The $C0/$C1/$C3/$C5/$C6 game logic is mutually recursive ($C302EF <-> $C30792 ...). check_exit_mx now proves
the declared hints together: every declaration is a hypothesis, calls to a hypothesis are summarised by it,
hypotheses whose body does not return exactly the declared width are dropped until a fixpoint (holds for every
terminating call, by induction on call depth). `check_exit_mx.py --induct PC...` does the same for queries.

### 3. Callbacks (TRACED axioms)
Function pointers the walker cannot enumerate: trampolines `$C07770` and `$C0CD0E` (`JML [$001F]`), pushed
frames `PHK / PEA r / JML [dp]` (with callee-pulled arguments, `$C0B97B`) and `PHK / PEA r / PHA / RTS`. The
walker assumes a callback returns with the widths of its call and records each such return site. New
diagnostic `TLK_CALL_PAIR=<path>` (`runner/tlk_callpair.c`, headless) records, per return PC, the
(call widths -> return widths) pairs the interpreter saw; `check_exit_mx.py --call-pairs` checks every recorded
site: 20 ok, 1 never ran, 0 wrong over 74 `--lle` captures (71 play tapes full length + 6987M/6781M/4735M
polled TAS). (`--mx-hist` alone flags `$C0CCFB`: `$C0CC61` is entered at M0X0 and M0X1, which a per-PC
histogram cannot pair; the call pairs show each width returns as entered.) Also axioms by manual proof +
call-pair evidence: `$C09BFD`/`$C09C64` (PHP/PHB/PHX ... PLX/PLB/PLP around a PEA $0000-terminated stack list).

### 4. Object walker table, BRK assertion
- `data_region C0 B8B5 B8B7`: `BRK #$4C` in node allocator `$C0B85B` (free list empty), never executed (MX_HIST).
  The walker now stops at cfg data regions.
- `ABFB` table 94 -> 107: added C03CAF C0B5FD C2D7CA C47870 C48C3A C4CD7E C58EF0 C5978B C5CED6 C5CF3F
  C65BF3 C65C8A C65DA0. **Left out `$C0B155`**: with it, default mode fails at `d_08_r7424` f2526 (stack
  corrupt, NMI to `$C01F20`; found by bisecting the 14). Cause not found; it runs interpreted as before.

### Still `lle_only` (4 entries)
`$C0A05D` (calls `$C50179`, whose demo-exit path JMLs into the main loop), `$C465F0` (calls `$C50119`, which
never returns: `JML $C08122`), `$C3887A`/`$C3883A` (`$C07624` pushed-frame dispatch + `JML $C0A0F0` frame;
level 8). Plus `$C0B155` as an unlisted walker pointer.

### Exit-width hints
`check_exit_mx.py`: 275 ok, 11 UNPROVEN (the 9 from task X minus `C0B8B9`, now ok; plus axioms `C0CD0E`,
`C09BFD`, `C09C64`), 0 WRONG.

### Gates (final tree, real output)
```
600 MATCH 57ccb8d5b24918fd / 3000 MATCH 89a48d6c4f12621b
1200 MATCH dd861e164c295e15 / 6000 MATCH 946033bd10683657
play tapes @f5000: 71 MATCH, 0 MISMATCH
IDLE MATCH frames=12000 every=5 pictures=2400
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 wram_frames_differing=0
TAS default/--lle: 6987M 36904/36904, 6781M 37093/37093, 4735M 38870/38870
death tapes every 10 f, tlk_sdl default == --lle: all 17 SAME (726-950 hashes each, full counts)
save f3000 -> load -> f6000: frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
t_continue_yes 5fd6d1cda59528fb == lle; t_continue_no 3e5eb7db18dd5385 == lle
audio: tlk_sdl 1200 f WAV 960000 samples, RMS 1860
```

## 2026-10-13: All Bugs TAS gate (task DD)

`tools/ram_sync_compare.py --resync K`: drift-tolerant mode. Scans every compared poll instead of stopping at the
first desync; differing polls are grouped into windows (gaps under K merged); a window is re-synced if K matching
polls follow it. Prints differing count, windows, last matching poll, end_in_sync, level starts of both sides;
exit 0 iff every window re-synced and the last poll matches. Without `--resync` output is byte-identical to before.

`scripts/check_allbugs.sh [-b bin] [-o outdir]`: allbugsF (allbugs_final.bk2) default and `--lle`,
RAM logs must be identical; vs BizHawk: end in sync, <=150 differing polls, all inside polls 3500-3800 or
4234-4235, level starts on the same poll outside that window. Added to the gate list.

```
default == --lle: IDENTICAL (60768 frames)
differing_polls=110 windows=2 resync=50
window polls 3571-3705 differing=109 hl_f=3897-4031 bh_f=3898-4032 resynced
window polls 4235-4235 differing=1 hl_f=4896-4896 bh_f=4742-4742 resynced
last_matching_poll=56869 end_in_sync=yes (headless f60764, bizhawk f60767)
differing=110 (<=150) outside_window=0 [] end_in_sync=yes level_starts=26 level_mismatch=0 []
ALLBUGS PASS
6987M in_sync_through_poll=36904   6781M 37093   4735M 38870   (old and new tool output identical)
```

Level starts at polls 3782 and 4234 (levels 1/A and 1) are one poll earlier than BizHawk (3783/4235), inside the
drift window; all 24 others are on the same poll.

Coverage (`TLK_ROMMAP`, `--lle`, all 60,768 frames, vs OR of the 74 maps in `build/rommap/`): allbugsF executes
163,501 code bytes; 6,047 are new (2,578 opcode bytes). ~4,000 lie in already-compiled (`aot_eligible`) bodies
(largest: `$C55247-$C55613` 973 B, `$C06BE0-$C06E67`, `$C1678F-$C16940`, `$C491A6-$C49252`). ~2,000 lie in no
analysed body, so they run interpreted: `$C087C1-$C08858`, `$C0EADC`, `$C239FE`, `$C23A52` (32 B each),
`$C400E4-$C4024E` 363 B, `$C43CD2-$C43DCB` 250 B, `$C54CF2-$C54D9C` 171 B, `$C55A66-$C55AF7` 146 B,
`$C63DC4-$C6417F` ~900 B. Not yet added to the cfgs.

## 2026-10-13: the last uncompiled entry points, `$C0B155` crash (task CC)

### Result

| | start (task AA tree) | after CC |
|---|---|---|
| manifest entry pcs / compiled | 1,259 / 1,255 | **1,260 / 1,260** (`$C0B155` is a new entry) |
| nodes `aot_eligible` / `lle_only` | 1,326 / 4 | **1,331 / 0** |
| share (l01_right 3000 f) | 5.1% | **3.9%** |
| wall (l01_right 5000 f, 3 runs, same machine, alternating with the AA binary) | 4.84-4.87 s | 4.76-4.81 s |

`--lle` hashes unchanged. All gates pass (end of this section).

### 1. `$C0B155` crash (d_08_r7424 f2526): two bugs

Compiled as object walker (`ABFB`) target, default mode diverged from `--lle` at f1656 and crashed at f2526.

- **Codegen bug, `(dp),Y` bank carry** (first WRAM difference, f1656 `$0B21`). `$C09BA6 LDA ($F8),Y` with DB=$CF,
  ($F8)=$xxxx, Y=$DF00 crosses into bank $D0. The 65816 adds Y to the 24-bit `DB:ptr`; the emitter wrapped `ptr + Y`
  to 16 bits and read bank $CF. (Found by logging A/X/Y/S/D/DB/P at every compiled block start vs every interpreted
  opcode: identical up to that read.) `$C09AB8` is shared, but only `$C0B155` reached it with such a Y. Fixed in
  codegen for `(dp),Y` and `(sr,S),Y` (`SegKind.DP_INDIRECT` with index, `STACK_REL_INDIRECT_Y`), the same 24-bit
  rule abs,X / long,X / `[dp],Y` already had: patch `0009-index-bank-carry-and-sep-x.patch`.
- **`$C07624` pushed-frame RTS** (crash). Level 8 code `$C3887A` calls `$C07624`, which calls `$C0B155` through
  `PHK / PEA $768E / LDA $C587BC / DEC / PHA / RTS` (handler RTLs to `$C0768F`). The compiled RTS at `$C0768E` falls
  back to `interp_tier_dispatch_balanced` only when the target has no compiled entry; once `$C0B155` had one, it
  took the tail dispatch `cpu_dispatch_pc_from`, so the handler's RTL ended `$C07624`: S 10 bytes low, X=1 leaked
  to `$C389E4`, BRK at `$C389FB`, then the BRK hook at `$70:8013` overwrote WRAM. Fix: `rts_call C0768E
  return:C0768F frame:3` (task U mechanism, same as `$C08763`, which calls the same `$C587BC` handler). The same
  frame at `$C0769E` (`$C0B5FD`) is past the compiled part of `$C07624` (it runs interpreted from `$C0768F`).
- Also in patch 0009 (found on the way, not the cause): SEP with bit 4, PLP, RTI and the long-call helper's SEP #$30
  now zero the high bytes of X and Y when x=1 (hardware does; the emitted `cpu_p_to_mirrors` did not).

### 2. Non-returning / non-local-exit callees (`$C0A05D`, `$C465F0`, `$C3887A`, `$C3883A`)

`scripts/check_exit_mx.py` extended:
- `LDX #imm16 / TXS` (S reset to a constant) ends the path: the caller's frame is gone, so it never returns there
  (`$C08122` at `$C08140`; soft reset `$C08000` -> `$C0800E`). `CLC / XCE` forgets both widths (native mode).
- A routine with no reachable return is reported `never returns`; its declared exit width is vacuous, and callers'
  paths end at the call.
- `PHK / PEA r / JML abs` is a long call returning to r+1 (`$C076A4 ... JML $C0A0F0` in `$C07624`).
- TRACED axiom `$C0A0F0` (task U manual proof: PEA/PHP into `$C0A0F7`'s body, PLP/RTS through the PEA to the RTL),
  also at M0X1. TLK_CALL_PAIR (`--lle`, l08 idle/random/right, d_08_r7424, 6987M/6781M/4735M polled TAS): return
  site `$C076AC` only ever M0X1 from M0X1; 18 ok, 4 never ran, 0 wrong.

New hints (all `check_exit_mx.py` ok): `C50179 0 0 0 0` (demo playback, PHP/PHD ... PLD/PLP/RTL; demo exit `JML
$C0890A` walks the main loop and only ends in S resets), `C50119 0 0 0 0` (never returns: `JML $C08122`),
`C07624 0 0 0 0`, `C3887A 0 0 0 0`. All four former `lle_only` entries compile.

**Polled input from compiled code.** `--input-polled` / `--ram-log` advanced the tape in an interpreter pre-opcode
hook at `$C0A07C` (`LDA $4218`). With `$C0A05D` compiled that hook never ran (t_ending.polled diverged at f592).
Runtime hunk `aot-pad-read-hook` (`cpu_state.hunks`): a compiled 8-bit read of `$4218` (outside the interpreter's
bus-timing window) first calls `g_tlk_aot_pad_read_hook` (`runner/aot_pace.c`), which `tlk_headless` and `tlk_sdl`
set to the same poll function when they arm the pre-opcode hook. Live play (no tape) is unaffected.

### 3. `$7E2006` (MVN/RTL RAM routine): not attempted

It is now the only interpreted work of note (264,640 + 15,189 of 870,038 interpreted ops on l01_right 3000 f), but
the time it costs is small: `--lle` vs default on the same 3000 f gives ~0.13 us per interpreted op, so ~37 ms of
2.87 s (~1.3%) is the ceiling for any replacement. A bulk copy still has to charge per byte exactly (code fetches at
WRAM speed, src/dst at their region speed, 2 internal cycles) and stop at the same byte as the interpreter on a
master deadline, IRQ, DRAM refresh or beam event, which is what the task V per-byte native hook did, and that was
slower than the interpreter. Left interpreted.

### Exit-width hints

`check_exit_mx.py`: 279 ok, 11 UNPROVEN (unchanged from task AA: `C0AD22`, `C00201`, `C0A0F0`, `C09EC6` x2,
`C07770`, `C0CD0E`, `C09BFD`, `C09C64`, `7E2006` x2), 0 WRONG.

### Files

`patches/snesrecomp/0009-index-bank-carry-and-sep-x.patch` (new), `cmake/runtime-patches/cpu_state.hunks`
(`aot-pad-read-hook`), `runner/aot_pace.c`, `runner/headless_main.c`, `runner/sdl_main.c` (compiled pad-read poll),
`recomp/bankc0.cfg` (ABFB +`C0B155`, `rts_call C0768E`, `C07624` hint), `recomp/bankc3.cfg` (`C3887A`),
`recomp/bankc5.cfg` (`C50179`, `C50119`), `scripts/check_exit_mx.py`, this file.

### Gates (final tree, real output)

```
600 MATCH 57ccb8d5b24918fd / 3000 MATCH 89a48d6c4f12621b
1200 MATCH dd861e164c295e15 / 6000 MATCH 946033bd10683657
play tapes @f5000: 71 MATCH, 0 MISMATCH
IDLE MATCH frames=12000 every=5 pictures=2400
IDENTICAL frames=3000 tape=build/playtapes/l01_right.tape native_ops=2854509 wram_frames_differing=0
death tapes, tlk_sdl default vs --lle every 10 f, whole tape: all 17 SAME (726-950 frame hashes each, full counts)
save f3000 -> load -> f6000: frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
t_continue_yes 5fd6d1cda59528fb == lle; t_continue_no 3e5eb7db18dd5385 == lle (f17000)
TAS default: 6987M in_sync_through_poll=36904, 6781M 37093, 4735M 38870
allbugsF 60648 f: default and --lle RAM logs byte-identical (cmp)
share 3.9% (AA binary on the same machine: 5.1%); l01_right 5000 f: 4.76 / 4.81 / 4.77 s (AA: 4.84 / 4.87 / 4.86)
```

## 2026-10-13: allbugsF-only code compiled (task EE)

### Result

| | before (task CC tree) | after EE |
|---|---|---|
| manifest entry pcs / compiled | 1,260 / 1,260 | **1,305 / 1,305** (1,378 nodes, all `aot_eligible`) |
| allbugsF default mode, interpreted ops (60,648 f) | 55,913,651 (non-idle 28,929,062 = 6.5% of `--lle`) | 52,493,504 (non-idle **25,508,915 = 5.8%**) |
| allbugsF interpreted ops outside any analysed body | 5,456,269 | 1,926,615 (1,198,017 of it `$7E2006`) |
| share l01_right 3000 f (`interp_share.py`) | 3.9% | 3.9% (these routines do not run there) |
| wall l01_right 5000 f, 3 runs | 4.76-4.81 s | 4.75-4.78 s |

Method: `tlk_headless --log` on allbugsF (default mode) lists every dispatch that found no compiled entry
(site, target, entry width, hits); `TLK_INTERP_HIST`/`TLK_MX_HIST`/`TLK_ROMMAP` on allbugsF (default and `--lle`)
for the before/after numbers. The DD regions are whole routines next to analysed ones, entered only through
pointers: object walker `$C0ABFB` (`JML [$0014]`), the object routine RTL dispatch `$C0D7F3`, the main-loop state
routine `$C08933`, the script VM `$C0AF52`, plus three JSL targets.

New cfg entries (44):
- `indirect_dispatch ABFB` 108 -> 114: `C38FA2 C400E4 C55A66 C581FA C58498 C63DC4`.
- `indirect_dispatch AF52` 290 -> 298: `C0DEBD C0EADC C239FE C23A52 C252B8 C26F8E C2788D C2808C` (VM ops ending in
  `JMP ($00A4)`, like the other table entries).
- `func ... entry_mx:0,1` (`$C0D7F3` / `$C08933` targets): `C0FC42 C0FCC6 C0FD2C C0FD9C C0FE4B`, `C18E95 C18EE3 C18F39
  C197CC C19802 C1983B C19874 C19CBC C19D10 C19D68 C19DC0`, `C43CD2 C43D25 C48DF7 C492F4`, `C59393 C593D3`,
  `C63FB6 C64019 C64040 C640C5 C641BB`.
- `func ... entry_mx:0,0` (JSL targets): `C54CF2` (recursive), `C63E38`, `C63F2A`.

No exit-width hints added: the analyser's own exit modes agree with `check_exit_mx.py` queries (all M0X1->M0X1 or
M0X0->M0X0; `C54CF2` needs `--induct`; the VM ops have no return). `check_exit_mx.py`: 279 ok, 11 UNPROVEN
(unchanged: `C0AD22 C00201 C0A0F0 C09EC6 x2 C07770 C0CD0E C09BFD C09C64 7E2006 x2`), 0 WRONG.

### Bug found: pushed-frame RTL call ran the target in the caller's bank

With `$C48DF7` compiled, allbugsF default mode left the `$A68C` frame allocator at +$2E at f30383 and crashed at
f30486; with `$C593D3` compiled, d_06_l9301 had one stack byte different (`$02E6` $C0 vs $C5) at f2650. Cause:
the `rts_call` RTL form (patch 0008, `$C0D7F3`) popped the target's bank but never set PB, so the target ran with
PB=$C0 and every JSL it made pushed $C0 as the return bank. New codegen patch
`0010-rtl-call-target-bank.patch`: PB = target bank for the call, caller's PB restored after. Both
cases now match `--lle`.

### What is still interpreted on allbugsF (default mode, outside analysed bodies)

- `$7E2006` RAM copy, 1.20 M ops (left by choice, task CC).
- `$C0B5A9-$C0B5BE` 0.57 M: the `$C0B5A9` indirect goto site itself; it jumps into level code (`$C30xxx-$C38xxx`,
  `$C45A5B` ...) at M0X1 where only M0X0 variants exist (`missing_exact_variant`, ~60 targets). Next candidate.
- `$C08764-$C0885C` (42 K) and `$C0768F-$C07724` (36 K): continuations after the pushed-frame RTS calls at
  `$C08763` / `$C0768E`; `rts_call` does not compile past the call, they run via `interp_tier_dispatch_balanced`.
  `$C087C1-$C08858` from the DD list is in here.
- `$C45072`, `$C4858D`, `$C1A4C8`, `$C47146`, `$C53E5E`, `$C580DF`: `$C0B5A9` goto targets (`missing_entry`),
  together ~65 K. `$C5DCAC` 11 K.

### Gates (final tree)

```
600 MATCH 57ccb8d5b24918fd / 3000 MATCH 89a48d6c4f12621b (wram same)
1200 MATCH dd861e164c295e15 / 6000 MATCH 946033bd10683657 (wram same)
play tapes @f5000: 71 MATCH, 0 MISMATCH
IDLE MATCH frames=12000 every=5 pictures=2400
native decomp l01_right 3000: IDENTICAL
17/17 death tapes tlk_sdl default == --lle every 10 f
save/load 6987M f3000 -> f6000: frame_sha256=946033bd...
t_continue_yes / t_continue_no f17000: MATCH
TAS 6987M 36904, 6781M 37093, 4735M 38870
allbugsF 60648 f --ram-log default vs --lle: identical (cmp)
check_allbugs.sh: default == --lle IDENTICAL (60768 frames), differing=110, end_in_sync=yes, ALLBUGS PASS
interp_share.py: 3.9%; l01_right 5000 f: 4.78 / 4.75 / 4.78 s
```

### Files

`recomp/bankc0.cfg`, `bankc1.cfg`, `bankc4.cfg`, `bankc5.cfg`, `bankc6.cfg` (entries above),
`patches/snesrecomp/0010-rtl-call-target-bank.patch` (new), this file, `docs/function_count.md`.

## 2026-10-13: pushed-frame continuations compiled; `$C0B5A9` goto NOT applied (task FF)

### Result (final tree)

| | before (task EE tree) | after FF |
|---|---|---|
| manifest entry pcs / nodes | 1,305 / 1,378 | 1,305 / **1,380** (all `aot_eligible`) |
| allbugsF default, interpreted ops (60,648 f) | 52,493,504 (non-idle 25,508,915 = 5.8% of `--lle`) | 51,615,215 (non-idle **24,630,626 = 5.6%**) |
| `$C08764-$C0885C` / `$C0768F-$C07724` (allbugsF) | 41,864 / 36,365 | 11,953 / 519 |
| share l01_right 3000 f (`interp_share.py`) | 3.9% | 3.8% |
| wall l01_right 5000 f, 3 runs | 4.75-4.78 s (EE report) | 4.80-4.83 s (same order; machine noise) |

### 1. Continuations after pushed-frame calls (applied)

Codegen patch `0011-rts-call-continuation.patch` (Rust + Python decoders, emitter, codegen):
- An `rts_call` site (RTS / RTL / JML) gets its `return:` pc as a decode successor (site widths), so the
  continuation is a compiled label of the same routine; the emitted goto still checks live M/X and falls
  back to `interp_tier_dispatch_balanced` on any other width.
- `rts_call` on a JML (`PHK / PEA r-1 / JML t`): emitted as `cpu_dispatch_call_pc_pushed(t, frame:3)` +
  goto continuation (PB set to the target bank for the call).
- `rts_return <rts_site> return:<ret> via:<jmp_site>`: `PEA ret-1 / PHP / JMP body`; the JMP gets `ret` as a
  second successor (PHP token popped), and the shared body's RTS checks the live frame (popped word ==
  ret-1, PB == bank) -> pop + goto; else the normal RTS. Hardware-exact.
- `ptrcall_tail <site> targets:...` (codegen only, unused in the final cfg, see 2).

cfg (`bankc0.cfg`): `rts_call C08773 return:C08774 frame:3`, `rts_call C0769E return:C0769F frame:3`,
`rts_call C076A8 return:C076AC frame:3` (JML `$C0A0F0`), `rts_return C0A2E7 return:C08780 via:C0877D`.
First attempt gave the RTS successor the JMP's PHP token: the loader functions blew the 4,096-insn decode
budget (`C08258/C08259/C082BB` lle_only); popping the token fixed it.

Exit-width fixes (proven, `check_exit_mx.py` ok): blanket `exit_mx_at X 0 0` replaced by per-variant
`0 0 0 0` + `0 1 0 1` for `C3857F C34CFC C34E86 C36797` (bankc3), `C465F0 C4690E` (bankc4), `C53B1F`
(bankc5): they were WRONG for M0X1 (PHP ... PLP, exit = entry). `check_exit_mx.py`: 282 ok, 11 UNPROVEN
(unchanged list: C0AD22 C00201 C0A0F0 C09EC6 x2 C07770 C0CD0E C09BFD C09C64 7E2006 x2), 0 WRONG.

### 2. `$C0B5A9` goto (tried, reverted; cfg lines kept commented)

`$C0B4FF` calls the object's level routine with `PHK / PEA $B5AB / JML [$0014]` (M0X1). As a ptrcall
(`indirect_dispatch B5A9 66 ptrcall return:B5AC frame:3`, 66 targets from allbugsF) the targets got M0X1
variants and allbugsF interpreted ops fell to 42.6 M (non-idle 15.7 M, `$C3xxxx` 7.75 M -> 2.5 K) with
default == `--lle` on allbugsF and 6987M. But targets that never return through the frame break it:
- `$C47146` (`JSL $C08122`, S reset + level loader): 6987M crashed at f942 (BRK $3A:4018). Proven by an
  in-place generated-code switch: old tail path for that target only -> MATCH.
- `$C462F6/$C465F0/$C4690E` -> `$C50119` (demo exit `JML $C08122`): attract crashed f2557.
- Still after `ptrcall_tail` for those four: `attract`/`t_attract` crash at f4234, ~120 frames after the
  last call (`$C32389`, which runs the script VM `JSL $C0AF09`; the VM can end the level/demo, so the set of
  non-returning targets is not static). Not fixed in the timebox.
Fix direction: keep `$C0B5A9` a tail transfer but give the runtime compiled M0X1 bodies to tail into (extra
roots), or make the ptrcall survive an S reset inside the callee (hand to scheduler like the interpreter
frame does, task Q). Also a vacuous `exit_mx_variant C08122 0 0 0 0` must not be declared (it makes
`$C47146` compile and run the loader nested in compiled frames).

### What still runs interpreted (allbugsF default, non-idle 24.6 M)

- `$C0B5A9` goto + level routines `$C3xxxx` (7.75 M) and `$C45072`/`$C5DCAC` (~20 K, reached from that
  interpreted code): section 2.
- `$7E2006` RAM copy 1.20 M (by choice, task CC).
- `$C0D7F3` RTL dispatch at M0X1 to targets with only M0X0 bodies (`$C490E1 $C19E18 $C191CC ...`, ~10 K
  calls): same class as `$C0B5A9`, not attempted.
- `$C08764-$C0885C` 12 K: the continuation when the loader itself runs in the interpreter (entered from
  interpreted code), not from the compiled routine.
- Hot pages `$C09E00` 3.1 M, `$C08800` 2.4 M, `$C0AB00` 1.9 M are interpreter frames under the above
  (NMI/loader code run inside interpreted calls).

### Gates (final tree, regenerated)

```
600 MATCH 57ccb8d5b24918fd / 3000 MATCH 89a48d6c4f12621b (wram same)
1200 MATCH dd861e164c295e15 / 6000 MATCH 946033bd10683657 (wram same)
play tapes @f5000: 71 MATCH, 0 MISMATCH
IDLE MATCH frames=12000 every=5 pictures=2400
native l01_right 3000: IDENTICAL
17/17 death tapes tlk_sdl default == --lle every 10 f (726-950 hashes each)
save/load 6987M f3000 -> f6000: frame_sha256=946033bd106836571fc087ed3b456e1fde7b818ed765260f3cc5ec631ae918bb
t_continue_yes 5fd6d1cda59528fb / t_continue_no 3e5eb7db18dd5385: default == --lle
TAS 6987M 36904, 6781M 37093, 4735M 38870
allbugsF 60648 f --ram-log default vs --lle: identical (cmp)
check_allbugs.sh: differing=110, end_in_sync=yes, level_mismatch=0, ALLBUGS PASS
interp_share.py: 3.8%; l01_right 5000 f: 4.83 / 4.82 / 4.80 s
```

### Files

`patches/snesrecomp/0011-rts-call-continuation.patch` (new), `recomp/bankc0.cfg`,
`recomp/bankc3.cfg`, `recomp/bankc4.cfg`, `recomp/bankc5.cfg`, this file, `docs/function_count.md`.

## Task WIN: Windows x86_64 cross build (MinGW-w64), checked under Wine

`cmake/mingw-w64-x86_64.cmake` + `-DTLK_STATIC_SDL=ON` builds `build-win/tlk_headless.exe` and `tlk_sdl.exe` with
x86_64-w64-mingw32-gcc 16.2. SDL3 3.4.10 and the MinGW runtime are static (snesrecomp's
`SNESRECOMP_STATIC_RUNTIME` adds `-static`); imports are Windows system DLLs and the UCRT `api-ms-win-crt-*` set only.
The generated C and the codegen are untouched.

- `runner/tlk_fiber.c`: a Win32 fiber backend (`ConvertThreadToFiber`, `CreateFiberEx` with the same 64 MB stack
  reserve, `SwitchToFiber`) beside ucontext; same switch points. Wine: `switches=674 resumes=673 aborts=0` at
  f600, as on Linux.
- `runner/tlk_compat.h`: `setenv` (via `_putenv_s`) and a one-argument `mkdir` on Windows.
- Logs (`--ram-log`, `TLK_FRAME_LOG`, `TLK_HWLOG`, ...) open `"wb"`: in text mode the Windows build wrote CRLF and the
  All Bugs RAM log was not byte-identical (same content otherwise).
- `sdl_main.c`: `\` accepted as a separator for the slot/recording paths and the ROM search; the remap self-test
  falls back to `%TEMP%` when `TMPDIR` is unset.
- `CMakeLists.txt`: the Linux-only SDL backend options are not forced on Windows.
- `scripts/package_win.sh` -> `dist/TLKRecomp-windows-x86_64.zip` (stripped `tlk_sdl.exe` + notes, CRLF), refuses
  non-system DLL imports.
- LLP64: `long` holds frame counts (< 2^31) and `unsigned long` stats/`g_tlk_fiber_gen` (compared for equality
  only); none needs 64 bits. Formats match their types.

Wine 11.17, Windows exe vs Linux `build/tlk_headless`, identical frame and WRAM hashes: no input 600/3000 and 6987M
1200/6000, default and `--lle`; 15 play tapes at f5000 (l00-l13 mix, d_03/d_09/d_13, t_continue_yes/no, t_pause,
gameover); save at 3000 / load in a new process -> 6000 `946033bd10683657` (also a Linux-written state loaded by the
exe); allbugsF 60,648 frames `--ram-log` byte-identical (sha256 `7b6094ab...`). `tlk_sdl.exe` 6987M `--hash-at
600,1200,3000` = Linux `tlk_sdl`; `--selftest-remap` PASS. Savestate files differ in 6 bytes between any two runs
(a host pointer inside the rollback blob; also between two Linux runs), not a platform difference.

Not verified: real Windows, audio, gamepads, the file dialog (the "Choose ROM" message box renders under Wine/Xvfb),
pre-Windows-10 systems (UCRT).

## Task VIDEO: shaders, aspect and integer scaling in tlk_sdl

`tlk_sdl` now presents through snesrecomp's desktop OpenGL 3.3 presenter (`desktop/opengl.c`, `glsl_shader.c`,
CMake `snesrecomp_target_opengl` + `snesrecomp_target_shader_presets`) instead of a plain SDL texture. The
emulation path is unchanged: still the headless host contract and frame driver, not the framework desktop host.

- Settings menu and ini: Shader (off, CRT, LCD grid, Sharpen, Composite: snesrecomp's CC0 presets, or any
  `.glslp`/`.glsl` path in the ini), Aspect (8:7 square pixels, the default and the old picture; 4:3 TV; 1:1),
  Smooth (bilinear), Int. scale. Window width follows the aspect (`SnesDisplayAspect_ComputeWindowWidth`).
- The menu and toasts are drawn in software into a separate `s_screen` buffer, so both presenters show them and a
  shader treats them like the picture. `s_pixels` (hashes, `--dump-png`, the gates) is never drawn on.
- No OpenGL 3.3 (or `opengl = 0`): a probe context fails, the window is recreated with the SDL renderer, and the
  Shader row reads "no OpenGL". The dummy video driver (`--selftest-remap`) takes this path.
- `runner/tlk_video.c` owns `g_config`: `config.h` pulls in `sdl_compat.h`, which needs SDL3's old-name aliases
  and does not compile inside `sdl_main.c`.
- Install/packages: `assets/shaders/` goes next to the binary (`install(DIRECTORY ...)`, both package scripts);
  `opengl32.dll` added to the Windows import allow-list.

Evidence (2026-10-17):

```
SDL_VIDEO_DRIVER=dummy tlk_sdl --selftest-remap                 remap_selftest: PASS (0 failed)
f3000 no input: tlk_headless / new tlk_sdl (dummy) / old tlk_sdl  frame 89a48d6c... wram 0043be7f... (all three)
Xvfb + llvmpipe, title screen at ~24 s: off 8:7 768x672, CRT/LCD/Sharpen/Composite 4:3 896x672, menu: looked at
scripts/package.sh       8:07, rc=0, tarball has assets/shaders
scripts/package_win.sh   8:29, rc=0, zip has assets/shaders, imports pass
unpacked tarball (x11, Xvfb) and zip (Wine, Xvfb), run from /tmp, CRT 4:3: shader applied in both;
  Linux package --frames 1700: fps=60.13
```

Owner report: "theres a shader that makes it all black". Reproduced on Xvfb with an XTest key injector cycling
the menu from CRT: every preset fine, then "off" fully black (capture mean 0). `GlslShader_Render` points its vertex
attributes at its own buffer on the bound VAO and then disables them; that VAO is `g_VAO`, whose attributes 0/1 the
no-shader program draws with. Fix: `cmake/runtime-patches/opengl.hunks` gives the shader path its own VAO and
rebinds `g_VAO` after it. `opengl.c` is added by `snesrecomp_target_opengl`, not the runner source list, so
`tlk_use_patched_target_sources(tlk_sdl)` swaps desktop copies on the target (fails if the original is missing).

```
menu cycle from CRT, capture mean   before: CRT .247 LCD .221 Sharpen .281 Composite .277 off 0
                                    after:  CRT .247 LCD .221 Sharpen .281 Composite .277 off .279, CRT .225, off .279
SDL_VIDEO_DRIVER=dummy tlk_sdl --selftest-remap                 remap_selftest: PASS (0 failed)
```

Not verified: real GPU
drivers and a real Windows machine, fullscreen. `lcd-grid-pass.glsl` (snesrecomp) mostly darkens the picture: its
`edge` term uses `smoothstep(0.84, 0.96, cell)` where `1 - smoothstep` looks intended, so the grid barely shows.
Not patched here (submodule). A full build of both hosts takes ~7 min; each package ~8 min.

## Task TURBO: hold-to-turbo key in tlk_sdl

Owner asked for a turbo key. Hold `` ` `` (bsnes's default; `turbo =` in the ini, any SDL key name) to run
`turbo_speed` (default 4, 2-16) times faster. Emulation still steps one frame at a time with the same pad word, so
recordings, tapes and hashes are unchanged; while held only every Nth frame is presented and paced, and the sound is
muted (still rendered, so the resampler stays fed; underruns not counted). A ">>4x" tag shows top right. Released
on focus loss and when the menu opens. Turbo is binding 13 on the Controls page (`kTurbo`, after the 12 SNES
buttons; not a pad bit): rebind to a key or a pad button, default `` ` `` / right stick click (R3), saved as
`turbo =` / `pad_turbo =`. Rows moved up 8 px so the page still fits above its footer.

Evidence (2026-10-17, Xvfb + llvmpipe, SDL dummy audio, `--frames 1500 --hash-at 1500`, key held via XTest from 3 s):

```
no turbo          wall 24.9 s  frame d55aba8a... wram 8b65177d...
turbo 4x          wall  8.3 s  same hashes        (expected ~8.5 s)
turbo 8x (ini)    wall  5.6 s  same hashes        (expected ~5.8 s; turbo = ` read back from the ini)
SDL_VIDEO_DRIVER=dummy tlk_sdl --selftest-remap   remap_selftest: PASS (0 failed), 11 new turbo checks: ` and
  rebound T hold turbo without a pad bit, Controls row rebind, save/reset/reload, virtual pad R3 holds turbo
Controls page on Xvfb (--menu-at 300 --menu-page controls): turbo row fits, looked at
```

Not verified: a real audio device (dummy driver only), a real gamepad (virtual pad only), how fast a slower CPU gets.

## Task PRACTICE: speedrun practice mode in tlk_sdl

Written on 2026-10-14 in the `speedrun-practice` worktree and left staged, never committed; the owner missed it on
2026-10-17. Rebased onto main (video + turbo). "Practice" in the settings menu or `speedrun = practice`: level timer
(SBDWolf practice hack rules: starts after the fade-in, skips fades and pause, stops on level end or a death), 6987M
TAS level time as a target, run timer, lag counter, checkpoint splits, input display, best times in
`tlk_practice_times.txt`, F9 restarts the level from an automatic state, and a Level row that replays the debug-menu
level select from a power-on state. Reads WRAM only. Port to main: the overlay is now drawn in software into
`s_screen` (task VIDEO), so the half-size SDL-renderer font is gone; the bar is two rows of the 8x8 font between the
health and roar meters, hidden while the menu is open. The settings rows moved to 10 px spacing to fit 15 rows. The
level-select macro fast-forwards through the turbo path (every 30th frame shown, muted). `--dump-window` now writes
`s_screen`.

Evidence (2026-10-17, worktree `TLKRecomp-sr`, generated/ from main, SDL dummy video):

```
compare_modes.sh 600 3000 / -t 6987M.tape 1200 6000    MATCH x4, WRAM same (57ccb8d5, 89a48d6c, dd861e16, 946033bd)
check_all_tapes.sh -s                                  71 MATCH, 0 MISMATCH
check_idle_lle.sh                                      IDLE MATCH frames=12000 every=5
check_native_decomp.sh 3000                            IDENTICAL
save f3000 / load / f6000                              946033bd10683657
TAS sync 6987M / 6781M / 4735M                         36,904 / 37,093 / 38,870
check_allbugs.sh                                       ALLBUGS PASS
check_exit_mx.py                                       0 WRONG
pytest tools/tests                                     27 passed
tlk_sdl --selftest-remap                               PASS (0 failed), incl. 9 practice checks
tlk_sdl 6987M.tape f6000, practice off vs on           both 946033bd... / wram 87fa6746... (practice changes nothing)
tlk_sdl 6987M.read.tape polled, practice               Pridelands 0:23.13 (= built-in TAS table), CP +0:00.00
tlk_sdl --practice-level 3 --frames 2600               frame 61f6953b... = l03_idle.tape at f2600 (headless)
--dump-window: bar, input display, menu                looked at
```

Full build of `snesrecomp_game tlk_headless tlk_sdl` from clean: 22.6 min. Not verified: a real window, real key
presses (F9 restart was not exercised), a real gamepad, best times across sessions.

## Task UI: recomp-ui's in-game menu in tlk_sdl

The owner asked for [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui) (MIT) instead of our own settings
menu; only the in-game menu, not its pre-boot launcher. recomp-ui is a submodule pinned at `0546fbc`. TLK supplies
the rows and callbacks (`runner/sdl_main.c`: Display, Graphics, Audio, Practice, Controls, System) and the Dear ImGui
glue (`runner/tlk_menu_imgui.cpp`). The OpenGL presenter swaps inside `EndDraw`, so a runtime hunk
(`opengl.hunks`, overlay-decl / overlay-draw) lets a host draw between the game quad and the swap; the menu goes
there, over the shader. Without OpenGL the menu draws its compact form into the 256x224 picture.

Behaviour changes: every change is applied and saved at once ("Save settings" is gone); Esc goes back and closes
instead of quitting, Quit is System > Quit pressed twice; Level + Start level replace the Level row; new Game dimming
and Menu opacity rows (`menu_dim`, `menu_opacity` in the ini) so a shader can be seen through the menu. Action rows
show recomp-ui's fixed "OPEN". `--dump-window` with OpenGL now reads back the composited window at window size.

Host-side fixes after the owner found it cramped: recomp-ui selects the row under the mouse every frame, so a
resting pointer undid every key press (Down x4 + Right changed Aspect, not Game dimming); the mouse now counts only
after it moves (`ImGuiConfigFlags_NoMouse` until motion/click, back on any key or pad button). Font 18 px (the theme's
size, was 20), rasterized at the window's pixel density. Shorter descriptions. Below 768x672 recomp-ui's footer runs
into Resume; the menu is laid out at 768x672 and shrunk to fit (scale 2 clean, scale 1 still too small to use).
FreeType lookup disabled (chat emoji only; the tarball must not need libfreetype). Packages add
`assets/fonts/LatoLatin-Regular.ttf` and LICENSE-recomp-ui/-imgui/-Lato; the Windows allowlist adds d2d1, dwrite,
windowscodecs (recomp-ui's Win32 emoji code).

Evidence (2026-10-18, worktree `rui`, generated/ from main):

```
compare_modes.sh 600 3000 / -t 6987M.tape 1200 6000    MATCH x4, WRAM same (57ccb8d5, 89a48d6c, dd861e16, 946033bd)
check_all_tapes.sh -s                                  71 MATCH, 0 MISMATCH
check_idle_lle.sh                                      IDLE MATCH frames=12000 every=5
check_native_decomp.sh 3000                            IDENTICAL
save f3000 / load / f6000                              946033bd10683657
tlk_sdl 6987M.tape f6000 (dummy video)                 946033bd... / wram 87fa6746...
check_exit_mx.py                                       0 WRONG
check_allbugs.sh                                       ALLBUGS PASS
pytest tools/tests                                     23 passed, 4 skipped (no ../tas in the worktree)
tlk_sdl --selftest-remap                               PASS (0 failed), rewritten for the new menu (incl. Esc, Quit x2)
Xvfb + XTest key presses, scale 3 and 2                Display, Practice, Audio, Controls (waiting), "Saved": looked at
menu over crt-soft, dim 20 / opacity 40                looked at
cmake --install: binary + font from the install dir    same pixels as the build tree
MinGW build: imports                                   system DLLs only (adds d2d1, DWrite)
wine tlk_sdl.exe --selftest-remap                      PASS (0 failed)
wine tlk_sdl.exe --menu-page controls --dump-window    looked at
```

Full clean build: 22.5 min (Linux), 20.0 min (MinGW, niced, in parallel with the gates). Not verified: the owner's
desktop with real keys and a real pad, the release tarball/zip (`package.sh`/`package_win.sh` not run), HiDPI, TAS
sync vs BizHawk (game side untouched), Windows hash gates under Wine.

## Task HD7: HD Mode 7 option in tlk_sdl

The owner asked for snesrecomp's opt-in HD Mode 7 (`snesrecomp/docs/HD_MODE7.md`, already in the pinned submodule)
as a video option. Graphics > HD Mode 7 (off, 2x, 3x, 4x; `hd_mode7 = 0|2|3|4` in the ini) binds a host surface with
`PpuBindMode7HdSurface`; the PPU then draws Mode 7 lines into it at that scale and copies every other line pixel for
pixel. `Present` shows that surface instead of the 256x224 frame, on both presenters (OpenGL: `BeginDraw` at the HD
size; plain SDL: a texture of the HD size). The toast, practice display, turbo tag and in-picture menu are drawn in
256x224 as before and scaled up over it (pixels where `s_screen` differs from `s_pixels`). The viewport maths runs on
the native size, so Aspect and Integer scaling count game pixels. Off is the default and binds nothing.
`s_pixels` (hashes, `--dump-png`, the gates) is not touched; the binding is not in savestates.

**The Lion King never uses Mode 7.** A temporary probe in `ppu_runLine` (reverted, not committed) counted visible,
non-blanked lines with BG mode 7: 0 on all four speedrun tapes to the end (6987M, 6781M, 4735M, All Bugs Final) and
0 on the 45 local play tapes (every level, attract, game over). 6987M uses BG modes 0 and 1 only, Stampede included.
So the option changes nothing in this game; it costs CPU time per frame and gives a shader a 2-4x larger input
(CRT scanlines get denser). Landed on request; it is there for other content (the DK mod) and is off by default.
The HD resampling itself was never seen on a Mode 7 scene here.

Evidence (2026-10-18, worktree `hdm7`, generated/ from main):

```
compare_modes.sh 600 3000 / -t 6987M.tape 1200 6000    MATCH x4, WRAM same (57ccb8d5, 89a48d6c, dd861e16, 946033bd)
check_all_tapes.sh -s                                  71 MATCH, 0 MISMATCH
check_idle_lle.sh                                      IDLE MATCH frames=12000 every=5
check_native_decomp.sh 3000                            IDENTICAL
save f3000 / load / f6000                              946033bd10683657
tlk_sdl 6987M.tape f6000 (dummy video)                 946033bd... / wram 87fa6746...
tlk_sdl --selftest-remap                               PASS (0 failed)
check_exit_mx.py                                       282 ok, 11 UNPROVEN, 0 WRONG
pytest tools/tests                                     23 passed, 4 skipped
check_allbugs.sh                                       ALLBUGS PASS (110 differing <= 150, 26 level starts, 0 mismatch)
Xvfb, OpenGL, title f1500, --dump-window               off / 2x / 4x pixel-identical, looked at
Xvfb + XTest keys: Graphics > HD Mode 7 2x -> 3x       "Saved", game shown at 4x after closing, back to off: ini 0
Xvfb, opengl = 0, menu in picture at f700              off / 2x pixel-identical, looked at
MinGW build (17:53, niced, beside the gates)           exit 0, tlk_sdl.exe + tlk_headless.exe
wine tlk_sdl.exe --selftest-remap, hd_mode7 = 2        PASS (0 failed)
wine tlk_sdl.exe, Xvfb, f1500 --dump-window            off / 2x pixel-identical, looked at
```

Not verified: a Mode 7 scene (none exists in this game), the owner's desktop, the release packages, TAS sync vs
BizHawk (game side untouched).

## Task LVL: level select apart from practice mode, with INVUL

The menu's Level and Start level rows sat in the Practice section and were greyed out with practice off. They now
have their own section, Level select (between Audio and Practice), usable either way, plus an **Invulnerable** row
(Off / Infinite health / Infinite lives): the debug menu's INVUL option. Picking a level or INVUL does nothing by
itself, so the action row is labelled `Start <level>  [Enter/A]` and changing either row puts "Now Enter/A on Start"
in the footer instead of "Saved" (neither is kept in the ini). `--practice-level N` no longer needs
`--speedrun practice`; `--invul N` goes with it, and `--menu-page level` opens the section for screenshots.

**The tape recipe had INVUL wrong.** The debug menu's cursor starts on LEVEL, not INVUL: with A x k right after
B A R R Y, `tlk_sdl --practice-level 3 --invul k` gave level 3 + k and `$7FFF9C = 0`. The macro now presses Up first
(only when k > 0, so the INVUL-off input is unchanged); `tools/re/make_level_tape.py` and `level_select.md` fixed the
same way. What the values do, from the disassembly: 1 refills health `$2004` from `$7FFFAC` every frame while it is
non-zero (`$C00201`), 2 stores 3 in lives `$7FFFAA` before the death path decrements it (`$C08AD9`).

Evidence (2026-10-18, main working tree, existing generated/):

```
tlk_sdl --practice-level 3 --invul 0 --frames 2600    frame 61f6953b... (same as task PRACTICE, l03_idle.tape)
same, --invul 0/1/2, state at f2600 -> headless       $7FFF9C = 0 / 1 / 2, $7FFF9E = 3 for all
idle from f2600 to f10000 (Stampede)                  off: lives 8 -> 2; 1: health 4, lives 8, reaches level 4;
                                                      2: lives held at 2
tlk_sdl --selftest-remap                              PASS (0 failed), also with an empty ini
pytest tools/tests                                    27 passed
Xvfb, OpenGL, --menu-page level --dump-window         section and three rows drawn, looked at
```

The self-test read `speedrun = practice` from the player's `tlk_sdl.ini` in the working directory and failed 5
practice checks on clean main before this change; it now sets practice off before
those checks. recomp-ui's ImGui backend shows "OPEN" as the value of every action row, Start included; changing that
means editing the submodule.

Not run: the emulation gates (compare_modes, tapes, allbugs: `tlk_headless` does not build `sdl_main.c`), the MinGW
build, a real key press in the owner's window.

## Task BLD: faster builds

A cold `./build.sh` took 13:41 (measured in a worktree on 2026-10-18). Two causes:

- **Each host compiled the generated C again.** `snesrecomp_game`, `tlk_headless` and `tlk_sdl` each built all 59
  generated files: 177 compiles. `snesrecomp_game` is now an OBJECT library that both hosts link, so 59 (an OBJECT
  library also keeps the module-registration constructor that a static archive could drop). CMake puts `ccache` in
  front of the compiler when it is installed (`-DTLK_USE_CCACHE=OFF`, or an explicit launcher, overrides it).
- **HiROM code landed in one file per bank.** snesrecomp shards a bank by entry PC with
  `part = max(0, (pc16 - 0x8000) // span)`, which assumes LoROM. In Lion King, every entry in `$0000-$7FFF` went to
  part 00, so each `bankXX_part00_v2.c` covered 34 KiB of ROM instead of 2 KiB: `bankc3_part00` was 14 MB and took
  1,008 s to compile alone, which set the build's length. Patch `0012-hirom-low-half-shards.patch` gives
  `$0000-$7FFF` its own parts after the `$8000-$FFFF` ones (part 10 = `$0000-$07FF`; LoROM numbering unchanged).
  59 files became 140; the largest is 4.4 MB (`bankc0_part00`, 185 s). The split happens after emission, so the
  generated functions are the same, only grouped differently.

Evidence (2026-10-18, worktree, 12 threads, regenerated with patch 0012):

```
./build.sh, CCACHE_DISABLE=1, empty build/        442 s (was 13:41)
clean, then rebuild with ccache warm              345 compiles from cache, ~1 s (fill run: 433 s)
MinGW build-win, cold                             460 s
compare_modes 600 3000 / -t 6987M 1200 6000        MATCH, WRAM same (57ccb8d5, c897f389, dd861e16, 946033bd)
same under Wine (build-win/tlk_headless.exe)      MATCH, same four hashes as Linux
check_all_tapes -s                                71 MATCH, 0 MISMATCH
check_idle_lle / check_native_decomp 3000         IDLE MATCH / IDENTICAL
save f3000 / load in new process / f6000           946033bd10683657
ram_sync_compare 6987M / 6781M / 4735M            36,904 / 37,093 / 38,870
check_allbugs                                     ALLBUGS PASS
check_exit_mx                                     282 ok, 11 UNPROVEN, 0 WRONG
pytest tools/tests                                27 passed
tlk_sdl --selftest-remap (SDL dummy)              PASS (0 failed)
tlk_sdl, Xvfb, 6987M f6000 --dump-window          level 1, Simba on the ledge, looked at
20,000 frames of 6987M, old vs new binary, 3x     19.84-20.00 s vs 19.66-20.37 s, same f20000 hash
```

Not run: the setup-host configuration (`-DSNESRECOMP_SETUP_HOST=ON` with an empty `generated/`), which now
compiles `setup_host_dispatch.c` in the game library; TLK does not ship one. Not tested: real Windows, the release
packages, the owner's desktop.

## Task REL: release builds for Linux, Windows and macOS

A `v*` tag now builds and publishes the players (`.gitea/workflows/release.yml`). The ROM never leaves the build
machines; repo variables `TLK_ROM` / `TLK_ROM_MAC` give its path on each.

- teresa (host runner, `docker-build`): `scripts/release.sh` generates, builds Linux and MinGW, compares compiled
  mode with `--lle` on both (Windows under Wine) at attract 600/3000 and 6987M f6000, requires Linux and Windows
  hashes to agree, then packs. `compare_modes.sh -p TAPE` (new) feeds `*.read.tape` with `--input-polled`.
- macOS arm64: `scripts/package_mac.sh` (static SDL3, only system frameworks linked, ad-hoc re-signed after strip).
  `runner/tlk_fiber.c` needed `_XOPEN_SOURCE`/`_DARWIN_C_SOURCE` for ucontext and a `MAP_STACK` fallback; the
  fibers work on arm64. Built on the MacBook's own runner (repo-scoped, `~/tlkrecomp-ci`, launchd agent
  `com.kinduff.tlkrecomp-runner`, cmake/ninja/node as nix out-links in `~/tlkrecomp-ci/tools`). By the owner's call
  the Mac build is only built and run, not compared.
- publish (teresa): one release with all three packages and SHA256SUMS on Gitea, then on the GitHub mirror with
  the `GH_RELEASE_TOKEN` secret once the hook has pushed the tag.

Evidence (2026-10-18):

```
scripts/release.sh, worktree, cold (no ccache hits)   14:20
linux / windows (Wine) attract 600, 3000            MATCH, WRAM same (57ccb8d5, c897f389)
linux / windows (Wine) -p 6987M.read.tape 6000      MATCH, WRAM same (a07184e1)
Mac generate + build (8 threads, cold)              ~15 s + ~60 s
Mac tlk_headless attract 3000 / 6987M polled 6000   result=completed, pictures right (title, level 2)
Mac packed zip, ROM beside tlk_sdl, 600 frames      found the ROM, 60.2 fps, codesign -v ok
Mac tlk_sdl 6987M 1500 frames from ssh              59.9 fps, rc 0; window not seen (screencapture denied)
```

Not run: the full gates (this change touches only the Apple branch of `tlk_fiber.c` and a new option in
`compare_modes.sh`). Not tested: the macOS build on any other Mac, Gatekeeper on a downloaded zip.

## Task PKG: AppImage, single .exe and macOS .app

Releases now carry one-file downloads next to the folder archives: `TLKRecomp-linux-x86_64.AppImage`,
`TLKRecomp-windows-x86_64.exe` and `TLKRecomp-macos-arm64.dmg` (TLKRecomp.app). Icon: original art, a paw print
in front of a sun (`tools/make_icon.py`, outputs in `packaging/icon/`).

- **Assets without a folder.** `tlk_sdl` now has its shader presets and menu font compiled in
  (`cmake/TlkEmbedAssets.cmake`, read by `runner/tlk_assets.c`). The folder next to the binary still wins; without
  one (the lone .exe) the files are written to `SDL_GetPrefPath` (`%APPDATA%\TLKRecomp`) and used from there.
  Bundled shader names in the ini are unchanged (`assets/shaders/...`); `TlkVideoConfig` resolves them.
- **Where settings go.** Unchanged when the current directory is writable. A .app started from Finder runs in
  `/`, so when it is not writable the ini (and with it slots, practice files, `last_session.rec`) goes to the
  per-user folder (`~/Library/Application Support/TLKRecomp`). The AppImage's AppRun sets `TLK_SDL_INI` beside
  the .AppImage file (snesrecomp's own AppImage convention). The ROM search now also looks next to the ini.
- **Packing.** `scripts/package_appimage.sh` (pinned appimagetool 1.9.1 and type2 runtime 20251108, SHA-256
  checked, extract-and-run so no FUSE); `package_win.sh` also writes the lone .exe (icon via `packaging/tlk_sdl.rc`);
  `package_mac.sh` also builds the .app (assets in `Contents/Resources`, where `SDL_GetBasePath` points in a
  bundle, `.icns` from the PNG, ad-hoc signed) inside a .dmg with an Applications link.

Evidence (2026-10-18):

```
scripts/release.sh, ccache warm                          1:57, all checks MATCH (57ccb8d5, c897f389, a07184e1)
packed AppImage / lone .exe (Wine), dummy video, f600     57ccb8d5, same as the checks
lone tlk_sdl, no assets/, Xvfb, crt-soft in the ini       files unpacked to $XDG_DATA_HOME/TLKRecomp, scanlines visible
same, --menu-at                                           menu drawn with Lato from the unpacked font
same, started in / without --rom, ROM in per-user folder  ROM found there
AppImage from its folder (real FUSE), ROM + ini beside    ROM found, CRT on, frame 400 1dd76aa3
lone .exe under Wine, fresh prefix, Xvfb                  ROM beside it found, assets in %APPDATA%\TLKRecomp, CRT on, f400 1dd76aa3
.dmg on the MacBook: mount, codesign -v --deep            ok; Info.plist 0.2.0; assets in Contents/Resources
.app binary run from / and /tmp                           60.2 fps; no unpacking (Resources found)
tlk_sdl --selftest-remap (SDL dummy)                      PASS (0 failed)
pytest tools/tests                                        23 passed, 4 skipped (TAS folder not reachable from the worktree)
```

Not tested: Finder double-click of the .app and Gatekeeper on a downloaded .dmg, the .exe on real Windows, the
AppImage on another distro (built on Arch: it needs a glibc at least as new as this machine's).

## Task WIDE: widescreen with a host compositor

The owner asked for widescreen "using the same enhanced renderer technique that megamanx uses". Read from
mstan/MegaManXSNESRecomp (cloned to `../ext/`, `docs/custom-renderer-spike.md`, `src/mmx_renderer.c`): the shared PPU
stays 256 pixels wide and authentic; each visible line's PPU state is snapshotted; a separate Mode 1 compositor draws a
wider picture from the snapshots, sampling the margins from data the game keeps (its level maps, ROM metatiles) instead
of the PPU's rolling tilemap, and drawing sprites from the game's piece submissions captured before its screen clip,
with signed coordinates. MMX layers ~2,000 lines of per-stage repairs on top; none of that is copied, only the
technique. `runner/tlk_wide.c` is TLK's version.

**What TLK keeps that the margins can use** (checked on savestates of all nine platform levels, f1500-f40000 of 6987M):

- BG1 (foreground): the native tilemap is 64x32 at VRAM `$4000`, a ring the game streams as the camera moves. The level
  map is copied to `$7F0000` at load (`NOTES.md` +09); cell `& $FFF8` indexes the ROM block table (+0F) for four
  tilemap words. Rebuilding the visible tilemap from map + block table matched VRAM on 957 of 957 tiles in every level.
  BG1 HOFS/VOFS equal the camera `$B29B/$B29D` modulo the ring, one frame behind; the page comes from the camera,
  the phase from the per-line scroll (MMX's `ExpandStageScroll`).
- BG2 (backdrop): a static 512x256 loop per level (VRAM `$4800`, unchanged across each level's states), so the margins
  sample it with the live per-line scroll and wrap, as the PPU would.
- BG3 is off in levels (TM = `$13`); its margins stay empty.
- Sprites: every piece goes through `$C0A727` (16x16) or `$C0A7AE` (8x8), which drop pieces outside x -15..255
  (-7..255) before writing OAM at `$0300`. The recorder repeats their arithmetic without the test: offset at `[$99]`,
  object screen position `$B4/$B6`, flips `$AF/$B2`, tile `$ACFD/$ADFD + $93*2`, attributes `$B1`, slot `$A8`, Y
  offset `$8C`. The NMI uploads OAM at `$C0A033` only when the game finished the frame (`$0ACF`), so the recorded list
  is latched there and pairs with the OAM it describes. Pass start `$C0AAC0` sets `$A8 = 5`: slots 0-4 are the
  reserved HUD (lives). The object loop at `$C0A58E` draws objects with obj+9 bit 6 with the camera (`$52/$54`) at
  zero: the health and roar boxes. Those are the screen-fixed pieces.

**Hooks, all read-only.** Lines: `tlk_game.c` passes an observer to the beam driver's existing
`snes_beam_frame_driver_draw_ppu_frame_observed`; it sees each visible line just after the PPU drew it (registers
from the raster journal, live CGRAM/OAM/VRAM). VRAM is copied only when `vramWriteCount` moves. Pieces: compiled code
calls `aot_pace_point(cpu, ..., pc24)` after every instruction with a constant PC, so a three-PC test in
`runner/aot_pace.h` folds away at every other call site; the interpreter side is the upstream
`interp_bridge_add_pre_opcode_hook`. A first attempt hooked `cpu_trace_func_entry` with a runtime hunk; TLK builds
with `SNESRECOMP_TRACE=0`, where that call is an empty inline, so it never fired (link error, hunk removed).

**Compositor.** Mode 1 decode, priorities, windows, colour math, brightness and the 32-sprite / 34-sliver limits
follow `ppu.c` (`PpuDrawWholeLine`, `ppu_evaluateSprites`). The centre 256 columns use VRAM and OAM like the PPU, so
they must equal the PPU's picture: `tlk_headless --wide W` counts differing pixels as `native_mismatch`. Margins: BG1
from the map, BG2/OBJ as above, HUD pieces moved out by the margin (a screen-fixed object entirely on one side of the
centre goes to that edge; one across the middle stays). Not a level (title, menus, Stampede, bonus stages: the map
does not match the tilemap) or a line in another mode: the native picture, centred, black sides.

**The ostrich ride.** First full run at 21:9: 569 frames with centre differences, first f3871. Simba and the camera
jump back 432 pixels there (`$B21B` `$997` -> `$7E7` at f3840); afterwards the map behind the camera no longer shows
what is on screen (121 of 957 visible tiles differ at f3870, the screen shows sky where the map has a giraffe). Two
general rules came out of it: the centre always comes from VRAM, and a side's margin uses the map only if map and
VRAM agree at that edge on every line of the frame, else the PPU's own ring. Stage detection needs a majority of
sampled tiles to agree, not 90%, so the ride does not flicker between wide and 4:3 (49 frames sampled f3500-f4700,
none narrow). Left after the warp: the ring margin can show older scenery (a strip of water at f3871's bottom left).

**Settings.** Graphics > Widescreen (off, 16:9, 21:9, fit window) and Widescreen HUD (at the edges, in place);
`widescreen = off|16:9|21:9|adaptive`, `widescreen_hud = edges|in place`. Off by default. Fixed ratios size the window
to that shape; fit window follows the window. `Present` draws the wide picture into the GL or SDL texture; the toast,
practice display and in-picture menu are laid over its centre (pixels where `s_screen` differs from `s_pixels`, as
for HD Mode 7). Integer scaling keeps whole multiples of the 224 lines. Widescreen wins over HD Mode 7 (this game has
no Mode 7 line): the first build let HD Mode 7 win, and the owner, whose ini has `hd_mode7 = 4`, saw only 4:3. State loads
drop the recorded pieces. Plain-SDL `--dump-window` now reads the rendered window back instead of saving the 256x224
buffer, so that path can be checked too.

Cost: 6987M 6,000 frames headless 6.4 s off, 14.7 s at 342 wide (~1.4 ms per frame for capture + compose, this
machine).

Not done: widening the game's own activation window. Enemies appear where the game creates them, which at 21:9 can be
inside the picture (MMX widens its spawn scans; that changes the game and would break every gate, so it would need to
be a separate, opt-in mode). Not checked: whether some objects skip drawing entirely when far off screen (they would be missing from the margins).

Evidence (2026-10-19, worktree `widescreen`):

```
compare_modes 600 3000 / -t 6987M 1200 6000        MATCH, WRAM same (57ccb8d5, c897f389, dd861e16, 946033bd)
check_all_tapes -s                                71 MATCH, 0 MISMATCH
check_idle_lle / check_native_decomp 3000         IDLE MATCH / IDENTICAL
save f3000 / load in new process / f6000           946033bd10683657
ram_sync_compare 6987M / 6781M / 4735M            36,904 / 37,093 / 38,870
check_allbugs                                     ALLBUGS PASS
check_exit_mx                                     282 ok, 11 UNPROVEN, 0 WRONG
pytest tools/tests                                23 passed, 4 skipped
tlk_sdl --selftest-remap (SDL dummy)              PASS (0 failed)
6987M 48,000 f: off / --wide 342 HUD / --wide 448  same frame and WRAM hashes (fce5e678 / c4278b1b)
--lle 3000 f, with and without --wide 342          same hashes; margin_sprite_pixels 221,178 = compiled run
native_mismatch, whole runs                       0: 6987M (448), 6781M, 4735M (342); allbugsF ran with HUD edges
Xvfb, OpenGL 16:9 and opengl = 0 21:9, --dump-window   wide picture, HUD at the edges, looked at
Xvfb + XTest: Esc, Graphics, Widescreen 16:9 -> 21:9    applied live, window resized, "Saved", ini widescreen = 21:9
contact sheet allbugsF every 1500 f, 16:9 HUD edges     looked at: levels wide, Stampede/bonus 4:3
```

Not run: the MinGW/Windows build, macOS, the owner's desktop, the release packages. One 21:9 SDL screenshot after
changing the setting in the menu lacked the roar box; headless 21:9 runs always showed it. Not reproduced.

### WIDE, part 2: enemies out of view (opt-in, changes the game)

The owner tried it and called it glitchy: enemies popping at the old screen edges ("can we preload enemies double the
distance so they appear off camera?"). Two game rules cause it, both found from the object allocator `$C0AAFD`
(called by the spawn helper `$C0AC33` from `$C0CC22`):

- **Spawn window**, `FN_C0B8EA` (run by the camera object): tile columns from 12 (96 px) left of the camera to 42
  (336 px, 80 past the native right edge) right of it, 16-pixel aligned, clamped to the map (`$D0`). Despawn bounds
  `$B1FF..$B205` derive from the same columns.
- **Draw test**, `FN_C0A5BA`: an object is drawn only if its box overlaps camera..camera+256. Off-screen objects
  submit no piece at all, so the recorder never sees them: an enemy alive in a margin is invisible, and vanishes at
  x = 0 / appears at x = 256 inside a wide picture. This is the larger effect.

Graphics > Widescreen enemies (as the game / out of view; `widescreen_enemies`, `--wide-spawn`) widens both by the
picture's margin: `$C0B915` (a CLC after `$46/$14` are stored, before use) moves the columns out by the margin
rounded to 16 px; `$C0A5C2` raises the right limit in `$14`; `$C0A5CF` (an LDY between `LDA $9C` and the ADC of the
width, so A is the same before and after it) adds the margin to the box's left edge. The compiled hook runs after the
instruction and the interpreter's before it; at these three points that is the same state, so compiled and `--lle`
agree with it on. Every piece of a newly drawn off-screen object still fails the game's OAM test, so OAM is unchanged;
the drawing work (tile uploads, CPU time) and the enemies' earlier wake-up are not. Off is the default.

`TLK_WIDE_OBJLOG=<path>` logs each drawn object's box per frame; `scripts/wide_popin.py LOG EXTRA` counts objects
whose first or last box is inside the view without touching its outer edge.

Evidence (2026-10-19):

```
first count (6987M 48,000 f, 16:9, before the fix)   28 appear / 73 vanish in the margins alone
visible pops, 9 run-right level tapes x 5,000 f      16:9: 170 -> 59;  21:9: 329 -> 91 (+300 in l09, see below)
  remaining: Simba blinking after hits (l09, he is hit more in the changed run), objects at the bottom edge with
  ~3 px showing (vertical culling, unchanged)
option on, compiled vs --lle, l01/l07 3000 f          same WRAM (52016cf4 / 26a232b9)
tlk_sdl 21:9 (522 wide) vs headless --wide 522        same WRAM cf211af0 (option on)
option off: compare_modes, check_all_tapes -s         MATCH (57ccb8d5, c897f389, dd861e16, 946033bd), 71 MATCH
contact sheets l01/l02 21:9 option on                 looked at: monkeys, rhino, giraffes, hyenas in the margins, no garbage
6987M with the option on                              desyncs at frame ~980 (RNG follows spawns): expected
```

Not checked: object-slot exhaustion or lag in crowded late levels with the option on at the widest views; a whole
playthrough with it on.

### WIDE, part 3: level ends, the ostrich ride, a flying HUD bug

The owner played the tapes in widescreen and reported three things: at the ends of a level (Pride Lands left and
right, Can't Wait to Be King left) the margin showed the level mirrored instead of stopping where the camera stops;
obstacles in "the sequences with obstacles" popped in; and the HUD glitched at the edges.

**Level ends.** The margins past a level's end came from `MapTile`, which reflects the map there. The game clamps its
camera x at `$C0B273..$C0B284` between `$A6EC` (min) and `$D8` (max); on every stage frame of the All Bugs run both
are fixed per level, 0 and the map width - 256 (logged per frame, levels 0, 1, 2, 4, 13, 15). `ViewColumn` now places
the view the same way: its left edge is the camera minus the margin, kept inside `[$A6EC, $D8 + 256]`, so at an end
the native picture moves off centre (towards the end) and the margin on the other side doubles. A level narrower
than the view (level 10, 256 wide) is centred, and columns outside the camera's range are black. The HUD pass places
health, roar and lives by the view, not by the camera: at the edges, or where a centred picture has them ("in
place"). Overlays in tlk_sdl stay over the frame's centre. With the option on, the spawn window and draw test take
each side's own margin from the live camera (`LiveMargins`), so the doubled margin also gets its enemies.

**Ostrich ride.** Its obstacles do not come from the spawn window: while `$0A56` is set, `$C0B5FD` compares the
distance ridden `$0A40` (the camera's x travel, added at `$C0B36C`) with the next entry of a table at `[$BA]` and,
once reached, `$C0BB80` (only called from there, 4 sites) creates them at x + `$140` of the object at `$AA`. Even with
"out of view" they appeared 270-290 px into the view. The obstacles stand still in the world (-4 px per frame on
screen, the camera's speed; the only movers after f9355 are not from the script), so with the option on the script
runs ahead by the right margin (rounded to 16, the script's step) and places each obstacle that much further right:
`$C0B618` (LDA `$0A56`) adds it to `$0A40`, `$C0B69F` (PLA, the common exit) takes it off, `$C0BB98` (LDY after STA
`$88`) adds it to the new x. None of the three instructions touches what its hook changes, so compiled and `--lle`
agree.

**HUD.** A screen-fixed object went to an edge only when all of it was on one side of the middle, so one crossing
the screen jumped by the margin twice: the bug that flies to the health box after Simba eats it (All Bugs f1781 and
f1786, the only two side changes in 20,000 frames). `HudSide` now moves an object by its centre: all the way within
64 px of an edge (health, roar, lives), not at all within 16 of the middle (titles), in proportion between.

Evidence (2026-10-20, worktree `widescreen`):

```
All Bugs, 21:9 HUD in place, 9000 f              native_mismatch=0, edge_frames=841 (not checked against the PPU)
6987M / 6781M / 4735M 48,000 f, 342 and 522 wide native_mismatch=0 on all six; edge_frames 3178-7212 per run
view off centre (All Bugs, TLK_WIDE_OBJLOG margins)  level 1 start f943-983, its right end f2214-2530 and
                                                 f3701-4107 (native picture fully right from f3872), level 2 start
                                                 f4897-4938; pictures f980, f1000, f3880-3960 looked at: no mirrored margin
tlk_sdl under Xvfb, 21:9, All Bugs f1000         window 1568x672, level start stops at the left edge, HUD at the edges
ride from a f8000 state, 1500 f, 21:9            obstacles appear: as the game 25 at the native edge;
                                                 out of view before: 24 inside the right margin; now 25 at the outer edge
  same run, f8100-8362 (before its first extra lag frame)  every object box in the native 256 identical to as-the-game
  both end the ride with health 6, lives 8; compiled vs --lle (option on, f8700): same frame c15e3fcc, WRAM d8219e1d
flying bug f1774-1790, edges                     moves 4-19 px per frame towards the health box, no 133 px jump
option off: compare_modes MATCH (57ccb8d5, c897f389, dd861e16, 946033bd), 71/71 tapes, idle, native decomp,
  save/load 946033bd, TAS sync 36904 / 37093 / 38870, ALLBUGS PASS, exit_mx 0 WRONG, remap, pytest 23 passed
```

Not checked: "in place" HUD frames at a level's end against anything but the eye; obstacles in the stampede-like
script path (`$DF = $041D`, `$C0BBBC`, a fixed spawn point, left alone); MinGW build.

### WIDE, part 4: Bug Hunt and closing scenes stay wide, the sunset's sky

After v0.3.0 two glitches were known: the Bug Hunts switched between wide and 4:3, and in the sunset level (level 4)
the sky stopped at the native edges with black above the rocks in both margins.

**Switching.** A frame is drawn wide when the level map in RAM (`$7F0000`, read with the record of level `$7FFF9E`)
agrees with the native BG1 tilemap. That map can stop describing the screen while the level is still shown: a Bug Hunt
changes the level number 14 frames before its fade (l11_random f1796-1809, then another level's record reads the
bonus map wrongly) and zeroes the camera's right limit `$D8` 2 frames before it (All Bugs f13007); the
closing scenes of levels 4 and 6 overwrite the map (camera still, agreement 56/56 -> 12/56 at All Bugs f26028; level 6
keeps 34/56 and the live map's overwritten cells showed as a strip of odd tiles past the right edge). Now a copy of
the last map that agreed everywhere is kept with its level record and camera limits, and when the live map does not
agree fully the one that agrees more is used. The copy only bridges an unbroken run of level frames: after a fade to
black it matched the title screen's mostly blank tilemap (attract f4252-5000, drawn off centre before the rule). On
a Bug Hunt's last frame the game turns the screen off below line 30, so the reference line is the first one shown
and, inside a run, 8 cells that all agree will do.

**Sky.** The sunset's sky is BG2, not scrolled (`hofs` 0 on the top lines) in a 64-tile tilemap (`$2109` = `$49`)
whose other half the game leaves empty but for a few stray tiles (4 of 72 margin samples at l04_random f1705). The
margins read that half. A layer that fills at least 90% of the native line and at most 10% of the margins on a line
now shows the native columns mirrored there (`Reflect`, as BG1 past a level's ends). Over All Bugs it fires on level
4, its closing scene, the Scar scene `$0F` (whose left margin had a black block before) and Bug Toss (one screen
wide, margins outside the camera's range, so not drawn).

Evidence (2026-10-20, worktree `widescreen`):

```
l11_random 16:9 f1790-1811                       wide through f1810 (before: 4:3 from f1796, 14 frames)
All Bugs closing scenes, 16:9                    level 4 f26028-26245 and level 6 f42240-42662 wide to the fade
                                                 (before: 4:3 from f26029 / f42242); level 6 right margin jungle, no strip
kept-copy runs over 71 play tapes + All Bugs     24 runs of 8+ frames: level ends, Bug Hunt ends, attract demo end;
                                                 end frames rendered: a level picture or black, no title
l04_random f1705, 16:9 and 21:9                  sky in both margins (before: black above the rocks)
6987M / 6781M / 4735M 48,000 f, 342 and 522 wide native_mismatch=0 on all six
tlk_sdl under Xvfb, 16:9 and 21:9                sunset f1705, Bug Hunt f1827/f1830, level 4 and 6 closing scenes: wide
option off: compare_modes MATCH (57ccb8d5, c897f389, dd861e16, 946033bd), 71/71 tapes, idle, native decomp,
  save/load 946033bd, TAS sync 36904 / 37093 / 38870, ALLBUGS PASS, exit_mx 0 WRONG, remap, pytest 23 passed
```

Not fixed: in level 9 the game moves the camera limits mid-level and the view jumps with them (All Bugs f57144 0 ->
71, f58932 142 -> 71); it was so before part 4. Not checked: MinGW build.

## Task GLIBC: Linux release built on Ubuntu 22.04

The AppImage catalog (appimage.github.io) rejected the v0.3.1 AppImage: on its test system, the oldest Ubuntu LTS in
standard support (22.04, glibc 2.35), `tlk_sdl` would not start: `version 'GLIBC_2.38' not found`. teresa (Arch,
glibc 2.44) links C23 `__isoc23_strtol`/`sscanf`, `strlcpy` (2.38) and `sqrtf`/`atan2f`/`acosf` (2.43). It also
flagged the name: an AppImage should not say "linux".

- `scripts/linux_container.sh` runs a command in an Ubuntu 22.04 image (`packaging/linux/Containerfile`, podman or
  docker, repo mounted at its own path, own ccache dir). CMake comes from PyPI (3.31.6): 22.04's 3.22 can't run the
  SDL3 fetch (`DOWNLOAD_EXTRACT_TIMESTAMP`). The image's -dev packages compile in the same SDL3 backends as an Arch
  build (diff of `SDL_build_config.h`: only libdecor 0.1 / xkbcommon 1.4 headers, and GLES1 added).
- `scripts/release.sh` builds Linux in `build-linux/` through it, packs the tarball and AppImage there, and stops if
  `tlk_sdl` needs a glibc newer than 2.35 or libstdc++ newer than GLIBCXX 3.4.30.
- `TLKRecomp-linux-x86_64.AppImage` is now `TLKRecomp-x86_64.AppImage`. The tarball keeps its name.
- `tlk_sdl` no longer quits when SDL's audio fails to start (no sound library: `SDL_Init` failed on `libjack.so.0` in
  a bare 22.04): it says so and runs silent, as it already did when the device would not open.

`tlk_sdl` also needs the system's `libGL.so.1` (recomp-ui's `launcher_gl.c` calls GL directly). Every desktop has it;
a bare container does not.

Evidence (2026-10-30, teresa):

```
build-linux/tlk_sdl, tlk_headless        newest GLIBC_2.34, GLIBCXX_3.4.29 (was GLIBC_2.43)
scripts/release.sh (8 min, warm ccache)  linux 600/3000 MATCH 57ccb8d5 c897f389, linux-6987M 6000 MATCH a07184e1,
                                         windows same, AppImage and exe f600 57ccb8d5
ubuntu:22.04 container, AppImage         --frames 600 f600 57ccb8d5; with libgl1 + Xvfb and no ROM: window
                                         "The Lion King" with the Choose ROM dialog, still up after 10 s
tlk_sdl audio on / SDL_AUDIO_DRIVER=nonexistent   both run, same frame at 60; remap self-test PASS
```

Not run: the full gate list (only release.sh's subset and the remap self-test; the change outside packaging is the
audio start in `sdl_main.c`), a real window on a desktop, and release.sh under the Gitea runner (podman as the runner's
user was only run from a shell).

## Task XDO: ROM popup invisible to the AppImage catalog test

The catalog test (appimage.github.io PR 10863) failed v0.3.2 with "Could not find a single window on screen" and
quoted the "No ROM given" text, which read as if `tlk_sdl` showed nothing. It did show the popup. A private copy of the
catalog's `test.yml` with a dump at the moment it gives up (xwininfo, xprop, gdb) showed the box mapped and viewable,
the root screenshot showing "Choose ROM..." / "Quit", and `tlk_sdl` idle in `X11Toolkit_DoWindowEventLoop` waiting for
a click (frames symbolized against `build-linux/tlk_sdl`, same build ID as the release). The test finds windows with
`xdotool search --onlyvisible --name .`, and xdotool 3.20160805 (Ubuntu 22.04) does not match a WM_NAME of type
UTF8_STRING. SDL's X11 message box calls `setlocale(LC_ALL, "")` while it is open, so under the runner's
`LANG=C.UTF-8` (or any UTF-8 desktop) its title gets that type. The task GLIBC check ("window The Lion King with the
Choose ROM dialog") passed only because that container had no LANG.

- `PickRom` shows the box through `ShowMessageBoxC`: on Linux/BSD it sets `LC_ALL=C` for the call and restores it, so
  the title is a plain STRING. The text is ASCII. SDL restores the process locale when the box closes. Windows and
  macOS call `SDL_ShowMessageBox` as before. When zenity is installed SDL uses it first (and always on Wayland); it
  starts from SDL's own copy of the environment (`SDL_GetEnvironment()`), so it is unchanged.
- Side effect: with host fonts and a UTF-8 locale the old box drew a blank band between its two lines (font set);
  in the C locale it does not.

Evidence (2026-11-02, teresa; build-linux from the 22.04 container):

```
catalog test.yml on a private copy, v0.3.2   window 0x600003 "The Lion King" IsViewable, WM_NAME(UTF-8);
                                             xdotool --name . finds nothing; bt: main > SDL_ShowMessageBox >
                                             X11_ShowMessageBox > X11Toolkit_DoWindowEventLoop > XPeekEvent
ubuntu:22.04, Xvfb, LANG=C.UTF-8, xdotool    v0.3.2 tlk_sdl: WM_NAME(UTF-8)  NOT FOUND
                                             this change:    WM_NAME(STRING) FOUND
xdotool with LANG=C.UTF-8 / unset / LC_ALL=C all NOT FOUND on the UTF-8 title (its own locale does not matter)
tlk_sdl --frames 600 (dummy video)           f600 57ccb8d5b24918fd (release.sh's package check)
--selftest-remap                             PASS (0 failed)
no ROM, --frames 10                          exit 2, no popup (scripted runs still fail without asking)
```

Not run: the hash gates (`sdl_main.c` is not in `tlk_headless`), the Windows and macOS builds, a real desktop, and
the catalog test on a new release.

## Task ROMWIN: the ROM prompt in the game window

Task XDO's title fix (v0.3.3) got the catalog test past its first window check, then a private rerun of the catalog's
`test.yml` on v0.3.3 failed at the next one: `worker.sh` counts windows with `xwininfo -tree -root | grep '": ("'`,
which needs a WM_CLASS, and SDL's X11 message box sets none (xprop: `WM_CLASS: not found`; SDL has no setting for it).

- With no ROM found, `tlk_sdl` now opens the game window first and asks there (`RomPrompt`, drawn by
  `DrawRomPrompt` into the 256x224 picture through `Present()` on either presenter): the paw icon's sunset, sun
  and paw (same shapes as `tools/make_icon.py`), "THE LION KING" in gold, and a brown panel with the stream
  layout's gold double frame holding the message, a "Choose ROM..." bar and "Enter or click to choose, or drop it
  on this window. Esc quits." in the overlay's 8x8 font. Original art only: there is no ROM yet, and the stream
  layout's backdrop, logo and sprites are game frames that never ship.
  Enter, a click or the pad's bottom/Start button opens the system file dialog (parented to the window); a dropped
  file works too. A wrong file, or no file dialog at all (a sandbox: no portal, no zenity), shows a note in place
  of the first two hints and keeps waiting. The window has WM_CLASS `tlk_sdl` and a STRING title (nothing here
  calls `setlocale`).
- SDL's message box and XDO's `LC_ALL=C` wrapper are gone. Scripted runs (--frames, tapes, self-test) still fail
  with exit 2 and no prompt; a ROM given or found never shows it.
- The catalog's screenshot is now this prompt instead of a 348x95 gray box. Its "mostly dark" warning (dark
  >= 60% and mean < 35% on the shot) was measured with the test's own formula: a first, taller and darker panel
  gave dark 60% / mean 33% once letterboxed as the test frames it; the panel now ends after the hints and the sky
  stops before its dusk purple: 57% / 37%.

Evidence (2026-11-02, teresa; build-linux from the 22.04 container):

```
catalog test.yml, private copy, this build's AppImage   window check FOUND; NUMBER_OF_WINDOWS: 1; window
  (worker.sh swapped in the candidate file)              resized 768x672 -> 768x570; "SUCCESS :-)"; screenshot
                                                         "15% one color, 18 words of text"; run green
                                                         (warnings only: no appdata, no update information)
catalog test.yml on v0.3.3                               NUMBER_OF_WINDOWS: 0, "Could not find a single window"
ubuntu:22.04, LANG=C.UTF-8, Xvfb                         xdotool FOUND; 1 window with a class
  Enter, no portal/zenity                                note "No file dialog here: drop the ROM file on this
                                                         window"; still running
  zenity: /etc/hostname, then the ROM                    wrong-file note; then the game starts (Westwood
                                                         logo), "rom saved to tlk_sdl.ini", rom = <path>
host Xvfb, OpenGL presenter and opengl = 0               same prompt picture on both
tlk_sdl --frames 600 (dummy video)                       f600 57ccb8d5b24918fd
--selftest-remap                                         PASS (0 failed)
no ROM, --frames 10                                      exit 2, no prompt
```

Not run: drag and drop (xdotool cannot send XDND), a real desktop (Wayland, portal dialog), the Windows and macOS
builds, and the hash gates (`sdl_main.c` is not in `tlk_headless`).
