#!/usr/bin/env python3
"""pescope — PE32 import-table dump + DRM heuristics. No dependencies.

Phase F0 recon tool: run against a game exe and every DLL shipped in its
install directory to produce the canonical HLE scope list.

    python3 pescope.py RomeTW.exe
    python3 pescope.py --json *.exe *.dll   > recon-imports.json

Reports, per file:
  - COFF/optional-header basics (machine, subsystem, image base, linker stamp)
  - section table (a DRM tell on its own)
  - full import table grouped by DLL, plus delay-load imports
  - DRM heuristics: SafeDisc / SecuROM / Steam-stub / generic packer markers
"""

import json
import struct
import sys

MACHINES = {0x014C: "i386", 0x8664: "x86-64", 0x01C4: "armv7", 0xAA64: "arm64"}
SUBSYSTEMS = {2: "gui", 3: "console"}

# Section names and byte signatures that identify era copy protection.
DRM_SECTIONS = {
    "stxt371": "SafeDisc",
    "stxt774": "SafeDisc",
    ".sdata": "SafeDisc",
    ".securom": "SecuROM",
    ".cms_t": "SecuROM (older)",
    ".cms_d": "SecuROM (older)",
    ".bind": "Steam stub (SteamStub/DRMP)",
    "UPX0": "UPX packer",
    "UPX1": "UPX packer",
    ".vmp0": "VMProtect",
    ".vmp1": "VMProtect",
}
DRM_STRINGS = {
    b"BoG_ *90.0&!!  Yy>": "SafeDisc v1/v2 signature",
    b"SafeDisc": "SafeDisc marker string",
    b"AddD\x03\x00\x00\x00": "SecuROM v4 marker",
    b".steamdrmp": "Steam DRM helper",
    b"SteamAPI_Init": "links Steam API (check if optional)",
}
DRM_IMPORT_DLLS = {
    "dplayerx.dll": "SafeDisc driver-side DLL",
    "clcd32.dll": "SafeDisc CD-check DLL",
    "secdrv.sys": "SafeDisc driver",
    "steam_api.dll": "Steam API (may be optional at runtime)",
    "steam_api64.dll": "Steam API (may be optional at runtime)",
}


class Pe:
    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("no MZ header")
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
            raise ValueError("no PE signature")
        coff = e_lfanew + 4
        (self.machine, nsects, self.timestamp, _, _, opt_size, _) = struct.unpack_from(
            "<HHIIIHH", data, coff
        )
        opt = coff + 20
        self.magic = struct.unpack_from("<H", data, opt)[0]
        if self.magic != 0x10B:
            raise ValueError(f"not PE32 (magic {self.magic:#x}) — PE32+ is out of scope")
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.subsystem = struct.unpack_from("<H", data, opt + 68)[0]
        ndirs = struct.unpack_from("<I", data, opt + 92)[0]
        self.dirs = [
            struct.unpack_from("<II", data, opt + 96 + 8 * i)
            for i in range(min(ndirs, 16))
        ]
        self.sections = []
        sect0 = opt + opt_size
        for i in range(nsects):
            off = sect0 + 40 * i
            name = data[off : off + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
            chars = struct.unpack_from("<I", data, off + 36)[0]
            self.sections.append(
                {"name": name, "vaddr": vaddr, "vsize": vsize,
                 "rawptr": rawptr, "rawsize": rawsize, "chars": chars}
            )

    def rva(self, rva: int) -> int:
        """RVA -> file offset."""
        for s in self.sections:
            if s["vaddr"] <= rva < s["vaddr"] + max(s["vsize"], s["rawsize"]):
                return s["rawptr"] + (rva - s["vaddr"])
        raise ValueError(f"RVA {rva:#x} not in any section")

    def cstr(self, rva: int) -> str:
        off = self.rva(rva)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def _walk_thunks(self, thunk_rva: int):
        names = []
        off = self.rva(thunk_rva)
        while True:
            entry = struct.unpack_from("<I", self.data, off)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                names.append(f"ordinal#{entry & 0xFFFF}")
            else:
                names.append(self.cstr(entry + 2))  # skip hint word
            off += 4
        return names

    def imports(self):
        """{dll_name: [function, ...]} from the regular import directory."""
        out = {}
        rva, size = self.dirs[1] if len(self.dirs) > 1 else (0, 0)
        if not rva:
            return out
        off = self.rva(rva)
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", self.data, off)
            if not name_rva:
                break
            dll = self.cstr(name_rva).lower()
            out[dll] = self._walk_thunks(ilt or iat)
            off += 20
        return out

    def delay_imports(self):
        out = {}
        rva, size = self.dirs[13] if len(self.dirs) > 13 else (0, 0)
        if not rva:
            return out
        off = self.rva(rva)
        while True:
            (attrs, name_rva, _, _, _, int_rva, *_rest) = struct.unpack_from(
                "<8I", self.data, off
            )
            if not name_rva:
                break
            # Attr bit 0 set => fields are RVAs (standard); clear => VAs.
            if not (attrs & 1):
                name_rva -= self.image_base
                int_rva -= self.image_base
            dll = self.cstr(name_rva).lower()
            out[dll] = self._walk_thunks(int_rva)
            off += 32
        return out

    def drm_findings(self):
        found = []
        for s in self.sections:
            tag = DRM_SECTIONS.get(s["name"])
            if tag:
                found.append(f"section '{s['name']}' -> {tag}")
        for sig, tag in DRM_STRINGS.items():
            if sig in self.data:
                found.append(f"byte signature -> {tag}")
        for dll in self.imports():
            tag = DRM_IMPORT_DLLS.get(dll)
            if tag:
                found.append(f"imports {dll} -> {tag}")
        return found


def scan(path: str) -> dict:
    with open(path, "rb") as f:
        pe = Pe(f.read())
    imports = pe.imports()
    return {
        "file": path,
        "machine": MACHINES.get(pe.machine, hex(pe.machine)),
        "subsystem": SUBSYSTEMS.get(pe.subsystem, str(pe.subsystem)),
        "image_base": f"{pe.image_base:#x}",
        "link_timestamp": pe.timestamp,
        "sections": [
            f"{s['name']} vaddr={s['vaddr']:#x} vsize={s['vsize']:#x}"
            for s in pe.sections
        ],
        "imports": imports,
        "delay_imports": pe.delay_imports(),
        "import_totals": {dll: len(fn) for dll, fn in imports.items()},
        "total_imported_functions": sum(len(v) for v in imports.values()),
        "drm_findings": pe.drm_findings(),
    }


def main(argv):
    as_json = "--json" in argv
    paths = [a for a in argv if not a.startswith("--")]
    if not paths:
        print(__doc__)
        return 1
    results = []
    for p in paths:
        try:
            results.append(scan(p))
        except (ValueError, OSError) as e:
            results.append({"file": p, "error": str(e)})
    if as_json:
        json.dump(results, sys.stdout, indent=2)
        print()
        return 0
    for r in results:
        print(f"\n=== {r['file']} ===")
        if "error" in r:
            print(f"  ERROR: {r['error']}")
            continue
        print(f"  {r['machine']} {r['subsystem']}, base {r['image_base']}, "
              f"{r['total_imported_functions']} imports from {len(r['imports'])} DLLs")
        print("  sections: " + ", ".join(s.split()[0] for s in r["sections"]))
        if r["drm_findings"]:
            print("  !! DRM/packer findings:")
            for f in r["drm_findings"]:
                print(f"     - {f}")
        else:
            print("  DRM heuristics: clean")
        for dll, fns in sorted(r["imports"].items(), key=lambda kv: -len(kv[1])):
            print(f"  {dll} ({len(fns)}):")
            for fn in sorted(fns):
                print(f"      {fn}")
        for dll, fns in sorted(r["delay_imports"].items()):
            print(f"  [delay] {dll} ({len(fns)}): {', '.join(sorted(fns))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
