# MikMarbleMod

A [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) C++ mod for **Marbles on Stream**
(Steam AppID `1170970`). It reads the final standings at the end of each race,
royale, or target race (Bullseye) and **POSTs the results as JSON** to an HTTP
endpoint, so an external service can react to **who won and who lost**.

A small Flask **dummy results server** and an **autonomous bot test runner** are
bundled so the whole loop (play a race → mod reads standings → POST → server
prints them) can be exercised without writing any backend.

---

## How it works

The mod is a single translation unit, `src/MikMarbleMod/dllmain.cpp` (~700 lines).
It is **event-driven** — it hooks the game's own gameplay UFunctions rather than
scraping the result UI. At a high level:

1. **Load config** (`config.txt` next to the DLL) — endpoint `host` / `port` /
   `path`. If the file is missing it is auto-created with defaults (`path=/mod`).
2. Once Unreal is initialized it fires a one-shot `POST {path}/ready` so the
   server knows the mod is live. The body identifies the build:
   `{"mod": "MikMarbleMod", "version": "3.22"}`. Builds before 3.21 sent `{}`,
   so treat both fields as optional if you need to support older mods.
3. When a match's HUD appears it registers post-hooks (via `UFunction::RegisterPostHook`)
   on three native `AMarbleRaceHUDY2` events (the royale HUD derives from it, so
   one set of hooks covers both modes), resolving them by walking the class's
   function chain:
   - `OnMarbleFinishedRace(Marble)` → records the **finish order** (race mode).
   - `OnMarbleEliminated(Marble, DamageInstigator, DamageEvent)` → records the
     **elimination order** (first out = the loser). The `DamageInstigator` struct
     carries the killer's `Username`/`DisplayName`, so **per-player elimination
     (kill) counts** are also event-derived.
   - `OnMatchEnded()` → the single authoritative "results are final" trigger.

   **Royale resurrections** (a powerup can revive an eliminated marble) are
   handled by hooking `AMarbleRoyaleGameMode::RespawnMarble(PlayerState)` — the
   function that actually spawns the new pawn, verified by forcing respawns in
   live matches (`URoyaleGameRespawnComponent::BP_RespawnPlayer` is also hooked
   as a second net but never fired in testing). On respawn the player's earlier
   elimination is erased, so only their *final* death (or survival) determines
   placement. A hook-free safety net backs this up: at match end, a standings
   leader who is "eliminated" but demonstrably alive was resurrected and is
   un-eliminated before the winner is chosen. So a resurrected marble that goes
   on to win is correctly crowned, and the loser slot isn't polluted by someone
   who died first but came back.

   Each `Marble` exposes its player name (`_Username`) and `PlayerState` by
   reflection, so standings are rebuilt from authoritative, virtualization-proof
   events — no guessed byte offsets, no dependence on the UI having rendered, and
   no "latch on a half-drawn frame" race.
4. On `OnMatchEnded` it builds the standings and **POSTs them on a background
   thread** via WinHTTP to `POST {path}/results`. Ordering is finishers-first then
   DNFs, so the **last entry is the first eliminated (the loser)**:
   - **race** — finishers in finish order, then DNFs (eliminated/timed-out) in
     reverse-elimination order.
   - **royale** — the winner first (the standings leader `GameState.Top10[0]` if a
     marble survived, otherwise the last marble eliminated), then everyone else in
     reverse-elimination order.
   - **target race (Bullseye)** — this mode's per-marble finish/elimination events
     never fire, so standings are **not** event-derived. The mode is detected by the
     presence of `BullseyeGameState` (so race/royale handling is untouched). On
     `OnMatchEnded` every marble is ranked by its **geometric distance to the target
     centre** (`GameState.BullseyeFinish.Bullseye`), reading world positions via the
     engine's `K2_GetActorLocation` / `K2_GetComponentLocation` (called through
     `UObject::ProcessEvent`). Closest first (winner), farthest last (loser); all
     marbles report `finished: true`. Distance is the actual scoring metric —
     `Top10`, `FinishedMarbles` (settle order), and `FindMarbleAtPosition` (finish
     time) were each verified *not* to encode the target placement. Marbles that
     fly off without settling are destroyed by the game (no actor to rank); they
     are recovered from `PlayerArray` (spectators/inactive excluded) and appended
     at the bottom — the tier the game puts them in — though their order within
     that bottom block is approximate. Replay ghost marbles are excluded.

     **Accuracy, measured (200-marble bullseye, UE 5.8):** winner and loser exact;
     the top ~2/3 of the order matches the game's own results table exactly, then
     the tail diverges (138/200 exact, mean rank delta 6.4). This is inherent to
     the mode rather than a bug: the game's table awards points to the **top three
     only** (400/300/200) and leaves *every* other player on **0**, so below rank 3
     there is no score to reproduce and the distance ranking is a reconstruction.
     The off-map marbles the mod appends do land in the right tier — in that run
     the 4 it flagged were exactly the last 4 rows of the game's table. Treat
     bullseye placements below the podium as approximate; winner/loser are solid.

Per-match accumulators reset whenever the `GameState` instance changes (a new
match). Reading is on the game thread; the HTTP POST is detached so a slow server
can't stall the game.

### Results payload

```jsonc
{
  "type": "race",            // "race", "royale", or "bullseye"
  "players": [
    { "name": "Alice", "finished": true,  "eliminations": 2 },
    { "name": "Bob",   "finished": false, "eliminations": 0 }  // last entry = the loser
  ]
}
```

> **Note:** `eliminations` is the number of kills credited to that player, counted
> from the `DamageInstigator` on each `OnMarbleEliminated` event (0 for
> environmental deaths and in bullseye). Kill counts are keyed case-insensitively
> (the instigator carries the Twitch login, the marble its display name). A
> `damage` field existed in the pre-3.17 UI-scraping era but has no event-derived
> source, so it stays dropped. For royales that **time out** with marbles still
> alive, only the winner and the eliminated players are reported (the alive
> middle-rank players have no event-derived ranking).

The mod also draws a small ImGui debug panel inside UE4SS showing the endpoint,
whether the hooks are registered, and the last standings sent.

---

## Engine updates (currently UE 5.8)

**The game is on Unreal Engine 5.8** (`++UE5+Release-5.8-CL-55116800`). Upstream
UE4SS only ships support up to 5.7, so this repo carries its own 5.8 support.
When the game jumps engine version again, expect the same three failure classes —
none of them are bugs in `dllmain.cpp`, and all of them stop the mod from loading
at all:

1. **`FName::FName(wchar_t*)` fails to resolve** → the pattern scan aborts
   (`Scan failed` on repeat, then `Fatal Error: PS scan timed out`). In 5.8 the
   `EFindName` argument is set *inside* the constructor, so upstream's call-site
   patterns (`41 B8 01 00 00 00`) match nothing. Fixed by
   `dist/ue4ss/UE4SS_Signatures/FName_Constructor.lua`, which matches the
   constructor's own prologue. The `0F B7 02` (16-bit load) in that signature is
   load-bearing — it distinguishes the WIDECHAR overload from the byte-indexed
   ANSICHAR one sitting 0xA0 earlier.

2. **`Fatal Error: Engine version is not supported.`** → no versioned container
   for the new version. Fixed properly by generating one (see below). Do **not**
   paper over this with `[EngineVersionOverride]`; forcing 5.7 on a 5.8 binary
   gets past the check and then hangs forever in
   `while (UObjectArray::GetNumElements() < 10000)`, burning a core, because the
   object-array offsets are wrong.

3. **`GUObjectArray` resolves to the wrong address** → same infinite
   `Waiting for object construction...` hang. UE 5.8 moved `ObjObjects` from
   offset `0x10` to `0x0` (and `FUObjectArray` grew `0xC0` → `0xE8`), so
   patternsleuth's stale correction lands on base + 0x68. Fixed by
   `dist/ue4ss/UE4SS_Signatures/GUObjectArray.lua`, which deliberately does *not*
   subtract `0x10` the way the bundled `.example` does.

4. **A same-version patch release breaks more patternsleuth resolvers** → `Scan
   failed` on repeat. The Oct-2026 update (still 5.8, but
   `CL-55116800` → `CL-58210709`) broke three at once:
   - `EngineVersion` → set `MajorVersion = 5` / `MinorVersion = 8` under
     `[EngineVersionOverride]` in `UE4SS-settings.ini`. Stating the binary's *true*
     version is safe; the warning in failure class 2 is about claiming a different one.
   - `GNatives` → `dist/ue4ss/UE4SS_Signatures/GNatives.lua` matches the static
     `GRegisterNative` stub for opcode 0. The table was found in live memory of the
     unmodded game (rename `dwmapi.dll` so UE4SS doesn't load) and cross-checked
     against the VM's inlined `FFrame::Step` dispatch. GNatives must be correct:
     UE4SS's UFunction hook path uses it.
   - `FUObjectHashTables::Get()` → `GUObjectHashTables.lua` is a **placeholder**.
     UE4SS v3.0.1 never reads this result (see the file header), so the override only
     removes it from the scan.

   The shipping build strips log strings, so string-anchored searches don't work.
   Find the data at runtime first, then build an AOB from the code that references it.

### Regenerating the versioned container for a new UE version

UE4SS derives its offsets from engine **PDB debug symbols** via its own bundled
tool, **UVTD** (`RE-UE4SS/UVTD`, CMake target `UnrealVTableDumper`). The version
list lives in `RE-UE4SS/assets/Default_UVTD_Configs/Config/pdbs_to_dump.json`.

1. Produce a PDB with `tools/get-ue-pdb.ps1` (see `tools/PDB-GENERATION-PROMPT.md`).
   It must be a **monolithic Win64 Shipping** build of the **matching engine
   revision** — layouts differ between build configs (`#if !UE_BUILD_SHIPPING`)
   and between patch releases. Match `CompatibleChangelist` in
   `Engine/Build/Build.version` against the game's `CL-…`.
2. Run UVTD from a **short path** (e.g. `C:\uvtd58`) with `Config/` and
   `PDBs/<X_YY>.pdb` beside the exe, and `pdbs_to_dump.json` listing only the new
   PDB. Long paths blow past `MAX_PATH` and it silently emits only a few files.
   Output goes to a staging tree `UVTD_Generated_Output/`, not into the repo.
3. Copy **only** `<X_YY>_*` FunctionBodies plus `UnrealVirtual<XYY>.hpp/.cpp`
   into `RE-UE4SS/deps/first/Unreal/`. Do **not** copy the shared wrappers or
   `MacroSetter.hpp` — regenerated from one PDB they lose every other version's
   members (`MacroSetter.hpp` shrinks ~126 KB → ~102 KB). A correct import shows
   as purely untracked additions, zero modifications.
4. Add the `Version::IsEqual(X, YY)` branch in
   `RE-UE4SS/deps/first/Unreal/src/VersionedContainer/Container.cpp`.
5. Re-run CMake configure (the source globs are evaluated at configure time, so
   a new `.cpp` is otherwise ignored), then `build.bat --deploy`.

> **Gotcha that costs a whole debug cycle:** UE4SS is loaded from
> `…\Win64\ue4ss\UE4SS.dll`, *not* the Win64 root. Deploying it to the root
> leaves the old DLL in place and the game keeps running the stale build while
> your rebuilds appear to do nothing. `build.bat` deploys to the right place now.

### Where the 5.8 support actually lives (and how to get it back)

The 5.8 container lives in a **nested submodule**,
`RE-UE4SS/deps/first/Unreal` (= `Re-UE4SS/UEPseudo`), which is outside this
repo's git. `setup.bat`, `git submodule update`, or any submodule reset will
detach it back to the pinned upstream commit and **the 5.8 files vanish** — after
which UE4SS still builds but aborts at runtime with
`Engine version is not supported.`

The 5.8 work is therefore kept on **our own fork** of that submodule —
`DrDraxi/UEPseudo`, branch **`ue5.8-support`** (commit `852378a`) — which is the
canonical source for it. `setup.bat` restores it automatically after initialising
submodules, and it can be run by hand any time:

```powershell
tools\restore-ue58-support.ps1 -Check   # is 5.8 support present?
tools\restore-ue58-support.ps1          # put it back
```

Then re-run CMake configure (globs are configure-time) and `build.bat --deploy`.

> No upstream PR was opened. `Re-UE4SS/UEPseudo` is a **private** repo and its
> 5.7 support landed as a direct maintainer push rather than a PR
> ([commit `0ecd6de`](https://github.com/Re-UE4SS/UEPseudo/commit/0ecd6de2d807806cc57acc9464c7feafb466119b)),
> so a fork PR isn't the channel maintainers watch. If upstream ever adds 5.8,
> drop the fork and this script.
>
> For reference, our change matches that precedent's shape: additive tables,
> shared wrappers untouched. No `Derived508` is needed — that layer was deleted
> in `efdd6f8` — and every other version arm in the codebase uses `IsAtLeast`,
> which 5.8 satisfies; the only `IsEqual(5, N)` checks live in `Container.cpp`.

**End users are unaffected** — `dist/` ships prebuilt binaries, so the release
bundle already contains a 5.8-capable `ue4ss/UE4SS.dll` plus the two signature
overrides. Only building from source needs any of the above.

---

## Repository layout

```
MikMarbleMod/
├── CLAUDE.md                     # this file
├── README.md                     # short human-facing intro
├── setup.bat                     # init RE-UE4SS submodule + register the mod
├── build.bat                     # build (and optionally --deploy) the DLL
├── .gitmodules                   # RE-UE4SS pinned as a submodule
├── src/MikMarbleMod/
│   ├── dllmain.cpp               # the entire mod
│   └── CMakeLists.txt            # mod build target
├── tools/
│   ├── get-ue-pdb.ps1            # builds a UE Shipping PDB (input for UVTD)
│   ├── PDB-GENERATION-PROMPT.md  # self-contained prompt for the build machine
│   └── restore-ue58-support.ps1  # re-applies UE 5.8 support after a submodule reset
├── dist/                         # PREBUILT, ready-to-run bundle (see "Run without building")
│   ├── dwmapi.dll                # UE4SS proxy DLL
│   └── ue4ss/…                   # UE4SS runtime + Mods/MikMarbleMod/dlls/main.dll
├── server/
│   ├── server.py                 # Flask dummy results server (port 5000)
│   └── requirements.txt
├── tests/
│   ├── bot_test.py               # autonomous race/royale runner
│   ├── requirements.txt
│   └── screenshots/              # reference images the bot clicks on (step1..step9, royale variants)
└── RE-UE4SS/                     # git submodule (UE4SS framework) — populated by setup.bat
```

---

## Prerequisites

To **build** the mod from source:

- **Visual Studio 2022 Community** (with the "Desktop development with C++"
  workload — `build.bat` calls its `vcvars64.bat`).
- **CMake** and **Ninja** on `PATH`.
- **git** (the build pulls UE4SS as a submodule).

To **run the dummy server / bot tests** (Python 3.10+):

- Server: `Flask` — `pip install -r server/requirements.txt`
- Bot runner: `pyautogui pydirectinput opencv-python pillow` —
  `pip install -r tests/requirements.txt` (or run via `uv`, see below).

You do **not** need Visual Studio or the submodule just to *run* the prebuilt
mod from `dist/`.

---

## Run without building (fastest path)

The `dist/` folder is a complete, ready-to-run bundle of the last released build.
Copy its contents into the game's `Win64` folder:

```
<Steam>\steamapps\common\Marbles on Stream\MarblesOnStream\Binaries\Win64\
```

So `dwmapi.dll` lands next to the game exe, and `ue4ss\` merges into the existing
`ue4ss\` folder. Launch the game; UE4SS loads the mod from
`ue4ss\Mods\MikMarbleMod\dlls\main.dll`.

---

## Build & deploy from source

```bat
setup.bat                  :: one time: clone RE-UE4SS submodule + register the mod
build.bat                  :: build the DLL
build.bat --deploy         :: build, then copy UE4SS.dll, dwmapi.dll and the mod
                           ::   straight into the Marbles on Stream install
build.bat clean            :: wipe the CMake build dir first, then rebuild
```

- `setup.bat` runs `git submodule update --init --recursive` (the first run is
  large — UE4SS pulls its own dependencies) and appends an `add_subdirectory(...)`
  line to `RE-UE4SS/cppmods/CMakeLists.txt` pointing at `src/MikMarbleMod`.
  UE4SS configures its own nested submodules with `git@github.com:` SSH URLs, so
  `setup.bat` passes `-c url.https://github.com/.insteadOf=git@github.com:` to make
  the recursive init work over HTTPS even without a GitHub SSH key. (If you cloned
  with `git clone --recurse-submodules` and don't have SSH set up, re-run that
  fetch with the same `-c url...insteadOf` flag, or just run `setup.bat`.)
- Build output lands in `RE-UE4SS/build/Game__Shipping__Win64/bin/`
  (`MikMarbleMod.dll`, `UE4SS.dll`, `dwmapi.dll`).
- `--deploy` copies the built `MikMarbleMod.dll` to
  `…\Win64\ue4ss\Mods\MikMarbleMod\dlls\main.dll` (renamed to `main.dll`).
- If your game is installed somewhere other than the default Steam path, edit
  `MARBLES_DIR` in `build.bat`.

---

## config.txt

The mod looks for `config.txt` next to its DLL
(`ue4ss\Mods\MikMarbleMod\config.txt`):

```ini
host=127.0.0.1
port=5000
path=/mod

# Optional: players to leave out of reported results.
ignore=DrDraxi, SomeModerator
ignore=AnotherName
```

### `ignore` — omitting players from results

Optional and off by default. Comma-separated display names; the key may be
repeated to append, whitespace is trimmed, blank entries and `#` comments are
skipped.

Matching is **exact but case-insensitive** — this matters in practice: the game
reports the local player's name lower-cased (`drdraxi`), so a case-sensitive
match on `DrDraxi` would silently never fire. It is *not* a prefix/substring
match, so `Bob` does not match `Bobby`.

Applied to **race, death race and bullseye**. Deliberately **not** applied to
**royale**, where placement is reconstructed from elimination order — dropping
entries there would misreport who actually went out first and last.

On startup the mod echoes what it loaded, so a typo is obvious immediately:

```
[MikMarble] ignore list: 2 name(s) loaded -> drdraxi, somemoderator
```

and per match it reports how many it removed:

```
[MikMarble] ignore list: omitted 1 player(s) from results
```

If every player is filtered (e.g. racing solo while ignoring yourself), the mod
sends `{"type": "race", "players": []}`; the bundled server accepts it and
reports `winner: null` / `loser: null`.

The mod POSTs to `{path}/ready` and `{path}/results`, so with `path=/mod` it hits
`/mod/ready` and `/mod/results` — which is exactly what the bundled `server.py`
serves. If `config.txt` does not exist it is auto-created with these defaults
(`path=/mod`), so a fresh deploy works against the dummy server out of the box.
Point `path` at your real backend to use it for real.

---

## The dummy results server

```bash
pip install -r server/requirements.txt
python server/server.py            # listens on 0.0.0.0:5000
```

Endpoints:

- `POST /mod/ready`   → `200` handshake. Logs the reporting build, e.g.
  `Ready: MikMarbleMod v3.22`, and echoes the version back in the response.
- `POST /mod/results` → validates the payload and prints the standings, the
  winner, and (for royales) the loser; echoes the parsed result back as JSON.

It listens on all interfaces so a game on another machine/VM can reach it.

---

## Autonomous bot testing

`tests/bot_test.py` drives the game through pyautogui by matching the reference
screenshots in `tests/screenshots/` (Start → mode select → map → bot count →
play → results), then the mod POSTs the standings to the running server. It's how
you exercise the result-reading logic across maps and modes without hand-playing.

On the results table it reads the ground-truth standings via the panel's
**copy-to-clipboard button** (`results_copy.png`, fixed-coord fallback): the game
puts a `Player/Points/Time` TSV on the clipboard in placement order (winner
first, loser last — all rows, even a 100-player royale), which is printed and
saved to `tests/bot_debug/verify_<ts>_results.tsv` for comparison against the
mod's POST. If the clipboard read fails it falls back to the old screenshot
crops (`verify_<ts>_top10/last.png`).

**Two things that will waste your time if you skip them:**

1. **Start the results server first.** If it's down, every POST fails with WinHTTP
   `error 12029` and nothing is recorded.
2. **Always pass `--with pydirectinput`.** Without it the script falls back to
   pyautogui's virtual-key Tab, which UE's raw-input loop ignores — the post-race
   cursor never wakes and the Final Results / Race Menu clicks all miss.

Run (via `uv`, no venv needed):

```bash
uv run --with pyautogui --with pydirectinput --with opencv-python --with pillow \
  python tests/bot_test.py 5 step3_battle_stadium.png:100 --royale
```

Or with a normal install (`pip install -r tests/requirements.txt`):

```bash
python tests/bot_test.py 3
```

CLI: `bot_test.py [N_RACES] [MAP[:BOTS] ...] [--flags]`

- `N_RACES` — number of races to run (default 3).
- `MAP` — `step3.png` (random, default) · `step3_target_race.png` ·
  `step3_death_race.png` · `step3_battle_stadium.png` (royale). Append `:NN` to
  set the bot count, e.g. `step3_battle_stadium.png:100`.
- Bullseye (`step3_target_race.png` = the Buckshot card): since Season 73 Buckshot
  is not among the first rows of the Race list. Click **Map Name** to sort, so it
  lands near the top, then run with `--skip-menu`. Random can also land on unlisted
  maps (it picked Death Alley and Multilane Madness).
- Flags: `--royale` (enter Royale mode) · `--skip-launch` (game already running) ·
  `--skip-menu` (already past the menu) · `--pause-results` (stop on the results
  table for manual inspection).

Paths: reference screenshots resolve to `tests/screenshots/` and debug captures
to `tests/bot_debug/` by default; override with the `MARBLE_SCREENSHOTS` and
`MARBLE_DEBUG_DIR` env vars. The reference screenshots are resolution/scaling
sensitive — if matches start missing, recapture them on your display.

---

## Versioning / packaging

The version is written in **two** places in `dllmain.cpp`. Update both. The log
line, the debug panel and the `/ready` body all derive from them:

| line | what |
|---|---|
| ~85 | `kModVersion = "3.22";`: narrow copy for the JSON handshake and ImGui |
| ~1340 | `ModVersion = STR("3.22");`: the log line users verify |

Also bump the version in `dist/INSTALL.txt`. To cut a release, build, then
refresh `dist/ue4ss/Mods/MikMarbleMod/dlls/main.dll` with the new
`MikMarbleMod.dll`, zip the `dist/` contents as `MikMarbleMod_vX.YY.zip`
(gitignored) and attach it to a GitHub release tagged `vX.YY`. The bundle does **not** ship a
`config.txt` — the mod auto-creates it (with `path=/mod`) next to the DLL on first
launch, so a fresh install works against the dummy server out of the box.
