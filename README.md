# Aheui libretro core

Runs pure-Hangul Aheui programs as libretro content. A PNG stores the Aheui
grid as pixel residues mod 40; `.aheui` and `.txt` are read as a grid directly;
`.b` and `.bf` are compiled to Aheui on load.

**Every file here belongs at the repository root**, beside the `.github`
directory. If they end up in a subfolder, the build fails with
`make: *** No targets specified and no makefile found.`

## Getting a build without installing anything

Push this repo, open the **Actions** tab, wait for the run, then download the
artifact for your platform from the run's summary page:

| artifact | file | goes in |
| --- | --- | --- |
| `aheui_libretro_windows` | `aheui_libretro.dll` | RetroArch cores directory |
| `aheui_libretro_linux` | `aheui_libretro.so` | same |
| `aheui_libretro_macos` | `aheui_libretro.dylib` | same |

`aheui_libretro.info` goes in the core info directory. `install.sh` does both
on Unix by reading the paths out of your `retroarch.cfg`.

## Building locally

    make                  # native
    make platform=win     # MinGW
    make platform=osx     # macOS
    make test             # build the headless frontend and run the suite

## Using it

Load the core first, then the content — RetroArch filters the file browser by
the loaded core's extensions, so a `.png` is not offered until Aheui is loaded.

    retroarch -L path/to/aheui_libretro.so maze.png

Start with `maze.png`: it needs no input. For anything that reads input, turn
on **Game Focus** (Scroll Lock), or RetroArch keeps your keystrokes for its own
hotkeys and the program sits at its prompt looking hung.

## Regenerating the tables

`aheui_tables.h` and `font8x8.h` are generated, and are the core's copy of
tables that also live in the Python compiler. Never edit them by hand:

    make tables
