"""Dummy results/events server for MikMarbleMod.

Serves the three endpoints the mod POSTs to, under a base path (default /mod):

    POST {base}/ready    one-shot handshake when the mod starts
    POST {base}/results  final standings, once per match (legacy payload)
    POST {base}/event    live gameplay events (3.23+)

Run several copies on different ports/paths to exercise the mod's multi-target
support, e.g.:

    python server/server.py                         # 0.0.0.0:5000/mod
    python server/server.py --port 5001 --base /b   # a second target

Every request can also be appended to a JSONL file (--log) for later checks.
"""
import argparse
import json
import sys
import threading
import time

from flask import Flask, request, jsonify
from werkzeug.exceptions import BadRequest

app = Flask(__name__)

ARGS = None
_log_lock = threading.Lock()

# Running per-event counts, printed with each line so bursts are easy to follow.
_counts = {}

KNOWN_EVENTS = {
    "map_entered",
    "player_joined",
    "marble_finished",
    "marble_eliminated",
    "marble_damaged",
    "countdown",
    "grand_prix_finished",
    "marble_resurrected",
}


def _log_request(kind, data):
    if not ARGS or not ARGS.log:
        return
    rec = {"received": time.time(), "kind": kind, "path": request.path, "body": data}
    with _log_lock:
        with open(ARGS.log, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")


def _out(msg):
    print(msg, flush=True)


# ── ready ───────────────────────────────────────────────────────────────────

def mod_ready():
    # The mod sends {"mod": "MikMarbleMod", "version": "x.y", ...}. 3.23 adds
    # "results" (bool) and "events" (the event names this target will get).
    # Older builds sent less ({} before 3.21), so every field is optional.
    info = request.get_json(silent=True) or {}
    _log_request("ready", info)
    version = info.get("version")
    mod_name = info.get("mod", "mod")
    if version:
        _out(f"Ready: {mod_name} v{version}")
    else:
        _out(f"Ready: {mod_name} (no version reported — pre-3.21 build?)")
    if "events" in info:
        events = info.get("events") or []
        _out(f"  results: {'on' if info.get('results', True) else 'off'}; "
             f"events: {', '.join(events) if events else '(none)'}")
    return jsonify({"message": "The mod is ready", "version": version}), 200


# ── results (legacy, unchanged) ─────────────────────────────────────────────

def receive_results():
    raw_body_for_logging = None
    try:
        raw_body_for_logging = request.data
        if ARGS and ARGS.verbose:
            _out(f"Request headers: {dict(request.headers)}")
            _out(f"Raw request data: {raw_body_for_logging}")
        data = request.get_json()
        _log_request("results", data)

        if not data or not isinstance(data.get('players'), list):
            return jsonify({"error": "Invalid payload. Expected JSON with a 'players' list."}), 400

        game_type = data.get('type', 'unknown')
        players = data['players']

        for p in players:
            if not isinstance(p, dict):
                return jsonify({"error": "Each player must be an object."}), 400
            if not isinstance(p.get('name'), str):
                return jsonify({"error": "Player 'name' must be a string."}), 400
            if not isinstance(p.get('finished'), bool):
                return jsonify({"error": "Player 'finished' must be a bool."}), 400
            if not isinstance(p.get('eliminations', 0), int) or isinstance(p.get('eliminations'), bool):
                return jsonify({"error": "Player 'eliminations' must be an int."}), 400
            if not isinstance(p.get('damage', 0), int) or isinstance(p.get('damage'), bool):
                return jsonify({"error": "Player 'damage' must be an int."}), 400

        # Players arrive in placement order: winners first (1st..Nth), then DNFs ordered
        # from highest-placed DNF to lowest (loser is the LAST entry).
        finished = [p for p in players if p['finished']]
        dnfs = [p for p in players if not p['finished']]

        winner = finished[0]['name'] if finished else None
        loser = None
        if game_type == 'royale':
            loser = dnfs[-1]['name'] if dnfs else None  # first eliminated = last in list
        elif game_type == 'bullseye':
            # Score-ranked: everyone finished, best first, lowest score is last.
            loser = finished[-1]['name'] if finished else None

        _out("=" * 60)
        _out(f"RESULTS  type={game_type}  players={len(players)} "
             f"({len(finished)} finished, {len(dnfs)} DNF)")
        for i, p in enumerate(players, 1):
            flag = "OK " if p['finished'] else "DNF"
            elims = p.get('eliminations', 0)
            elims_note = f"  (elims: {elims})" if elims else ""
            _out(f"  {i:>2}. [{flag}] {p['name']}{elims_note}")
        if winner:
            _out(f"Winner: {winner}")
        if loser:
            _out(f"Loser:  {loser}")
        if players and not finished:
            _out("Note: all players DNF.")
        _out("=" * 60)

        return jsonify({
            "message": "Results received successfully",
            "type": game_type,
            "winner": winner,
            "loser": loser,
            "players": players,
        }), 200
    except BadRequest as e:
        error_message = f"BadRequest: {str(e)}"
        _out(error_message)
        if raw_body_for_logging:
            _out(f"Raw request body that caused BadRequest: "
                 f"{raw_body_for_logging.decode('utf-8', errors='replace')}")
        return jsonify({"error": error_message}), 400
    except Exception as e:
        _out(f"An unexpected error occurred: {str(e)}")
        return jsonify({"error": "An unexpected error occurred on the server."}), 500


# ── events (3.23+) ──────────────────────────────────────────────────────────

def _who(e):
    name, disp = e.get("name"), e.get("display_name")
    return disp if disp and disp.lower() != (name or "").lower() else (disp or name or "?")


def _describe(e):
    ev = e.get("event")
    if ev == "map_entered":
        gp = "  [grand prix]" if e.get("grand_prix") else ""
        return f"map '{e.get('map')}'  mode={e.get('mode')}  level={e.get('level')}{gp}"
    if ev == "player_joined":
        how = f"with {e['command']!r}" if e.get("command") else ("via chat" if e.get("via_chat") else "(bot / no chat command)")
        return f"{_who(e)} joined {how}"
    if ev == "marble_finished":
        return f"{_who(e)} finished #{e.get('position')}" + ("  WINNER" if e.get("winner") else "")
    if ev == "marble_eliminated":
        by = f" by {e['killer']}" if e.get("killer") else ""
        return f"{_who(e)} eliminated{by}  (#{e.get('elimination_order')} out)"
    if ev == "marble_resurrected":
        return f"{_who(e)} RESURRECTED (back in the royale)"
    if ev == "marble_damaged":
        by = f" from {e['attacker']}" if e.get("attacker") else ""
        return f"{_who(e)} took {e.get('damage')} dmg{by}  hp {e.get('health')}/{e.get('max_health')}"
    if ev == "countdown":
        return f"{e.get('phase')} countdown: {e.get('seconds_left')}"
    if ev == "grand_prix_finished":
        rows = e.get("standings") or []
        lines = [f"grand prix finished after {e.get('races')} races, {len(rows)} players"]
        for r in rows[:10]:
            lines.append(f"      {r.get('position'):>3}. {_who(r):<24} {r.get('points'):>6} pts  "
                         f"{r.get('wins')} wins")
        if len(rows) > 10:
            lines.append(f"      ... {len(rows) - 10} more")
        return "\n".join(lines)
    return json.dumps(e, ensure_ascii=False)


def receive_event():
    e = request.get_json(silent=True)
    if not isinstance(e, dict) or not isinstance(e.get("event"), str):
        return jsonify({"error": "Expected a JSON object with an 'event' string."}), 400
    _log_request("event", e)
    ev = e["event"]
    _counts[ev] = _counts.get(ev, 0) + 1

    quiet = ARGS and ev in ARGS.quiet
    if not quiet:
        unknown = "" if ev in KNOWN_EVENTS else "  (unknown event)"
        test = "  [TEST]" if e.get("test") else ""
        _out(f"[{ev:<19}] {_describe(e)}{test}{unknown}")
    return jsonify({"ok": True, "event": ev, "count": _counts[ev]}), 200


def catch_all(subpath):
    _out(f"Received request: {request.method} /{subpath}")
    base = ARGS.base if ARGS else "/mod"
    return jsonify({"error": f"Invalid route. Use {base}/ready, {base}/results or {base}/event."}), 404


def register_routes(base):
    base = "/" + base.strip("/") if base.strip("/") else ""
    app.add_url_rule(f"{base}/ready", "ready", mod_ready, methods=["POST"])
    app.add_url_rule(f"{base}/results", "results", receive_results, methods=["POST"])
    app.add_url_rule(f"{base}/event", "event", receive_event, methods=["POST"])
    app.add_url_rule("/<path:subpath>", "catch_all", catch_all, methods=["GET", "POST"])
    return base


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description="MikMarbleMod dummy results/events server")
    ap.add_argument("--host", default="0.0.0.0", help="interface to listen on (default: all)")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--base", default="/mod", help="base path the mod is configured with (default /mod)")
    ap.add_argument("--log", help="append every request as JSON lines to this file")
    ap.add_argument("--quiet", default="", help="comma list of events not to print (still counted/logged), "
                                                 "e.g. marble_damaged,countdown")
    ap.add_argument("--verbose", action="store_true", help="also print raw headers/bodies for /results")
    ARGS = ap.parse_args()
    ARGS.quiet = {q.strip() for q in ARGS.quiet.split(",") if q.strip()}
    ARGS.base = register_routes(ARGS.base)

    print(f"Starting MikMarbleMod dummy server on http://{ARGS.host}:{ARGS.port}{ARGS.base}"
          f"  (ready / results / event)", flush=True)
    # No reloader: it would run two processes appending to the same --log file.
    app.run(host=ARGS.host, port=ARGS.port, debug=False, use_reloader=False, threaded=True)
