# CD-i / CD-RTOS notes for static recompilation

These are facts worked out while porting *Link: The Faces of Evil*. The Green
Book (CD-i Full Functional Specification, May 1994) describes the APIs but
mostly omits numeric codes. The codes below were inferred from the game's
library wrappers and the parameter lists in the spec. An entry marked *(seen)*
was confirmed by actual use in that game.

## Disc and boot

* Sectors are raw 2352 bytes, Mode 2: sync (12), header (4), subheader
  (file, channel, submode, coding, ×2), then data. Form 1 carries 2048 bytes
  and Form 2 carries 2324 (audio uses 2304).
* The disc label is at LSN 16 (`"CD-I "` at data offset 1). The application
  identifier at label offset 574 names the boot module, for example `cdi_link`.
* Real-time files (`.rtr`, `.rtf`) have file number ≥ 1. Their directory size
  counts only that file's sectors, and padding sectors use file number 0.
  Empty sectors (submode with no V/A/D bit) can still carry EOR, trigger or EOF
  and **must be processed** for play-control purposes.

## OS-9 process start (what the runtime emulates)

* The kernel copies idata into static storage and applies irefs. The first
  table is code-relative (+module base) and the second data-relative (+data
  base).
* On entry, **a6 = static storage + 0x8000**, a5 = a7 = parameter area,
  a3 = module, d5 = parameter size and d6 = total memory.
* MPW-built programs (Link): idata begins with a jump table header
  `[size, a5 offset]` followed by `dc.w seg; jmp abs.l` stubs copied to
  `a5+0x20`. `jsr N(a5)` jumps to a stub in RAM, which costs one extra
  instruction. Count it, or cycle-exact comparisons with the interpreter
  drift. Function names are present as MacsBug symbols after each function.
* Microware C programs (the Philips "bumper"): a6-relative data and calls of
  the form `movea.l #off,aN / jsr d8(pc,aN.l)`.

## Kernel behaviour the games rely on

* **Signals** are delivered asynchronously. Games busy-wait on flags that
  their intercept handlers set, so translated code must yield at backward
  branches.
* **F$Sleep and F$Wait clear the signal mask.** The standard pattern is
  "mask, start an operation, sleep".
* While an intercept routine runs, signals are masked. F$RTE restores the
  context and unmasks.
* F$Alarm `A$Cycle` (d1=2) with time bit 31 set means the time is in 1/256 s.
  Link's 16 Hz game tick is `0x80000010`.
* F$Fork + F$Wait: a forked child (for example the bumper) runs to completion
  while the parent waits. Asynchronous signal sources must target the process
  that requested them.
* Common system call numbers: F$Link 00, F$Load 01, F$UnLink 02, F$Fork 03,
  F$Wait 04, F$Exit 06, F$Send 08, F$Icpt 09, F$Sleep 0A, F$ID 0C, F$Time 15,
  F$UnLoad 1D, F$RTE 1E, F$TLink 21, F$SetSys 27, F$SRqMem 28, F$SRtMem 29,
  F$Alarm 56, F$SigMask 57, F$SRqCMem 5C, I$Create 83, I$Open 84, I$Delete 87,
  I$Seek 88, I$Read 89, I$Write 8A, I$ReadLn 8B, I$WritLn 8C, I$GetStt 8D,
  I$SetStt 8E, I$Close 8F.
* Error codes: E$BMode CB, E$MemFul CF, E$UnkSvc D0, E$EOF D3, E$PNNF D8,
  E$MNF DD, E$DevBsy FA.

## Devices and the CSD

* Games read `/nvr/csd`. It holds text Device Status Descriptors of the form
  `type:name:params` ending in CR. Types:
  * 1 is the CD control unit (`/cd`);
  * 2 is the audio processor (`/ap`);
  * 3 is video (`/vid`, with `LI="525"` or `"625"`);
  * 4 is NVRAM (`/nvr`, `SZ#8`);
  * 5 is the pointer (`/ptr`, `CL="d"` for a manoeuvring device).
* GetStat 0x43 on `/nvr` returns free NVRAM bytes in d2 *(seen)*.

## CDFM (`/cd` files)

| SetStat | Code | Notes |
|---------|------|-------|
| SS_Play | 0x2D | a0 = PCB *(seen)* |
| SS_Abort | 0x31 | *(seen)* |
| SS_Seek | 0x30 | |
| SS_Enable / SS_Disable / SS_Cont / SS_Stop / SS_Pause | 0x39 / 0x38 / 0x37 / 0x36 / 0x33 | probable numbering |

* **PCB:** Stat.w, Sig.w, Rec.l, Chan.l, AChan.w, then CIL pointers for video,
  audio and data.
  * PCB_Sig is sent on EOR, trigger and finish.
  * Rec is decremented on each EOR, and the play ends when it reaches 0.
* **PCL:** Ctrl.b, res, SMode.b, Type.b, Sig.w, Nxt.l, Buf.l, BufSz.l (in
  sectors), Err.l, res.w, Cnt.l.
  * When the buffer fills, Ctrl bit 0 is set, the CIL entry advances to Nxt,
    and PCL_Sig is sent.
  * A full PCL that comes round again is an overrun. That ends the play with
    an error.
* Plays run at 75 sectors per second (1×). Audio sectors whose channel is in
  AChan go to the audio processor. Otherwise they go to the audio CIL.

## Audio (`/ap`)

* SS_SM = 0x3B. These are SetStat unless marked GetStat:
  * Creat 0 (coding, groups) returns the id and a0 = data;
  * Out 1 (id, a0 = asynchronous status block);
  * Off 2;
  * Close 3;
  * Info 0 is GetStat and returns a0 = descriptor.
* **SM_Out on a busy processor queues the new soundmap** to start when the
  current sector finishes. Games double-buffer music this way and pace
  themselves on the completion signal (ASY_Sig), so completions must be
  sample-exact.
* SS_SC = 0x3D, SC_Atten d3: bytes 2 and 0 are the direct paths (L→L, R→R),
  bytes 3 and 1 the cross paths. 0xFF is off.
* ADPCM coding byte:
  * bits 0-1 select stereo (01 = stereo);
  * bits 2-3 select the rate (00 = 37.8 kHz, 01 = 18.9 kHz);
  * bits 4-5 select the sample size (00 = 4-bit, 01 = 8-bit).

  A sound group is 128 bytes: 16 parameter bytes (copies at 4..11) followed by
  112 data bytes. The filter coefficients are the XA ones.

## Video (`/vid`)

* SS_DC = 0x56 (d2 = function):

  | Code | Function | Code | Function |
  |------|----------|------|----------|
  | 0 | CrFCT | 0xA | WrLI |
  | 1 | RdFCT | 0xB | DlLCT |
  | 2 | WrFCT | 0xC | FLnk |
  | 3 | RdFI | 0xD | LLnk |
  | 4 | WrFI | 0xE | Exec |
  | 5 | DlFCT | 0x11 | SSig |
  | 6 | CrLCT | 0x12 | Relea |
  | 7 | RdLCT | 0x1B | PRdLCT |
  | 8 | WrLCT | 0x1C | PWrLCT |
  | 9 | RdLI | | |

  Seen: 0, 2, 4, 6, 7, 0xA, 0xC, 0xD, 0xE, 0x11 and 0x1C. 0xF and 0x13 are
  probably Intl and SetCmp; they're ignored.
* LCT line numbers are in UCM coordinates: normal-resolution tables hold half
  the lines. "P" variants use physical lines.
* SS_GC = 0x52: Pos 0, Show 1, Hide 2, Ptn 3, Col 4, Org 5, Blnk 6.
  Coordinates are high resolution (768×480).
* SS_PT = 0x59: Coord 0 (GetStat) returns d0 = buttons (bit 15 = active) and
  d1 = H:V. SSig 1, Relea 2, **Pos 3, Org 4** (note this order).
* Video interrupts come only from DCP instruction `$60`. DC_SSig counts them.
* DCP opcodes:

  | Opcode | Meaning |
  |--------|---------|
  | 10 | NOP |
  | 20 | link (driver-internal) |
  | 40 | display line start |
  | 60 | interrupt |
  | 78 | display parameters (RMS bits 0-1, 10 = run-length) |
  | 80-BF | CLUT entry within the current bank |
  | C0 | coding: CM0 bits 0-3, CM1 bits 8-11, NM bit 19, CS bit 22 |
  | C1 | transparency: TA bits 0-3, TB bits 8-11, bit 23 = no mix |
  | C2 | plane order |
  | C3 | bank |
  | C4 / C6 | transparent colour |
  | C7 / C9 | mask colour |
  | CA / CB | DYUV start values |
  | D0-D7 | mattes |
  | D8 | backdrop |
  | D9 / DA | mosaic |
  | DB / DC | ICF (0-63) |

* RL7: a byte with bit 7 set starts a run of colour b&0x7F whose length is the
  next byte (0 = to the end of the line). Otherwise the byte is a single pixel.
* DYUV deltas: 0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255.
* Colours are in studio range (black 16, white 235).
* Link draws sprites with **compiled sprite routines loaded from disc into
  RAM**. They run in the interpreter.

## Timing model used by the runtime

* Emulated time counts executed 68k instructions (`CPU_IPS` = 4M per second).
* Video fields (60 Hz), CD sectors (75 Hz), alarms and soundmap completions are
  scheduled on that clock. Pacing to the wall clock happens once per field.
* Translated code yields only at backward branches (`BUDGET`), and the
  interpreter yields by the same rule. That keeps `--interp` runs comparable
  frame for frame.

## Learned from Voyeur

### Disc

* Every file on the disc, programs included, is in file number 1, so the
  directory's file number doesn't mark real-time files. `cdi_extract.py` now
  skips `.rtf`/`.rtr` files and anything over 4 MB instead.
* `<module>.stb` is a Microware linker symbol module: after the module name
  come the entry table offset.l and count.l, then 10-byte entries (value.l,
  type.b, pad.b, name offset.l). Code symbols are module offsets. Data
  symbols are a6-relative (`0xFFFF8004` = -0x7FFC(a6)).

### Microware C code patterns

* `jsr d(a6)` calls through `_jmptbl`: `jmp abs.l` stubs in static storage,
  initialised from idata, each operand with a code-relative iref. A few are
  `jmp 0` placeholders that the program patches at run time. The generated
  code checks the stub's operand before using the static target.
* Switch bounds: `moveq #N,dR / cmp.l dR,dX / bhi default`.
* Hand-written assembly dispatches through tables of long offsets relative
  to the table: `and.w #mask,dX / lea T(pc),aN / move.l 0(aN,dX.w),dY /
  jsr 0(aN,dY.l)`.
* `CopyMem` and friends `jsr` into the middle of unrolled `move` runs.

### Kernel

* F$GPrDsc ($18): the C library's `clock()` reads P$UTicks/P$STicks at
  descriptor offsets $2B4/$2B8.
* GetStat SS_FD ($0F) on an NVRAM file returns the RBF file descriptor.
  Voyeur shows the modification date (FD_DAT, offset 3: Y-1900 M D H M).

### CDFM play semantics Voyeur depends on

* Its BOLT resource reader runs a ring of 1-sector PCLs over a `.rtb` file
  (Form 2 "video" sectors on channel 16). It expects the play to **finish**
  once no selected channel has a PCL left, and then restarts from the next
  unfilled buffer.
* To stop a play it sets PCB_Rec = 0 and PCB_Chan = $1F, removes its PCL from
  the CIL, and waits for the finished bit. Clearing Rec must end the play
  even though no further sector matches the channel mask.
* A new SS_Play (e.g. on `RTVA.rtf`) can replace a BOLT play that is still
  running. The replaced PCB has to be marked finished.
* PCL overruns on the BOLT ring are normal while the game is busy. It
  recovers by restarting the read.

### Video memory and the viewfinder FMVs

* F$SRqCMem with size -1 ("largest block") must report roughly one 512 KB
  video bank. Voyeur sizes its two BOLT pools from it (colours $80 and $81,
  up to $76000 each, minus a $2000 reserve). With 256 KB pools, the
  viewfinder clips and zoomed pictures (type $21 members of about 92 KB)
  fail to allocate. `doRoom` then retries forever behind a black viewfinder,
  and the intro's viewfinder clip is skipped silently.
* SS_DC function $10 is **DC_NOP** (lct, line, col, lines, cols; UCM lines;
  no buffer): it fills an LCT region with NOPs. The camera view pans with a
  240-line table of per-line VSR loads into a 1024-byte-wide panorama.
  Before showing a zoomed 384x240 DYUV picture, the game wipes that table
  with DC_NOP. Ignoring the call leaves the stale VSR loads, and the DYUV
  plane shows noise.
* UCM numbering around it: Exec $E, Intl $F, NOP $10, SSig $11, Relea $12.
* Viewfinder FMVs are RLF (RL7) streams: each frame lands in a PCL whose
  BufSz the game updates per frame, and the RLF signal handler points a line
  with DC_WrLI (`$40` VSR load) at the new buffer.

### Audio

* Voyeur's library numbers the SS_SM SetStat functions Creat 0, Out 1,
  Off 2, **Cncl 3** (a0 = asy block), **Close 4**. The runtime follows this
  numbering. The Link port treated 3 as Close; recheck this for older titles.

### Input

* The game reads the pointer with PT_Coord, takes the delta from (384, 240)
  and moves the pointer back there with PT_Pos. Its own pointer moves in
  normal-resolution x and line y, at half the device delta.

### FMV upscaling (renderer)

`compose_line` sets the frame buffer's alpha byte for pixels that come from
a photographic plane: DYUV, or run-length coding other than RL3. The
Direct3D shader reads those pixels on the 384-pixel grid, using the even
column of each doubled pair. It resamples them with a Lanczos-2 kernel,
stretched along edges found by a structure tensor over the 4x4 window and
clamped to the nearest 2x2 texels to stop ringing. A bilinear blend of the
mask joins video and graphics. In Voyeur the room is DYUV on plane A. The
actors and the viewfinder marks share RL7 on plane B, so the marks are
upscaled too. They are thin and high-contrast and come out clean.

### Voyeur's video file (RTVA.rtf) and the HD edition

RTVA.rtf holds 72 clips (73,778 frames). Video is on channel 0 and ADPCM
audio on channel 1. Each clip has three parts:

* **Header data sectors** (submode 08/09):
  * the first word is the number of header sectors;
  * +0x24 is the frame count and +0x26 the frame rate (12 or 15 fps);
  * +0x6A holds 128 RGB colours (studio range; entry 0 is 00 FF 00, the
    transparent colour key);
  * +0x36A holds one byte per frame: the number of video sectors in that
    frame.
* **A DYUV still** (coding 05): the clip's background, 384x240. The game
  starts every line at Y=U=V=0x80, not at Y=16.
* **RL7 frames** (coding 0x90): 240 run-length lines each, starting on a
  sector boundary, padded with zeros.

The game shows 238 or 239 of the 240 lines, often several lines lower than
in the clip. Matching therefore works per line:
* `video.c` hashes the coded bytes of each displayed RL7/DYUV line
  (`g_vfi`);
* the pack's index holds up to 12 line hashes per picture, chosen from
  lines unique on the disc;
* a vote gives the picture and the vertical offset, and every line is then
  checked against that picture's full list of line hashes.

Run-ahead needs the whole machine state to be snapshot-able at the frame
boundary:
* Machine globals live in the `.emud`/`.emub` sections (`emu_state.h`).
* Machine heap allocations come from a write-watched arena (`emu_alloc`),
  and RAM is write-watched too, so a snapshot copies only the pages that
  changed.
* NVRAM files are opened per operation, with the file position kept in
  machine state; writes are skipped while running ahead.
* `video_field` no longer presents. It sets `g_frame_ready`, and the CPU
  loop calls `frame_boundary()` once translated code has unwound to the
  dispatcher.
* F$Sleep is restartable: it returns to its own trap instruction when a
  field completes. `gen.py` makes those traps re-entry points.
