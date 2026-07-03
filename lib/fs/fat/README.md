# FAT filesystem support

Read-only FAT12/16/32 support for LK's `lib/fs`, registered as `"fat"`.

`ff.c`, `ff.h`, `ffunicode.c` and `diskio.h` are vendored unmodified from
FatFs R0.16 by ChaN (http://elm-chan.org/fsw/ff/, FatFs license — BSD-style).
`ffconf.h` is the FatFs configuration (read-only, LFN enabled, CP437,
4 volumes). `fat.c` is the LK glue: it implements the `fs_api` on top of
FatFs and backs the FatFs `diskio` layer with LK bio devices.
