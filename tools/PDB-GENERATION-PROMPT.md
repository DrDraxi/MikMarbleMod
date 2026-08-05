# Prompt for the AI assistant on the build machine

Copy everything below the line into Claude Code (or any coding assistant) on the
machine where you'll build Unreal Engine. It is self-contained — it does not
assume any knowledge of the other conversation.

Bring `get-ue-pdb.ps1` (next to this file) along with it.

---

## Task: produce a UE 5.8.1 Shipping PDB

### Background — why this is needed

I mod a game called **Marbles on Stream** using **UE4SS**, a C++ modding
framework for Unreal Engine games. The game just updated to **Unreal Engine
5.8** (`++UE5+Release-5.8-CL-55116800`) and UE4SS now refuses to load with:

```
Fatal Error: Engine version is not supported.
```

UE4SS does not hard-code engine offsets. It ships generated tables of vtable
indices and member-variable offsets per engine version, produced by its own tool
**UVTD (Unreal VTable Dumper)**, which reads those layouts out of a **PDB
debug-symbol file** from a real build of that engine version.

UE4SS ships tables for UE 4.10 → 5.7 only. There is no 5.8 support anywhere
upstream, and no open PR or issue for it — already checked. So the tables must be
generated, and UVTD needs a UE 5.8 PDB as input.

**The only thing needed from this machine is that PDB file.** Everything after
that (running UVTD, patching UE4SS, rebuilding) happens on the modding machine.

### Prerequisites

1. **Visual Studio 2022** with:
   - "Desktop development with C++"
   - "Game development with C++"

   These give the **MSVC v143** toolchain Unreal requires. Note that Unreal does
   **not** support the **v145** toolset from Visual Studio 2026, and there is a
   known UE 5.8 + VS2026 compile failure. If VS 2026 is the only IDE present,
   add the individual component *"MSVC v143 - VS 2022 C++ x64/x86 build tools
   (Latest)"* plus a Windows 10/11 SDK — the script checks for this and refuses
   to build without it.

2. **GitHub access to `EpicGames/UnrealEngine`** (a private repo; requires being
   in the Epic Games GitHub organisation). Git must be authenticated:
   ```powershell
   gh auth login
   gh auth setup-git
   ```

3. **~150 GB free disk** and a few hours. No Epic Games Launcher install is
   needed — UE 5.8 is source-only, and UVTD prefers source-build PDBs anyway.

### What to run

```powershell
.\get-ue-pdb.ps1
```

Put the source tree on a roomier drive if needed:

```powershell
.\get-ue-pdb.ps1 -SourceDir D:\UnrealEngine-5.8 -OutDir D:\ue-pdb-out
```

If the compile fails partway, resume without redoing the clone/download:

```powershell
.\get-ue-pdb.ps1 -SkipClone -SkipSetup
```

### What the script does

1. **Preflight** — checks git, free disk, and that an **MSVC v143** toolset
   exists (aborting otherwise). Pins UBT to v143 *only* if a VS2026 v145 toolset
   is also installed; on a v143-only machine it leaves the global UBT config
   alone.
2. **Shallow-clones** `EpicGames/UnrealEngine` at tag **`5.8.1-release`**.
3. **Verifies the revision** — reads `Engine/Build/Build.version` and confirms
   `CompatibleChangelist == 55116800`, matching the shipped game exactly. It
   **aborts on mismatch** rather than silently producing offsets for the wrong
   engine revision. (`5.8.0-release` is *not* a match — it records changelist 0.)
4. **`Setup.bat`** — downloads ~20–25 GB of binary dependencies. May launch
   prerequisite installers that ask for elevation.
5. **Builds `UnrealGame Win64 Shipping`** — the engine's own built-in game
   target. In Shipping this is a **monolithic** binary statically linking the
   whole engine, so its PDB carries the full engine type information. No project
   needs to be created.
6. **Copies out `5_08.pdb`** (the filename UVTD expects).
7. **Validates** the PDB actually contains `FUObjectArray`, `FUObjectItem`,
   `UObjectBase`, `UStruct`, `UClass`, `FProperty`, `FField`.

Timing: clone 5–20 min, dependencies 20–60 min, compile 1–3 h.

### Two things that must not be "optimised"

- **Shipping configuration is mandatory.** Unreal wraps class members in
  `#if !UE_BUILD_SHIPPING`, so layouts genuinely differ between configs. The game
  is a Shipping build. A Development PDB would produce offsets that are subtly
  wrong — UE4SS would load and then *silently misread* game data, which is far
  worse than an obvious crash.
- **The tag must be `5.8.1-release`.** It is the revision whose changelist
  matches the game binary.

The script never cooks or packages — only the PDB matters, and skipping that
saves a lot of time.

### Success looks like

```
[+] Changelist matches the shipped game build
[+] All required engine types present
[+] DONE in NNN min
  PDB:  ...\ue-pdb-out\5_08.pdb
  Size: ~1000-3000 MB
```

Then compress it (7-Zip; PDBs compress 3–5×) and transfer it back. **`5_08.pdb`
is the only artifact needed** — the whole source tree can be deleted afterwards.

### If it fails

- **"Repository not found"** on clone — the account isn't in the Epic Games org,
  or git isn't authenticated. Run `gh auth login` then `gh auth setup-git`.
- **"No MSVC v143 toolset found"** — install the component named above.
- **"CompatibleChangelist is X, expected 55116800"** — the wrong tag was cloned.
  Do not override with `-ExpectedChangelist 0` unless you know why.
- **Build fails partway** — re-run with `-SkipClone -SkipSetup`. Out-of-memory
  is common on <32 GB RAM; reduce parallelism via a `BuildConfiguration.xml`
  `<ParallelExecutor><MaxProcessorCount>` setting.
- **Out of disk** — use `-SourceDir` on a larger drive.
- **Types missing in validation** — report the exact output rather than working
  around it; a bad PDB yields silently-wrong offsets, the worst outcome here.

Please report the final script output, plus the path and size of the PDB.
