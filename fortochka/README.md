# fortochka

Track 2 runtime: Win32 games in a browser tab, no source required.
Plan, architecture, and phase status live in [`../FORTOCHKA.md`](../FORTOCHKA.md).

Everything in this directory is MIT and written from scratch. Wine, WineD3D,
and Boxedwine are read-only oracles — never copy code from them.

## Layout

- `corpus/` — conformance corpus: tiny 32-bit PEs we compile ourselves, one
  seam each. Every binary is a CI test forever. Built with MinGW-w64
  (`i686-w64-mingw32-gcc`), no CRT, minimal import tables.
- `tools/` — recon and debugging tools (`pescope.py`: import-table dump +
  DRM heuristics, no third-party deps).

## Building the corpus

```sh
# macOS: brew install mingw-w64   Linux: apt install gcc-mingw-w64-i686
cd corpus && make
```
