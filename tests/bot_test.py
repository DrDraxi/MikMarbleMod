"""
Autonomous Marbles on Stream race runner.
Drives the game via pyautogui using reference screenshots.

Run with:  uv run --with pyautogui --with pydirectinput --with opencv-python --with pillow python bot_test.py [N_RACES] [MAP[:BOTS] ...] [--flags]
  IMPORTANT: --with pydirectinput is REQUIRED. Without it the script falls back to
  pyautogui's virtual-key Tab, which UE's raw-input loop ignores, so the post-race
  cursor never wakes and Final Results / Race Menu clicks all miss.
  N_RACES: number of races to run (default 3)
  MAP: step3.png (random, default) | step3_target_race.png | step3_death_race.png
       | step3_battle_stadium.png (royale)   ;  append :NN to set bot count
  Flags:
    --royale       enter the Royale mode (clicks the Royale button instead of
                   Race; defaults the map to Battle Stadium)
    --skip-launch  game already running
    --skip-menu    already past the main menu / mode select
    --pause-results  stop ON the results table (skip the Race Menu click) so the
                     final standings can be inspected manually
"""
import sys
import time
import subprocess
from pathlib import Path

import pyautogui
try:
    import pydirectinput   # scancode-based input — UE games' raw-input loops accept these
                           # whereas pyautogui's virtual key codes are often ignored
    pydirectinput.PAUSE = 0.05
except ImportError:
    pydirectinput = None

# Reference screenshots live alongside this script (tests/screenshots/); debug
# captures are written to tests/bot_debug/. Override with env vars if needed.
import os
_HERE = Path(__file__).resolve().parent
SCREENSHOTS = Path(os.environ.get("MARBLE_SCREENSHOTS", _HERE / "screenshots"))
DEBUG_DIR = Path(os.environ.get("MARBLE_DEBUG_DIR", _HERE / "bot_debug"))
DEBUG_DIR.mkdir(parents=True, exist_ok=True)
STEAM_APPID = 1170970  # Marbles on Stream
STEAM_URL = f"steam://rungameid/{STEAM_APPID}"

pyautogui.PAUSE = 0.4
pyautogui.FAILSAFE = True

DEFAULT_CONF = 0.92            # tighter to avoid transition false-positives
# Per-image confidence overrides: map cards have rendered 3D backgrounds with
# pink/purple hover borders that move pixel values away from the reference.
PER_IMAGE_CONF = {
    "step3.png": 0.75,
    "step3_death_race.png": 0.7,
    "step3_target_race.png": 0.7,
    "step3_battle_stadium.png": 0.7,   # royale map card (rendered 3D bg)
    "step2_royale.png": 0.7,           # Royale mode button (rendered 3D bg)
}
SETTLE_BEFORE_CLICK = 2.0      # let UI settle before clicking
POST_CLICK_DELAY = 1.5

_step = [0]
def dbg_shot(label):
    _step[0] += 1
    p = DEBUG_DIR / f"{_step[0]:03d}_{label}.png"
    try:
        pyautogui.screenshot(str(p))
    except Exception as e:
        print(f"  screenshot err: {e}", flush=True)

def find(image_name, timeout=120, confidence=DEFAULT_CONF, region=None):
    img = str(SCREENSHOTS / image_name)
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            loc = pyautogui.locateCenterOnScreen(img, confidence=confidence, region=region)
            if loc:
                # require image to be at the same place 2 polls in a row (settled)
                if last and abs(loc.x - last.x) < 5 and abs(loc.y - last.y) < 5:
                    return loc
                last = loc
        except pyautogui.ImageNotFoundException:
            last = None
        except Exception as e:
            print(f"  locate err: {e}", flush=True)
        time.sleep(0.6)
    return None

def click_when(image_name, timeout=120, confidence=None, label=None):
    label = label or image_name
    if confidence is None:
        confidence = PER_IMAGE_CONF.get(image_name, DEFAULT_CONF)
    print(f"[click] waiting for {label} (timeout={timeout}s, conf={confidence})", flush=True)
    loc = find(image_name, timeout=timeout, confidence=confidence)
    if not loc:
        print(f"[click] TIMEOUT {label}", flush=True)
        dbg_shot(f"TIMEOUT_{label.replace(' ','_')}")
        return False
    time.sleep(SETTLE_BEFORE_CLICK)
    dbg_shot(f"before_click_{label.replace(' ','_')}")
    # Move TO the button slowly (gives game's input handler time to register
    # the move), wiggle, then click. Sometimes a direct click is dropped.
    pyautogui.moveTo(loc.x, loc.y, duration=0.15)
    time.sleep(0.1)
    pyautogui.moveTo(loc.x + 5, loc.y, duration=0.05)
    pyautogui.moveTo(loc.x, loc.y, duration=0.05)
    time.sleep(0.1)
    pyautogui.click(loc.x, loc.y)
    print(f"[click] {label} at {loc}", flush=True)
    time.sleep(POST_CLICK_DELAY)
    dbg_shot(f"after_click_{label.replace(' ','_')}")
    return True

def wait_for(image_name, timeout=600, confidence=DEFAULT_CONF, label=None):
    label = label or image_name
    print(f"[wait]  expecting {label} (timeout={timeout}s)", flush=True)
    loc = find(image_name, timeout=timeout, confidence=confidence)
    if loc:
        print(f"[wait]  {label} appeared at {loc}", flush=True)
        return True
    print(f"[wait]  TIMEOUT {label}", flush=True)
    dbg_shot(f"WAIT_TIMEOUT_{label.replace(' ','_')}")
    return False

def read_results_via_clipboard(ts):
    """Read the final standings via the Results panel's copy-to-clipboard button.

    The button (top-left of the Results panel, camera-stack icon) puts a TSV
    table on the clipboard: a "Player\\tPoints\\tTime" header, then one row per
    player in PLACEMENT ORDER — winner first, loser last. That covers all rows
    of a 100-player royale without the scrollbar dance, and the names are exact
    text (no OCR from crops). Returns the ordered player list, or None so the
    caller can fall back to the screenshot method.
    """
    loc = find("results_copy.png", timeout=5, confidence=0.85)
    xy = (loc.x, loc.y) if loc else (326, 151)   # fixed fallback (2560x1440)
    # Pre-clear the clipboard so stale content can't be mistaken for results.
    try:
        subprocess.run(["powershell", "-NoProfile", "-Command", "Set-Clipboard -Value ' '"],
                       check=False, timeout=5)
    except Exception:
        pass
    # Same cursor rules as the other results-screen buttons: big move (wakes a
    # hidden cursor), then a tiny hover jiggle so the button registers, then click.
    pyautogui.moveTo(1280, 700, duration=0.15)
    time.sleep(0.1)
    pyautogui.moveTo(xy[0], xy[1], duration=0.2)
    time.sleep(0.1)
    pyautogui.moveTo(xy[0] + 3, xy[1] + 2)
    time.sleep(0.05)
    pyautogui.moveTo(xy[0], xy[1])
    time.sleep(0.08)
    pyautogui.click()
    time.sleep(0.8)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Clipboard -Raw"],
                             capture_output=True, text=True, errors="replace", timeout=5).stdout or ""
    except Exception as e:
        print(f"[clip] read err: {e}", flush=True)
        return None
    rows = [r.split("\t") for r in out.replace("\r", "").split("\n") if r.strip()]
    if len(rows) < 2 or rows[0][0] != "Player":
        print(f"[clip] unexpected clipboard content ({len(rows)} rows) — falling back", flush=True)
        return None
    players = [r[0] for r in rows[1:]]
    try:
        (DEBUG_DIR / f"verify_{ts}_results.tsv").write_text(out, encoding="utf-8")
    except Exception as e:
        print(f"[clip] tsv save err: {e}", flush=True)
    print(f"[clip] {len(players)} players copied: winner={players[0]} loser={players[-1]}", flush=True)
    return players

def dismiss_copy_dialog():
    """The copy button opens a centered modal ("Data Copied To Clipboard
    Successfully." + Ok) that swallows ALL further clicks — the Race Menu clicks
    land on nothing until it's dismissed. Click its Ok button (screen-centered,
    fixed coords like the rest of the script)."""
    pyautogui.moveTo(1280, 870, duration=0.2)
    time.sleep(0.1)
    pyautogui.moveTo(1283, 872)   # tiny hover jiggle so the button registers
    time.sleep(0.05)
    pyautogui.moveTo(1280, 870)
    time.sleep(0.08)
    pyautogui.click()
    time.sleep(0.5)
    print("[clip] dismissed copy-confirmation dialog", flush=True)

def run_one_race(map_image, bot_count=None, pause_results=False):
    print(f"\n=== Starting race on {map_image}" + (f" with {bot_count} bots" if bot_count else "") + " ===", flush=True)
    if not click_when(map_image, timeout=20, label=f"map ({map_image})"):
        return False
    if not click_when("step4.png", timeout=15, label="Bots"):
        return False
    # Optionally set bot count: double-click the racer-count input field at
    # (2100, 1100), select-all, then type the desired number.
    if bot_count is not None:
        time.sleep(0.5)
        # "Max Racers" input pill — native (2105, 895), same row as the Bots button.
        # (Previously clicked (2100,1100) which is ~205px too low = grass, so the
        #  paste never landed and the race ran with the field's default.)
        pyautogui.doubleClick(2105, 895)
        time.sleep(0.3)
        # Select-all FIRST so the paste REPLACES any existing value (the field may
        # default to 20/50; double-click alone may not select the whole number, so
        # "100" could append → game clamps to its max). Ctrl+A then paste replaces.
        if pydirectinput:
            pydirectinput.keyDown("ctrl"); pydirectinput.press("a"); pydirectinput.keyUp("ctrl")
        else:
            pyautogui.hotkey("ctrl", "a")
        time.sleep(0.2)
        # Layout-independent: copy number to clipboard, then Ctrl+V. This bypasses
        # any keyboard layout issues (Czech QWERTZ remaps the number row, NumLock
        # state may turn numpad digits into arrow keys, etc.).
        try:
            import subprocess as _sp
            _sp.run("clip", input=str(bot_count), text=True, check=True, timeout=2)
        except Exception as e:
            print(f"  clip err: {e}", flush=True)
        time.sleep(0.2)
        if pydirectinput:
            pydirectinput.keyDown("ctrl"); pydirectinput.press("v"); pydirectinput.keyUp("ctrl")
        else:
            pyautogui.hotkey("ctrl", "v")
        # Forcefully release Ctrl — if it stays stuck, the later Tab becomes Ctrl+Tab
        # (cursor never wakes) and clicks become Ctrl+clicks. This was the bug behind
        # "no mouse cursor" on the results screen.
        for _ in range(2):
            try:
                if pydirectinput: pydirectinput.keyUp("ctrl")
                pyautogui.keyUp("ctrl")
            except Exception: pass
        time.sleep(0.4)
        print(f"[type ] bot_count={bot_count} pasted via clipboard (select-all first)", flush=True)
        # Debug: capture the count field so we can confirm what value actually stuck.
        try:
            ts0 = int(time.time())
            pyautogui.screenshot().crop((1950, 865, 2320, 985)).save(
                str(DEBUG_DIR / f"botcount_{ts0}.png"))
            print(f"[type ] saved botcount field crop botcount_{ts0}.png", flush=True)
        except Exception as e:
            print(f"  botcount crop err: {e}", flush=True)
    if not click_when("step5.png", timeout=15, label="PLAY (race-select)"):
        return False
    # step6 = host-config Start! button on the Game setup panel that appears once
    # the map is loaded. This must be clicked to begin the race countdown.
    if not click_when("step6.png", timeout=120, label="Start (host-config)"):
        return False
    # Wait up to 10 min for the Continue button to appear (race finished)
    if not click_when("step7.png", timeout=600, label="Continue"):
        return False
    # After Continue the game shows either:
    #  (a) a Winner splash panel with a "Final Results" button (step8), OR
    #  (b) the Results table directly (Race Menu / step9 in the sidebar).
    # Two facts about this screen:
    #  1. The in-game cursor is HIDDEN after Tab and only wakes when the mouse makes a
    #     LARGE move. A small move (e.g. from the adjacent Continue button) does NOT
    #     wake it, so the click lands on nothing.
    #  2. The Final Results button is at different coordinates in race vs royale, so we
    #     LOCATE it by image (step8) rather than hardcoding — clicking the found spot is
    #     fine; the earlier failures were the small-move issue, not the image lookup.
    # Sequence: locate step8 -> park mouse far away -> Tab -> click the found location
    # (the big move onto it wakes the cursor and lands the click). Retry; each Tab
    # flips the on/off toggle so it converges in a few tries.
    s9img = str(SCREENSHOTS / "step9.png")
    time.sleep(0.8)
    # Defensive: make sure no modifier is stuck before the cursor-wake Tabs.
    for k in ("ctrl", "shift", "alt"):
        try:
            if pydirectinput: pydirectinput.keyUp(k)
            pyautogui.keyUp(k)
        except Exception: pass
    # Locate the Final Results button ONCE by image (works for both race and royale
    # layouts — the button sits at different coords per mode). Doing it once up front,
    # not every retry, keeps the retry loop identical to the version proven in-script.
    fr_loc = find("step8.png", timeout=4, confidence=0.85)
    FR_XY = (fr_loc.x, fr_loc.y) if fr_loc else (670, 1120)
    advanced = False
    for fr_attempt in range(12):
        try:
            if pyautogui.locateCenterOnScreen(s9img, confidence=0.9):
                advanced = True
                print(f"[fr] results table reached (attempt {fr_attempt+1})", flush=True)
                break
        except Exception:
            pass
        # Park FAR so the move onto the button is large (wakes the cursor); Tab; move
        # onto the button; then a TINY jiggle ON the button to register the hover
        # (the button won't accept the click until it's hovered); then click in place.
        pyautogui.moveTo(1280, 400)
        time.sleep(0.1)
        if pydirectinput: pydirectinput.press("tab")
        else: pyautogui.press("tab")
        time.sleep(0.4)
        pyautogui.moveTo(*FR_XY)
        time.sleep(0.1)
        pyautogui.moveTo(FR_XY[0] + 3, FR_XY[1] + 2)   # tiny hover jiggle
        time.sleep(0.05)
        pyautogui.moveTo(*FR_XY)
        time.sleep(0.08)
        pyautogui.click()                               # click at current (hovered) pos
        time.sleep(0.9)
        print(f"[fr] tab+hover+click Final Results @{FR_XY[0]},{FR_XY[1]} attempt {fr_attempt+1}", flush=True)
    if not advanced:
        print("[fr] failed to reach results table", flush=True)
        return False
    time.sleep(0.5)  # extra settle after Race Menu appears
    try:
        full = pyautogui.screenshot()
        ts = int(time.time())
        # Full results screen (for locating UI elements / new reference crops)
        full.save(str(DEBUG_DIR / f"verify_{ts}_full.png"))
        # First-place row (50px tall) for quick winner check
        full.crop((550, 300, 850, 350)).save(str(DEBUG_DIR / f"verify_{ts}_first.png"))
        # Primary check: the copy-to-clipboard button gives the FULL standings as
        # text in placement order (winner first, loser last) — exact names, all
        # rows, no scrolling. Saved to verify_{ts}_results.tsv.
        players = read_results_via_clipboard(ts)
        dismiss_copy_dialog()   # the copy click opens a modal that blocks all other clicks
        if players is None:
            # Fallback (clipboard failed): screenshot crops of the table.
            # Top-10 block (500px tall = 10 rows of 50px each)
            full.crop((550, 300, 850, 800)).save(str(DEBUG_DIR / f"verify_{ts}_top10.png"))
            # Jump to bottom by DRAGGING the scrollbar thumb down its full travel.
            # Mouse-wheel scrolling is unreliable on this list (doesn't reach the end of
            # a 100-row royale), so grab the scrollbar at (1847,400) and drag to y=1400.
            pyautogui.moveTo(1847, 400, duration=0.2)
            time.sleep(0.1)
            pyautogui.mouseDown(button='left')
            time.sleep(0.1)
            pyautogui.moveTo(1847, 1400, duration=0.8)
            time.sleep(0.1)
            pyautogui.mouseUp(button='left')
            time.sleep(0.6)
            full2 = pyautogui.screenshot()
            # Capture a TALL bottom band (~9 rows) so the true last row is in-frame even
            # if exact alignment varies — the royale loser is the bottom row.
            full2.crop((550, 1000, 850, 1450)).save(str(DEBUG_DIR / f"verify_{ts}_last.png"))
            print(f"[verify] saved first/top10/last crops ts={ts}", flush=True)
    except Exception as e:
        print(f"[verify] err: {e}", flush=True)
    if pause_results:
        print("[pause] leaving game ON the results table for manual inspection "
              "(skipping Race Menu). Scroll up to re-read rows.", flush=True)
        return True
    # Race Menu → map selection. Same cursor rule as Final Results: Tab to wake the
    # cursor, then click the FIXED Race Menu coordinate immediately; retry until the
    # map-select screen (the map card) reappears.
    RM_XY = (116, 751)
    map_img = str(SCREENSHOTS / map_image)
    map_conf = PER_IMAGE_CONF.get(map_image, DEFAULT_CONF)
    for rm_attempt in range(8):
        try:
            if pyautogui.locateCenterOnScreen(map_img, confidence=map_conf):
                print(f"[rm] map-select reached (attempt {rm_attempt+1})", flush=True)
                return True
        except Exception:
            pass
        # Same as Final Results: park far, Tab, move onto the button, tiny hover
        # jiggle (so the button registers the hover), then click in place.
        pyautogui.moveTo(1280, 700)
        time.sleep(0.1)
        if pydirectinput: pydirectinput.press("tab")
        else: pyautogui.press("tab")
        time.sleep(0.4)
        pyautogui.moveTo(*RM_XY)
        time.sleep(0.1)
        pyautogui.moveTo(RM_XY[0] + 3, RM_XY[1] + 2)   # tiny hover jiggle
        time.sleep(0.05)
        pyautogui.moveTo(*RM_XY)
        time.sleep(0.08)
        pyautogui.click()
        time.sleep(1.0)
        print(f"[rm] tab+hover+click Race Menu attempt {rm_attempt+1}", flush=True)
    print("[rm] failed to reach map-select", flush=True)
    return False

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    skip_launch = "--skip-launch" in flags or "--skip-menu" in flags
    skip_menu   = "--skip-menu"   in flags
    royale        = "--royale"        in flags
    pause_results = "--pause-results" in flags
    n_races = int(args[0]) if len(args) > 0 else 3
    # All remaining positional args after N are maps to rotate through.
    # Royale defaults to the Battle Stadium card; Race defaults to random dice.
    if len(args) > 1:
        map_rotation = args[1:]
    else:
        map_rotation = ["step3_battle_stadium.png"] if royale else ["step3.png"]

    if not skip_launch:
        print(f"Launching game via Steam ({STEAM_URL}) and running {n_races} race(s); maps={map_rotation}", flush=True)
        subprocess.Popen(["cmd", "/c", "start", "", STEAM_URL], shell=False)
        print("Sleeping 15s for Steam dialog dismiss + initial game window...", flush=True)
        time.sleep(15)

    if not skip_menu:
        if not click_when("step1.png", timeout=180, label="Start"):
            sys.exit("FAIL: never saw step1 (game launch)")
        # Mode-select screen: Race button (step2.png) vs Royale button.
        mode_btn = "step2_royale.png" if royale else "step2.png"
        mode_lbl = "Royale" if royale else "PLAY (Race)"
        if not click_when(mode_btn, timeout=60, label=mode_lbl):
            sys.exit(f"FAIL: never saw {mode_btn}")

    completed = 0
    for i in range(n_races):
        spec = map_rotation[i % len(map_rotation)]
        # Map spec is "image.png" or "image.png:NN" (with bot count)
        if ":" in spec:
            m, bots_str = spec.rsplit(":", 1)
            try:
                bots = int(bots_str)
            except ValueError:
                m, bots = spec, None
        else:
            m, bots = spec, None
        ok = run_one_race(m, bot_count=bots, pause_results=pause_results)
        if not ok:
            print(f"!!! Race {i+1} ({spec}) aborted (timeout). Stopping.", flush=True)
            break
        completed += 1
        print(f"=== Race {i+1}/{n_races} ({spec}) done ===", flush=True)
    print(f"\nCompleted {completed}/{n_races} races. Closing.", flush=True)

if __name__ == "__main__":
    main()
