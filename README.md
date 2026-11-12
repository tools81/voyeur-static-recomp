# Voyeur (CD-i) static recompilation

A native Windows build of the Philips CD-i game *Voyeur* (1993). The game's
68000 program (`cdi_Voyeur`) and the Philips logo program (`cdi_bumper`) are
translated ahead of time into C and linked with a runtime that provides
CD-RTOS, the CD file manager, video and audio. The game data streams from your
own disc image at run time; nothing from the disc is included in the build.

## Installing (players)

Download the latest release from the
[Releases page](https://github.com/tools81/voyeur-static-recomp/releases):

* `Voyeur-<version>-setup.exe`: installer. It asks for the folder that holds
  your `.cue`/`.bin`, and adds Start menu shortcuts (no administrator rights
  needed). For a silent install use `/VERYSILENT /DISC="D:\path\to\rom"`.
* `Voyeur-<version>-portable.zip`: unzip anywhere and run `voyeur.exe`. It
  asks for the disc image the first time.

You need your own disc image of Voyeur: the release contains no game files
and no game code. The release executables are *interpreter builds*: they
read the game's program from your disc and run it in the runtime's 68000
interpreter. That is the same runtime, with identical output (checked
against the recompiled build: same audio, bit for bit). Build from source
(below) for the statically recompiled version.

Everything the program writes stays in its own folder (portable): `saves\`
holds the saves, `settings.ini`/`settings_hd.ini`, `disc_path.txt` (the disc
location) and the log, and `hd_pack\` holds the HD video pack. At start-up
it checks this repository's latest release and offers to install a newer
version (`--no-update-check` turns this off). "Build HD video pack" (Start
menu) makes the AI video pack for `voyeur_hd.exe` from your disc
(`hd_pack_builder.exe`, a frozen `tools/hd_pack.py`). It downloads
Real-ESRGAN on first use.

### Making a release

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

This builds the interpreter-only executables (`-DRECOMP_INTERP_ONLY=ON` in
`build\release`) and `hd_pack_builder.exe` (PyInstaller, in a private venv).
It writes `dist\Voyeur-<version>-portable.zip` and, with Inno Setup 6
(`installer\voyeur.iss`), `dist\Voyeur-<version>-setup.exe`. The version
and the update repository are set in `runtime/version.h`. Attach both files
to a GitHub release tagged `v<version>`: the in-game updater looks for the
`*-portable.zip` asset of the latest release.

`gen/` (the translated game code), `extracted/`, `rom/` and `hd_pack/` are
not in the repository: they are made from the disc. Without `gen/`, CMake
builds the interpreter-only version automatically.

## Building and running

1. Put the disc image in `rom/` (`Voyeur (USA) (Rev 1) (Alt).bin` + `.cue`).
2. Run `build.bat` (needs Python 3, CMake 3.19+ and Visual Studio 2022).
3. Run `voyeur.exe`. It finds the image in `rom/` next to the exe, or pass the
   `.cue`/`.bin` path as an argument.

Saves (the CD-i NVRAM files) and `recomp.log` go to `saves/` next to the exe.

### Controls

| Action | Keyboard | Gamepad |
|--------|----------|---------|
| Move the pointer | Arrow keys (Shift = faster) | Left stick / D-pad |
| Button 1 (select) | Z, Space, A | A |
| Button 2 | X, Ctrl, S | B, Y |
| Both buttons | C, Enter | X, Start |
| Unlimited camera battery on/off | F5 | |
| FMV upscaler on/off | F8 | |
| Aspect / filter / fullscreen | F9 / F10 / F11 (or Alt+Enter) | |
| Fast forward | Tab (hold) | |
| Quit | Esc | |

On a fresh save the code-entry screen already holds a stored code, `3333`:
enter it on the keypad and press **ACCEPT** to start.

### Unlimited camera battery (F5)

F5 (or `--unlimited-battery` on the command line) stops the camcorder
battery from draining; the title bar shows when it is on. In Voyeur the
battery is also the clock of each time period: the events in the building
are scheduled against it, and the period ends when it runs out. So while
the cheat is on, time stands still. The scenes in the windows stay where
they are, which gives you as long as you like to look around and zoom.
Press F5 again to let time (and the story) move on. The cheat is off at
every start. Details are in `runtime/cheats.c`.

### FMV upscaling

The video clips (RL7) and photographic backgrounds (DYUV) are upscaled on
the GPU from their true 384x240 resolution with an edge-directed Lanczos
filter. Contours come out smooth instead of stair-stepped, and grain stays
grain instead of smearing. Interface graphics keep the selected filter, so
text stays crisp. The upscaler is on by default with the *sharp* and
*smooth* filters, toggled with F8, `--upscale 0|1`, or `upscale=` in
`settings.ini`. It needs the Direct3D renderer (not `--gdi`); the CRT and
nearest filters show the original pixels. The game's data files are not
changed: CD-i video formats have a fixed resolution, so the upscaling
happens at display time.

### HD edition (`voyeur_hd.exe`)

`build.bat` also builds `voyeur_hd.exe`. It is the same game (same
recompiled code, same saves folder) with three additions:

* **AI-upscaled video.** It loads a pack of Real-ESRGAN-upscaled pictures
  for every video frame and clip background on the disc (73,778 frames in
  72 clips). Build the pack once:

  1. Download `realesrgan-ncnn-vulkan-*-windows.zip` from
     https://github.com/xinntao/Real-ESRGAN/releases and unzip it to
     `tools\ai\realesrgan\`. It runs on any Vulkan GPU.
  2. Run `pip install numpy pillow opencv-python`, then
     `python tools\hd_pack.py`.

  The pack is written to `hd_pack\` next to the executables. The defaults
  are the photographic `realesrgan-x4plus` model and output at 2x. Useful
  options:

  * `--scale 4` keeps the full 4x output.
  * `--model realesr-animevideov3` is much faster.
  * `--clips 0,60` builds only some clips (`--list` shows them).

  A run can be interrupted and resumed, because finished clips are
  skipped. It takes hours for the whole disc.

  Pictures are matched on screen by hashing their coded lines, so a clip
  is recognised wherever the game shows it, including partly hidden. If
  no pack is present, or for pictures outside it, the shader upscaler is
  used.
* **60 fps video.** The clips run at 12 to 15 fps. Between two frames,
  the upscaled picture is warped toward the next frame using optical
  flow precomputed by `hd_pack.py`. The next frame is known from the clip
  order, so this adds no delay. Toggle with F7.
* **Run-ahead.** After each field the whole machine is snapshotted, one
  more field is emulated with the same input and shown, then the state is
  restored. This hides one field of the game's own input lag. F6 cycles
  0, 1 and 2 fields; `--runahead N` sets it from the command line.
  Snapshots copy only the memory pages that changed, so the cost is small.

The planes are composed on the GPU at window resolution: the video and
backgrounds from the pack, the graphics sharp, and the cursor drawn last.
Its settings are kept in `saves\settings_hd.ini`. F8 switches HD video off
to compare.

### Status

* Boots through the Philips logo, the code-entry screen and the opening
  titles into the game, with video streamed from `RTVA.rtf` and audio.
* Over 4000 fields of scripted play, the recompiled build is bit-identical
  (every frame and the audio) to a pure-interpreter run (`--interp`).
* About 20 minutes of random-input play (`--fuzz`) produced no crashes.

### What was needed beyond the template

* `tools/cdi_extract.py`: Voyeur puts every file, programs included, in file
  number 1, so real-time streams are now recognised by extension and size.
* `tools/analyze.py`:
  * function starts and names from the Microware linker symbol module
    (`cdi_Voyeur.stb`);
  * `jsr d(a6)` calls through the static `_jmptbl` of `jmp abs.l` stubs;
  * `moveq #N,dR / cmp.l dR,dX` switch bounds;
  * hand-written dispatch through tables of table-relative long offsets;
  * every instruction of unrolled copy loops (Duff's device) as an entry.
* `runtime/`:
  * undiscovered addresses inside recompiled code are interpreted until
    they reach known code, and logged (instead of a fatal error);
  * F$GPrDsc, used by the C library's `clock()`;
  * CDFM: a play ends when no selected channel has a buffer left, when the
    program clears PCB_Rec, or when a new play replaces it;
  * SS_FD on NVRAM files (the "last play" date is the file's date);
  * sound map numbering per Voyeur's library: Cancel 3, Close 4;
  * F$SRqCMem "largest block" reports a 512 KB video bank, so the game's
    BOLT pools are big enough for the viewfinder FMVs and zoomed pictures;
  * DC_NOP (display control function $10), which the game uses to reset
    its panning line table before a zoomed picture.

### Debugging aids

* `--fuzz N`, `--fuzz-start F`, `--fuzz-stop F`: random input between two
  fields.
* `--poke "field:addr:word,..."`: write into emulated RAM at a field. For
  example, Voyeur's pointer is the word pair at $14441C (x) and $14441A (y).
* `"trace_functions": [...]` in `game.json`: log entry (with d0/d1 and the
  return address) to the named functions. The names come from the symbol
  table. Rebuild after changing the list.
* Environment variables:
  * `RECOMP_RAMDUMP=file`: dump RAM, registers and a stack backtrace when a
    headless run ends;
  * `RECOMP_VIDLOG=1`: display state every field;
  * `RECOMP_LCTDUMP=field`: dump the line and field control tables at that
    field (add `RECOMP_LCTDUMP_ALL=1` for every line);
  * `RECOMP_HIDEPLANE=1|2`: hide plane A or B;
  * `RECOMP_SHOT=N[,N...]`: save the GPU-rendered window image of those
    frames as `shot_N.bmp` (windowed runs only);
  * `RECOMP_DUMP_FROM=N`: with `--dump`, start dumping at frame N;
  * `RECOMP_HASHLOG=1`, `RECOMP_HDLOG=1`: log picture hashes, and (HD) the
    pack matches and interpolation phase;
  * `RECOMP_CLUTLOG=field`, `RECOMP_DYUVLOG=1`: colour table, DYUV start
    values;
  * `RECOMP_HD_PACK=dir`: use a pack from another folder.
  * `RECOMP_WATCH=addr[,addr...]`: log 16-bit RAM words (hex addresses)
    whenever they change.
* `voyeur_hd --hd-extract DIR [--hd-clip N] [--hd-hashes]`: decode the
  video file's clips to TGA frames plus line hashes (used by `hd_pack.py`).

See `docs/CDI_NOTES.md` for details.

---

# The CD-i recompilation template

This is a starting point for turning a Philips CD-i game into a native Windows
program. The game's 68000 code is translated ahead of time into C. A runtime
provides the parts of the CD-i the code expects: the CD-RTOS (OS-9) kernel, the
CD file manager, the video decoder (with a Direct3D 11 presenter at any
resolution) and the ADPCM audio processor.

It was extracted from the *Link: The Faces of Evil* project. That build is
bit-identical to a pure-interpreter run of the same game, and this template
reproduces it exactly.

## Starting a new game

1. Copy this folder to a new project folder.
2. Put the disc image (`.bin` + `.cue`, raw 2352-byte sectors) in `rom/`.
3. Edit `game.json`: set `title`, `exe_name`, and the modules to recompile (see
   below).
4. Run `build.bat`. It extracts the program modules from the disc, recompiles
   them into `gen/`, builds with MSVC and copies `<exe_name>.exe` to the project
   root.
5. Run the game, and use the debugging workflow below for anything that breaks.

Requirements: Windows 10+, Visual Studio 2022 (C compiler), CMake 3.19+ and
Python 3.8+. No third-party libraries are needed.

### game.json

```json
{
  "title": "My CD-i Game",
  "exe_name": "mygame",
  "modules": [
    {"file": "auto", "base": "0x100000", "extra_entries": [], "a5_table": "auto"},
    {"file": "cdi_bumper", "base": "0xE0000", "a5_table": "auto"}
  ]
}
```

* The **first module** is the program the player boots. `"auto"` reads its name
  from the disc label's application identifier (`extracted/_app.txt`).
* **Other modules** are programs the game loads at run time with F$Load or
  F$Fork, for example the Philips logo "bumper". Each one is recompiled for its
  `base` address, and the runtime places it there. Ranges must not overlap and
  must stay below `0x140000`, where the heap starts. Program modules the game
  loads that are *not* listed still run, slowly, in the built-in interpreter.
* **`extra_entries`** are code addresses the analyzer can't discover, as
  absolute hex values (`base + offset`). The analyzer already finds:
  * call and branch targets;
  * switch tables;
  * MPW A5 jump tables;
  * Microware `move.l #off,dN / jsr (pc,dN.l)` calls;
  * F$Icpt handlers;
  * `lea`/`pea` function pointers that start with `link` or `movem`.
* **`a5_table`**: `"auto"` detects programs built with Apple MPW-style
  toolchains, whose calls go through `jsr N(a5)`.

To see what the analyzer found, run `python tools/analyze.py [module index]`.
It writes a disassembly with recovered function names, from MacsBug symbols if
present, to `build/listing_<module>.asm`.

## Layout

| Path | Contents |
|------|----------|
| `tools/cdi_extract.py` | Green Book filesystem reader. Writes program files and `_app.txt` (`--all` also dumps the real-time streams). |
| `tools/m68k.py` | MC68000 decoder |
| `tools/os9mod.py` | OS-9 module header, idata and irefs |
| `tools/analyze.py` | Code discovery (recursive descent plus the patterns above) |
| `tools/gen.py` | 68000→C generator, flag liveness, dispatch tables, `gen/game_config.h` |
| `tools/config.py` | `game.json` loader |
| `tools/gen_tests.py` | Per-instruction differential tests |
| `tools/bmp2png.py` | Converts frame dumps to PNG |
| `runtime/cpu.c` | Dispatcher and run loop |
| `runtime/interp.c` | 68000 interpreter, for RAM code and the `--interp` reference mode |
| `runtime/alu.c` | Shifts, division and BCD with exact flags |
| `runtime/os9.c` | OS-9 kernel: memory, processes (fork/wait/exit), signals, alarms, paths, NVRAM, CSD |
| `runtime/cdfm.c` | CD file manager: reads, and real-time SS_Play with PCB/PCL/CIL |
| `runtime/video.c` | UCM display control (FCT/LCT), MCD212-style renderer, cursor, pointer |
| `runtime/audio.c` | ADPCM levels A/B/C, sound maps with gapless SM_Out, attenuation |
| `runtime/render_d3d11.c` | GPU presenter: aspect modes and sharp/smooth/CRT filters |
| `runtime/host_win32.c` | Window, input (keyboard and XInput), waveOut, pacing, scripted and fuzz input |
| `test/` | Instruction differential tester and ALU reference vectors |
| `docs/CDI_NOTES.md` | CD-i facts and API codes worked out during the first port |

## Debugging workflow

* **Find what fails.** Unimplemented OS calls and SetStat/GetStat codes are
  logged to `saves/recomp.log`. Fatal errors also show a 68k register dump and a
  stack backtrace with function names.
* **Debug options:**
  * `--trace` / `--trace3` log syscalls with callers, signals and disc activity.
  * `--headless --frames N --dump K` runs fast without a window and writes every
    Kth frame as a BMP.
  * `--script "field:key:dur,..."` scripts input (keys `1 2 L R U D`).
  * `--fuzz SEED` drives the game with random input.
  * `--wav out.wav` records the audio.
* **Check the translation.** Run the same headless session twice, once with
  `--interp`, which executes everything in the interpreter, and compare the
  frame dumps and WAVs. Headless runs are deterministic: emulated time is
  counted in instructions, and the clock uses a fixed epoch. Any difference
  points to a translation bug.
* **Unit-level checks.** `python tools/gen_tests.py` and
  `python test/gen_alu_vectors.py` generate test sources. Then build the
  `insn_test` and `alu_test` targets:

  ```
  cmake --build build\cmake --config Release --target insn_test alu_test
  build\cmake\Release\insn_test.exe rom\<image>.bin 200
  build\cmake\Release\alu_test.exe
  ```

* Code that jumps into RAM is not an error. It runs in the interpreter. Jumps to
  *undiscovered* addresses inside a recompiled module are fatal. Add those
  addresses to `extra_entries`, or teach `analyze.py` the pattern.

## Things a new game may need

* **More CD-RTOS calls.** The runtime implements what the first game used. See
  `docs/CDI_NOTES.md` for what's known and what isn't. The CD-i Full Functional
  Specification (Green Book, May 1994) documents the calls but gives no numeric
  function codes. Infer those from the game's library wrappers: look at the
  `d2` values passed with `os9 I$SetStt`.
* **UCM drawing functions** (DM_/DP_/DR_/RG_ and text). These aren't implemented
  yet because the first game drew everything itself.
* **PAL games.** The CSD in `os9.c` (`write_csd`) advertises a 525-line NTSC
  player and the video runs 240 lines at 60 Hz. A 625-line title needs
  `LI="625"`, 280 lines (`SCREEN_LINES` in `video.c`) and `FIELD_RATE` 50.
* **Full Motion Video** (MPEG cartridge) isn't supported.
