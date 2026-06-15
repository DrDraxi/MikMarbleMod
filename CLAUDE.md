# MikMarbleMod

A [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) C++ mod for **Marbles on Stream**
(Steam AppID `1170970`). It reads the final standings at the end of each race or
royale and **POSTs the results as JSON** to an HTTP endpoint, so an external
service can react to who won, who lost, eliminations, and damage.

A small Flask **dummy results server** and an **autonomous bot test runner** are
bundled so the whole loop (play a race → mod reads standings → POST → server
prints them) can be exercised without writing any backend.

---

## How it works

The mod is a single translation unit, `src/MikMarbleMod/dllmain.cpp` (~2k lines).
At a high level:

1. **Load config** (`config.txt` next to the DLL) — endpoint `host` / `port` /
   `path`. If the file is missing it is auto-created with defaults
   (**see the config warning below**).
2. On `start_mod` it registers UE4SS hooks. Once Unreal is initialized it fires a
   one-shot `POST {path}/ready` so the server knows the mod is live.
3. While a match runs it inspects the game's result UI widgets and the
   `GameState` to recover each player's **placement, finish time, DNF/eliminated
   status, elimination order, eliminations and damage**. Much of the file is
   *offset discovery* — it figures out where fields like `FinishTime`,
   `bEliminated`, `Position` and `EliminationOrder` live in the result structs by
   cross-referencing what the visible UI shows (DNF vs a placement number), so it
   keeps working across game updates. A "sticky DNF cache" remembers players who
   scrolled off-screen.
4. When the standings are final it builds a JSON payload and **POSTs it on a
   background thread** via WinHTTP to `POST {path}/results`. Players are ordered
   by placement: finishers first (1st…Nth), then DNFs ordered highest-placed →
   lowest, so for royales the **last entry is the first eliminated (the loser)**.

### Results payload

```jsonc
{
  "type": "race",            // or "royale"
  "players": [
    { "name": "Alice", "finished": true,  "eliminations": 3, "damage": 120 },
    { "name": "Bob",   "finished": false, "eliminations": 0, "damage": 0   }
  ]
}
```

The mod also draws a small ImGui debug panel inside UE4SS for inspecting what it
read.

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
├── dist/                         # PREBUILT, ready-to-run bundle (see "Run without building")
│   ├── dwmapi.dll                # UE4SS proxy DLL
│   └── ue4ss/…                   # UE4SS runtime + Mods/MikMarbleMod/dlls/main.dll + config.txt
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

## config.txt (read this — it bites)

The mod looks for `config.txt` next to its DLL
(`ue4ss\Mods\MikMarbleMod\config.txt`):

```ini
host=127.0.0.1
port=5000
path=/mod
```

The mod POSTs to `{path}/ready` and `{path}/results`, so with `path=/mod` it hits
`/mod/ready` and `/mod/results` — which is exactly what the bundled `server.py`
serves.

⚠️ **If `config.txt` does not exist, the mod auto-creates it with `path=/winners`,
which does NOT match the dummy server** (you'd get 404s). The `dist/` bundle ships
a correct `config.txt` with `path=/mod`. If you build/deploy fresh, make sure the
deployed `config.txt` says `path=/mod` (or point it at your real backend).

---

## The dummy results server

```bash
pip install -r server/requirements.txt
python server/server.py            # listens on 0.0.0.0:5000
```

Endpoints:

- `POST /mod/ready`   → `200` ("mod is ready" handshake)
- `POST /mod/results` → validates the payload and prints the standings, the
  winner, and (for royales) the loser; echoes the parsed result back as JSON.

It listens on all interfaces so a game on another machine/VM can reach it.

---

## Autonomous bot testing

`tests/bot_test.py` drives the game through pyautogui by matching the reference
screenshots in `tests/screenshots/` (Start → mode select → map → bot count →
play → results), then the mod POSTs the standings to the running server. It's how
you exercise the result-reading logic across maps and modes without hand-playing.

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
- Flags: `--royale` (enter Royale mode) · `--skip-launch` (game already running) ·
  `--skip-menu` (already past the menu) · `--pause-results` (stop on the results
  table for manual inspection).

Paths: reference screenshots resolve to `tests/screenshots/` and debug captures
to `tests/bot_debug/` by default; override with the `MARBLE_SCREENSHOTS` and
`MARBLE_DEBUG_DIR` env vars. The reference screenshots are resolution/scaling
sensitive — if matches start missing, recapture them on your display.

---

## Versioning / packaging

`ModVersion` is set in `dllmain.cpp` (currently `3.16`). To cut a release, build,
then refresh `dist/ue4ss/Mods/MikMarbleMod/dlls/main.dll` with the new
`MikMarbleMod.dll` and zip the `dist/` contents. Keep `config.txt` set to `/mod`
in the shipped bundle so it works against the dummy server out of the box.
