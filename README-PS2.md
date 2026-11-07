# NJEMU on maxsdk (PS2)

NJEMU emulates CPS1, CPS2, Neo Geo MVS and Neo Geo CD. Built for the PS2 with `libs/maxsdk` (clang,
R5900) by `Makefile`, one ELF a system. `src/` is upstream; `src/maxsdk/` is the platform backend in
place of `src/ps2/`, with no gsKit, ps2sdk or audsrv client of its own.

```
make submodules                          third_party/miniz, third_party/libmad
make                                     all four, in build/apps/njemu/<system>/
make TARGET=MVS                          one of CPS1, CPS2, MVS, NCDZ
make TARGET=MVS MX_MEMORY=phys           all in core, with upstream's streaming cache
```

`<system>/data/` has what the emulator reads at run time, made by upstream's host tool: put it next to
the ELF, with `rominfo.<system>` from `resources/` and the game data:

```
<game dir>/mvs.elf, lang/, font/, game_metadata.mvs, dip_metadata.mvs, rominfo.mvs
<game dir>/roms/neogeo.zip, <game>.zip
<game dir>/processed/<game>_cache/      romcnv's output, for sets with encrypted graphics
```

## Memory

MVS and CPS2 games can be larger than the PS2's 32 MiB, so those two ELFs are linked into virtual
memory (`MX_MEMORY=vm`, code in core) without upstream's cache: a game is resident in VM. What is
pinned or core, and why, is at the top of `src/maxsdk/maxsdk_video.c`.

The processed sprites and samples (`processed/<game>_cache/crom` and `vrom`) are mapped from their
files: read when touched, dropped when evicted, no swap traffic and no loading time. The rest of a
game goes through the swap file. A set without processed data has its sprites and samples in the swap
file too, up to 80 MiB more: mind the size of the shared `swap.bin`.

## Encrypted sets

The emulator does not decrypt C-ROMs. Convert such a set (Metal Slug 3, KOF99 and later) on a PC:

```
cmake -S romcnv -B <dir> -DTARGET=MVS && cmake --build <dir>
cd <a directory with game_metadata.mvs, rominfo.mvs, lang/, roms/>
<dir>/romcnv_mvs roms/mslug3.zip         writes processed/mslug3_cache/
```

## Running without a player

`make FRAMES=n PLAY='{60,PLATFORM_PAD_B1},' SHOTS='300,' TESTDIR=/njemu` builds a test ELF
(`src/maxsdk/maxsdk_test.c`): scripted pad input through the real menus, a picture of the screen at the
given flips, the test runners' end marker at flip n. Flips are counted in vsync waits too. On a first
run the BIOS select menu needs a press as well.

## Controls

As upstream: Circle confirms, Cross cancels. Select + Start + L1 + R1 ends the program.

## Not done

- No multitap, and no analog-edge sampling thread: pads are read when sampled.
- The menu language is English at start; the console's setting is not read.
- `src/ps2/`'s HDD and exFAT launch code has no counterpart: mxlink owns storage.
