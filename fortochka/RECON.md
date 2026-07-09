# RECON — Phase F0 worksheet (Rome: Total War)

Exit criterion from FORTOCHKA.md: import table ranked, DRM verdict, shader
verdict, call-frequency table. Fill sections in place; this file becomes the
one-page RECON.md when done.

Machine split: runtime dev + corpus build on macOS; everything Wine-related
on the Linux box (32-bit Wine prefixes are painless there).

## 1. DRM verdict — GATE, do first

- [ ] Identify which RTW release you lawfully own (retail CD = SafeDisc = dead
      end; Steam-era "Rome: Total War Collection" / Gold — verify actual DRM
      on the shipped exe, not the store wrapper; PCGamingWiki row first).
- [ ] Copy the install dir to the Linux box and run:

```sh
python3 tools/pescope.py /path/to/RomeTW.exe
```

`drm_findings` must be empty (or Steam-stub only → check whether the exe
launches without the client; if it refuses, it does not onboard — hard rule,
no §1201 gray areas). Record verdict here:

> **Verdict (2026-07-10, RTW Gold via Steam on cachynator):**
>
> *Static:* NO DRM WRAPPER. No `.bind`/SteamStub, no packer sections — plain
> PE32, base 0x400000, unencrypted. Links `steam_api.dll` (18 imports) as an
> ordinary DLL. So this is NOT a §1201 *access-control* case: nothing is
> decrypted to run the code.
>
> *Runtime (the deciding test):* launched under Wine with the Steam client
> shut down, authentic files untouched (relay trace `/tmp/rtw-relay.log`).
> The game's OWN code runs, then:
> ```
> 00dc:Call KERNEL32.OutputDebugStringA(... "Steam must be running to play this game\n")
> 00dc:Call ntdll.NtRaiseException(...)
> 00dc:Call KERNEL32.ExitProcess(00000000)
> ```
> RTW.exe performs a **Steam client-presence check** (via steam_api.dll) and
> **refuses to run without the live client.** This is the plan's
> "demands a live client" case. Per our own hard rule, RTW Gold **does not
> onboard as-is** — we do not emulate/replace steam_api (§1201-gray, banned).
>
> *Nuance for the flagship decision:* the exe is unencrypted and the user owns
> a lawful license; the block is a runtime ownership check, not an encryption
> wall. It runs normally with the real Steam client up (that path is where the
> d3d census must be captured). The blocker is specifically **browser has no
> Steam client** to satisfy the check.
>
> **d3d census + shader/renderer experiments: BLOCKED** until flagship
> resolved — they need the game to actually render, which needs the Steam
> client running (can't be automated here without touching the user's live
> session). Not yet captured.

## 2. Import table — the canonical scope list

```sh
cd /path/to/RTW-install
python3 /path/to/tools/pescope.py --json *.exe *.dll > rtw-imports.json
python3 /path/to/tools/pescope.py RomeTW.exe          # human-readable
```

- [x] Dump committed: `rtw-imports.json`. RomeTW.exe: 295 imports / 14 DLLs.
      Ranked:
      - **trivial**: advapi32 (6 plain RegXxxA → regweb), winmm (5 timer fns),
        gdi32 (`GetDeviceCaps`, one function), dinput8 (`DirectInput8Create`),
        ole32 (6 CoXxx), msvfw32 (3 DrawDib* — intro videos, skippable),
        ddraw (2: CreateEx + EnumerateExA — mode enum/menu blit)
      - **mechanical**: kernel32 (140), user32 (26 — textbook message pump:
        RegisterClassA/CreateWindowExA/PeekMessageA/DispatchMessageA/
        MsgWaitForMultipleObjects, nothing exotic), wsock32 (18 winsock-1.1
        ordinals — multiplayer, stub to failure, single-player unaffected)
      - **hard**: d3d9/d3d8 (one import each — `Direct3DCreate9`/`Create8`;
        the real surface is COM vtables behind them)
- [x] CRT linkage: **static** — no msvcrt.dll import. Smaller surface, as
      hoped. No delay imports at all.
- [x] **RTW ships BOTH renderers**: imports `Direct3DCreate8` AND
      `Direct3DCreate9`. If the D3D8 path is selectable/forcible, Track 1's
      existing d8web covers RTW rendering and d9web demotes to
      quality-of-life. Runtime experiment decides (Wine, force/pick renderer).
- [x] Guest-DLL census: `mss32.dll` (Miles) + `steam_api.dll` run emulated.
      **Miles outputs via waveOut** (`waveOutOpen/Write/PrepareHeader` +
      midiOut/mixer/aux minor) — NOT DirectSound. dsweb shrinks to a
      waveOut→AudioWorklet shim; DirectSound emulation off RTW's critical
      path entirely. No binkw32 anywhere — videos are VfW DrawDib.

## 3. Shader-path experiment — decides d9web critical path

On Linux, 32-bit prefix:

```sh
export WINEPREFIX=~/.wine-rtw WINEARCH=win32
wineboot
wine reg add "HKCU\\Software\\Wine\\Direct3D" /v MaxShaderModelPS /t REG_DWORD /d 0 /f
wine reg add "HKCU\\Software\\Wine\\Direct3D" /v MaxShaderModelVS /t REG_DWORD /d 0 /f
wine RomeTW.exe
```

- [ ] Does menu + campaign + battle still render with PS forced off?
  - Yes → d8web FFP synthesizer covers RTW baseline; bytecode translator is
    an effects milestone, off the critical path.
  - No → SM2 translator moves onto the critical path in F3. Record which
    screens break.

> **Verdict:** _pending_

## 4. Boundary census — call-frequency trace

```sh
WINEDEBUG=+d3d9 wine RomeTW.exe 2> d3d9-menu.log      # sit in menu ~30s, quit
WINEDEBUG=+d3d9 wine RomeTW.exe 2> d3d9-campaign.log  # load campaign, few turns
WINEDEBUG=+d3d9 wine RomeTW.exe 2> d3d9-battle.log    # one small battle

# frequency table per phase:
grep -oE '[a-z0-9_]+::[A-Za-z0-9_]+' d3d9-battle.log | sort | uniq -c | sort -rn | head -40
```

(If the grep pattern misses Wine's current trace format, eyeball the log and
adjust — we want calls/frame for menu vs campaign vs battle.)

- [ ] Paste top-40 tables here; derive draws/frame and Lock/Unlock churn —
      this re-bases the F3 perf budget.

## 5. Corpus toolchain

- [x] `i686-w64-mingw32-gcc` on macOS (brew mingw-w64); `corpus/hello.exe`
      builds: PE32, base 0x400000, exactly 3 kernel32 imports, 6 KB.
      Era-correct enough — minimal import tables matter more than era compilers.
- [x] Wine oracle on macOS: wine-11.0 (`brew install --cask wine-stable`,
      Rosetta 2 + WoW64). `wine corpus/bin/hello.exe` → prints correctly,
      exit 0. Corpus CI can assert `wine rung.exe` == zhelezo output locally.
- [ ] Same toolchain on the Linux box (`apt install gcc-mingw-w64-i686`) —
      second oracle + where RTW graphics recon runs.

## Notes / gaps found during doc review (fold into FORTOCHKA.md)

- **Virtual CD drive:** era exes (even DRM-free) may call
  `GetDriveTypeA`/`GetVolumeInformationA` for disc-presence or CD-audio
  (`mciSendString`). k32web needs virtual drive letters over OPFS; a volume
  label in the cartridge. Only for games with *no* TPM — a real check that
  gates on the disc is DRM and stays untouchable.
- **COOP/COEP:** AudioWorklet ring buffer + SAB requires cross-origin
  isolation headers on the hosting site. Site config, decide early.
- **CSP for tier-1 JIT:** runtime `WebAssembly.Module` codegen needs
  `wasm-unsafe-eval` in the site CSP. Same bucket as above.
- **Memory layout detail for F1:** identity mapping needs the Emscripten
  static data/heap *above* the guest arena — build with `-sGLOBAL_BASE`
  raised (e.g. guest owns [0, 512 MB), host runtime above). Decide before
  writing k32web's VirtualAlloc.
