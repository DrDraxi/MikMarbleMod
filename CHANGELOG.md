# Changelog

All notable changes to MikMarbleMod are documented here.

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
