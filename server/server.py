from flask import Flask, request, jsonify
from werkzeug.exceptions import BadRequest

app = Flask(__name__)

@app.route("/*", methods=['GET', 'POST'])
def catch_all():
    print(f"Received request: {request.method} {request.path}")
    return jsonify({"error": "Invalid route. Please use /mod/results or /mod/ready."}), 404

@app.route('/mod/ready', methods=['POST'])
def mod_ready():
    return jsonify({"message": "The mod is ready"}), 200

@app.route('/mod/results', methods=['POST'])
def receive_results():
    raw_body_for_logging = None
    try:
        print(f"Request headers: {dict(request.headers)}")
        raw_body_for_logging = request.data
        print(f"Raw request data: {raw_body_for_logging}")
        data = request.get_json()
        print(f"Parsed JSON data: {data}")

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

        finished_count = len(finished)
        dnf_count = len(dnfs)

        print(f"Game type: {game_type}")
        print(f"Players ({len(players)} total, {finished_count} finished, {dnf_count} DNF):")
        for i, p in enumerate(players, 1):
            flag = "OK " if p['finished'] else "DNF"
            elim = p.get('eliminations', 0)
            dmg = p.get('damage', 0)
            print(f"  {i:>2}. [{flag}] {p['name']:<20} elim={elim:<3} dmg={dmg}")
        if winner:
            print(f"Winner: {winner}")
        if loser:
            print(f"Loser:  {loser}")
        if players and finished_count == 0:
            print("Note: all players DNF.")

        return jsonify({
            "message": "Results received successfully",
            "type": game_type,
            "winner": winner,
            "loser": loser,
            "players": players,
        }), 200
    except BadRequest as e:
        error_message = f"BadRequest: {str(e)}"
        print(error_message)
        if raw_body_for_logging:
            try:
                decoded_body = raw_body_for_logging.decode('utf-8', errors='replace')
                print(f"Raw request body that caused BadRequest: {decoded_body}")
            except Exception as decode_err:
                print(f"Could not decode raw body: {str(decode_err)}")
                print(f"Raw request body (bytes): {raw_body_for_logging}")
        else:
            print("Could not retrieve raw body before BadRequest.")
        return jsonify({"error": error_message}), 400
    except Exception as e:
        print(f"An unexpected error occurred: {str(e)}")
        if raw_body_for_logging:
            try:
                decoded_body = raw_body_for_logging.decode('utf-8', errors='replace')
                print(f"Raw request body (in generic error context): {decoded_body}")
            except Exception as decode_err:
                print(f"Could not decode raw body: {str(decode_err)}")
                print(f"Raw request body (bytes): {raw_body_for_logging}")
        return jsonify({"error": "An unexpected error occurred on the server."}), 500

if __name__ == '__main__':
    # To run this server:
    # 1. Make sure Flask is installed: pip install Flask
    # 2. Execute this file: python server.py
    print("Starting Flask server on http://localhost:5000")
    app.run(host='0.0.0.0', port=5000, debug=True) # Listen on all network interfaces for easier access from potential VMs/containers 