# Changelog

All notable changes to MikMarbleMod are documented here.

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
