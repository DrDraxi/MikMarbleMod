# Changelog

All notable changes to MikMarbleMod are documented here.

## [3.22] - 2026-10-27

The Season 73 game update (Steam build 26247165) is still on Unreal Engine 5.8,
but it is a newer engine build (`CL-55116800` → `CL-58210709`), and every UE4SS
mod stopped loading again. UE4SS's startup scan failed to find three engine
lookups, retried with `Scan failed` until `PS scan timed out`, and never started
any mod. As in 3.21, nothing was wrong in `dllmain.cpp`. The mod code is
unchanged apart from the version number.

### Fixed — Season 73 / CL-58210709 compatibility
- **`EngineVersion` no longer detected** (`Failed to find EngineVersion`): none
  of patternsleuth's patterns match this build. `UE4SS-settings.ini` now sets
  `[EngineVersionOverride]` to 5.8. That is the binary's real version, so it is
  safe; the 3.21 warning is only about claiming a different one.
- **`GNatives` not found.** Both patternsleuth routes, the inlined
  `GRegisterNative` init and the `UObject::SkipFunction` prologue, stopped
  matching. Added `UE4SS_Signatures/GNatives.lua`, which matches the static
  registration stub for opcode 0 (`EX_LocalVariable`). The table was located
  in live memory of the unmodded game and confirmed against the VM's inlined
  `FFrame::Step` dispatch; all 91 registration stubs agree on its address.
  GNatives must be correct: UE4SS's UFunction hook path uses it.
- **`FUObjectHashTables::Get()` not found.** UE4SS v3.0.1 never reads this
  result, so `UE4SS_Signatures/GUObjectHashTables.lua` is a labelled
  placeholder that only takes the resolver out of the scan (see its header).

### Tests
- `tests/screenshots`: recaptured the Race and Royale mode cards (the mode
  select screen was redesigned) and the Buckshot map card. Buckshot is now
  reached by sorting the Race list by **Map Name**.
- `bot_test.py`: the Max Marbles field is located relative to the Bots button
  instead of hardcoded coordinates (the August update moved that row).

### Verified
Against the game's own results export on build 26247165: race 20/20 exact,
death race 20/20 exact, royale 100/100 exact (99 kills credited for 99
eliminations), royale with 3 forced resurrections 50/50 exact (the first
marble out was resurrected and placed 3rd), bullseye 48/50 (winner, podium,
loser and the 3 off-map marbles exact; one swap among 0-point places). Also
verified: `config.txt` auto-create, the ignore list (case-insensitive, not
applied to royale), the WinHTTP error path with the server down, and the
ImGui panel's test buttons.

## [3.21] - 2026-08-05

Marbles on Stream moved to **Unreal Engine 5.8**
(`++UE5+Release-5.8-CL-55116800`) and every UE4SS mod stopped loading. This
release restores compatibility and adds a version handshake plus an optional
ignore list. None of the breakage was in `dllmain.cpp` — it never got to run.

### Fixed — UE 5.8 compatibility
- **`FName::FName(wchar_t*)` no longer resolvable**, which aborted UE4SS's
  pattern scan entirely (83 retries, then `PS scan timed out`). In 5.8 the
  `EFindName` argument is materialised *inside* the constructor rather than at
  the call site, so upstream's `41 B8 01 00 00 00` patterns match nothing —
  5908 of its 5911 direct call sites never touch `r8`. Added
  `UE4SS_Signatures/FName_Constructor.lua`, matching the constructor's own
  prologue; the `0F B7 02` word load is what distinguishes it from the
  byte-indexed ANSICHAR overload sitting 0xA0 earlier.
- **No UE 5.8 versioned container existed** (upstream UE4SS stops at 5.7), so
  it aborted with `Engine version is not supported.` Generated
  `UnrealVirtual508` plus 62 vtable / member-layout tables with UVTD from a
  monolithic Win64 Shipping PDB of engine tag `5.8.1-release` (matching the
  game's changelist 55116800).
- **`GUObjectArray` resolved to `FUObjectArray + 0x68`**, leaving UE4SS
  spinning forever on `Waiting for object construction...` and burning a core.
  5.8 moved `ObjObjects` from `0x10` to `0x0` (and grew `FUObjectArray` from
  `0xC0` to `0xE8`), so patternsleuth's pre-5.8 correction misses. Added
  `UE4SS_Signatures/GUObjectArray.lua`, which deliberately does *not* subtract
  `0x10` the way the bundled example does.

> Forcing `[EngineVersionOverride]` to 5.7 is **not** a workaround: it clears
> the version check and then hangs on the same object-construction loop.

### Added
- **`/ready` now identifies the build.** The handshake body is
  `{"mod": "MikMarbleMod", "version": "3.21"}` so a backend can tell which
  version is reporting. Earlier builds sent `{}` — both fields should be
  treated as optional. `server.py` logs it as `Ready: MikMarbleMod v3.21`.
- **Optional `ignore` config** to leave players out of reported results:
  `ignore=Name1,Name2` (comma-separated, key may be repeated). Matching is
  exact but **case-insensitive** — the game reports the local player's name
  lower-cased, so case-sensitive matching would silently never fire. Applies to
  race, death race and bullseye; **not** to royale, where placement is derived
  from elimination order. Off by default. The loaded list is echoed at startup
  and removals are logged per match. If every player is filtered the payload is
  `{"type": "...", "players": []}`.

### Fixed — tooling / packaging
- `build.bat --deploy` copied `UE4SS.dll` to the Win64 root instead of
  `Win64\ue4ss\`, so the game silently kept loading a stale build while
  rebuilds appeared to do nothing. It also reported success when the copy
  failed because the game was running; it now fails loudly.
- The version string was duplicated in three places with only one bumped, so
  the mod could log a stale version — the log line now derives from
  `ModVersion`.
- `INSTALL.txt` in the bundle still said 3.17 and documented an outdated
  payload (no `eliminations`, no `bullseye`). Rewritten with the full API.
- Removed a stray `bash.exe.stackdump` that had shipped since 3.18.

### Testing
Verified against the game's own results export on UE 5.8 — race 200/200 exact,
royale 100/100 exact including elimination counts, death race 192/200 (every
mismatch a genuine tie: swapped marbles share identical elimination times to
six decimals), bullseye winner/loser exact with mid-field approximate by nature
(the game scores only the top three). Ignore list verified live in both
directions, plus 21 unit tests over parsing, matching and edge cases.

## [3.20] - 2026-07-02

Resurrection handling fixed against live-verified behavior. Forced-respawn
testing in 200-bot royales (a throwaway Lua mod calling the game's own
`RespawnMarble`) showed that resurrections do NOT go through the
`BP_RespawnPlayer` hook that 3.19 relied on — and one test match reproduced the
original bug exactly (a bot died, was resurrected, won, and the payload crowned
the wrong player while listing the real winner mid-table).

### Fixed
- **Hook the function that actually resurrects:**
  `AMarbleRoyaleGameMode::RespawnMarble(PlayerState)` (returns the new pawn).
  `BP_RespawnPlayer` (the wave-respawn wrapper, which never fired in testing)
  stays hooked as a second net; the respawn recorder dedupes.
- **Hook-free safety net:** at match end, if the standings leader (`Top10[0]`)
  is in the eliminated set but their marble is demonstrably still alive, they
  were resurrected by a path no hook saw — they are un-eliminated before the
  winner is chosen. Aliveness at match end is the proof; this catches any
  resurrection mechanism, present or future.
- **Bullseye on large fields (200 marbles):** the roster scan used to stop after
  4 consecutive empty position slots, silently dropping the tail — including the
  true last place. It now scans the full position range and sweeps all live
  marble actors. Marbles that fly off without settling are DESTROYED by the
  game and can't be ranked geometrically; they're recovered from `PlayerArray`
  (spectator/inactive PlayerStates excluded, so a non-racing host is never
  added) and appended at the bottom, where the game itself places them. Their
  order *within* that bottom block is approximate (the game sorts it by an
  inaccessible leave time). Verified twice at 200 marbles: full roster, winner
  and loser both matching the game's own export.
- **Replay ghost marbles (`GhostMarble_BP_C`) are excluded** from all marble
  sweeps — one sat at the target centre and "won" a test bullseye.

### Testing
- All modes verified at 200 players against the game's own results export
  (copy-to-clipboard TSV): 6 royales (3 with forced resurrections via the
  game's `RespawnMarble`), 2 random races, 2 death races, 4 bullseyes.

## [3.19] - 2026-07-01

Royale resurrection support + per-player elimination counts.

### Fixed
- **Resurrected royale winners are now crowned correctly.** A new post-hook on
  `URoyaleGameRespawnComponent::BP_RespawnPlayer` erases a respawned player's
  earlier elimination, so only their *final* death (or survival) determines
  placement. Previously a player who died, respawned, and won stayed in the
  eliminated set — the mod crowned the last-eliminated marble instead, and if
  the resurrected winner had been the first out, they were reported in the
  loser slot. (The hook registers lazily on the first royale, since the respawn
  component only exists in that mode.)
- **The royale loser is the first *final* elimination.** A player who was first
  out but respawned no longer occupies the loser slot.

### Added
- **`eliminations` per player in the payload** — kills credited to each player,
  read from the `FDamageInstigator` (killer `Username`/`DisplayName`) that the
  game passes with every `OnMarbleEliminated` event. Event-derived, unlike the
  pre-3.17 UI-scraped counts. 0 for environmental deaths; keyed
  case-insensitively (instigator carries the Twitch login, marbles the display
  name). `damage` remains dropped — it has no event-derived source.
- Respawns are logged (`[MikMarble] respawn: <name> (elimination erased)`) and
  the ImGui panel shows respawn-hook status and per-player elim counts.
- Hook param offsets are resolved from the UFunction's own param reflection
  (no packing assumptions), and the respawn hook binds by static path
  (`/Script/MarblesOnStream.RoyaleGameRespawnComponent:BP_RespawnPlayer`) so it
  registers at startup without needing the royale-only component instance.
- Kill counts are joined from killer *display* names back to standings
  *usernames* via a per-match name map (the `DamageInstigator` carries display
  names; bot usernames differ by a digit suffix, humans only by case).
- The bot test now reads ground-truth standings via the results panel's
  copy-to-clipboard button (full TSV in placement order, saved to
  `tests/bot_debug/verify_<ts>_results.tsv`) instead of scrolling screenshot
  crops; the first eliminations of each match are logged raw for diagnosis.

### Payload
```jsonc
{
  "type": "royale",
  "players": [
    { "name": "Alice", "finished": true,  "eliminations": 2 },
    { "name": "Bob",   "finished": false, "eliminations": 0 }  // last = the loser
  ]
}
```

## [3.18] - 2026-06-26

Target race (Bullseye) result reading — marbles ranked by geometric distance to
the target centre (`GameState.BullseyeFinish.Bullseye`), closest first. Race and
royale handling untouched.

## [3.17] - 2026-06-18

A ground-up rewrite of how the mod reads results — from scraping the result UI to
listening to the game's own gameplay events.

### Changed
- **Event-driven result reading.** The mod now hooks three native
  `AMarbleRaceHUDY2` UFunctions via `UFunction::RegisterPostHook` (the royale HUD
  derives from it, so one set covers both modes):
  - `OnMarbleFinishedRace(Marble)` → finish order (race)
  - `OnMarbleEliminated(Marble)` → elimination order (first out = the loser)
  - `OnMatchEnded()` → the single authoritative "results are final" trigger

  Player names are read off each Marble by reflection (`_Username`). Standings are
  rebuilt from authoritative, virtualization-proof events — no guessed byte
  offsets, no dependence on the UI having rendered.

### Fixed
- **Winner and loser are now correct and authoritative.** Royale winner = the
  standings leader (`GameState.Top10[0]`) if a marble survived, otherwise the last
  marble eliminated; loser = the first marble eliminated.
- **All racers are captured regardless of player count.** The old ~24-row limit
  was a side effect of reading the virtualized result list; events fire once per
  marble, so 40-player matches report all 40.
- **No more latching on a half-rendered result frame** — the result is built on the
  game's `OnMatchEnded` event, not on whatever the result table showed first.
- Removed the silent alphabetical-order fallback that could mislabel the loser.

### Removed
- Per-player `eliminations` and `damage` fields from the payload. The game only
  exposes those in the opaque result data object / UI, so they could not be made
  authoritative from events. The payload is now `{ "name", "finished" }`.
- ~1,800 lines of UI-scraping, runtime byte-offset discovery, the sticky-DNF
  cache, and the stale-listview rejection heuristics (dllmain.cpp: ~2,560 → ~780
  lines).

### Notes
- Config default corrected to `/mod`. `config.txt` is auto-created next to the DLL
  on first launch and is no longer shipped in the bundle.
- The HTTP `httpBusy` guard is now an atomic compare-exchange; the stale ImGui
  version string is fixed.
- **Known limitation:** a royale that times out with marbles still alive reports
  only the winner + eliminated players (the alive middle-rank marbles have no
  event-derived ranking). Fully-fought royales and all races report everyone.

### Payload
```jsonc
{
  "type": "race",            // or "royale"
  "players": [
    { "name": "Alice", "finished": true  },
    { "name": "Bob",   "finished": false }   // last entry = the loser
  ]
}
```

## [3.16]

Last release of the UI-scraping era. Read the result widgets and recovered
per-player placement / finish time / DNF / eliminations / damage by discovering
byte offsets in the result data object at runtime, cross-referencing the visible
table. The payload included `eliminations` and `damage`. Prone to occasional
wrong winner/loser on large royales (offset-discovery failure, latching on an
animating frame, off-screen rows beyond the ~24 visible).
