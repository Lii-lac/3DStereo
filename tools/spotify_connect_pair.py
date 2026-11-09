"""Pair Pocket's Spotify Connect (the 3DS in Spotify's device list), once, on the PC.

This login is the Spotify desktop app's client id, the way librespot does it (Pocket's own app id
isn't allowed to be a player). Authorization Code + PKCE, in your own browser; the refresh token
lands in S:\\3ds\\player\\spotify-connect.json and nowhere else. Pocket's usual pairing
(tools/spotify_pair.py, spotify.json) is still needed too: the 3DS looks songs up with it.

    python tools/spotify_connect_pair.py                 # pair, write to the SD card
    python tools/spotify_connect_pair.py --out x         # somewhere else (then ftpd it over)
    python tools/spotify_connect_pair.py --from-proof    # take the login connect/proof already made
"""
import argparse
import base64
import hashlib
import http.server
import json
import secrets
import sys
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"   # Spotify's desktop app (librespot's KEYMASTER_CLIENT_ID)
PORT = 8898
REDIRECT = f"http://127.0.0.1:{PORT}/login"
SCOPES = ("streaming user-read-private user-read-email user-read-playback-state user-modify-playback-state "
          "user-read-currently-playing playlist-read-private playlist-read-collaborative user-library-read "
          "app-remote-control")
DEFAULT_OUT = Path("S:/3ds/player/spotify-connect.json")
PROOF = HERE.parent / "connect" / "proof" / f".token-{CLIENT_ID[:8]}.json"


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def post_form(url: str, fields: dict) -> dict:
    req = urllib.request.Request(url, urllib.parse.urlencode(fields).encode(),
                                 {"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def login() -> str:
    verifier = b64url(secrets.token_bytes(64))
    state = b64url(secrets.token_bytes(16))
    got = {}

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            if u.path != "/login":
                self.send_error(404)
                return
            q = urllib.parse.parse_qs(u.query)
            ok = q.get("state", [""])[0] == state and "code" in q
            got["code"] = q["code"][0] if ok else None
            got["error"] = q.get("error", ["state mismatch"])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = "Paired. You can close this tab." if ok else f"Pairing failed: {got['error']}"
            self.wfile.write(f"<p style='font:16px sans-serif'>{msg}</p>".encode())

        def log_message(self, *a):
            pass

    url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": CLIENT_ID, "response_type": "code", "redirect_uri": REDIRECT,
        "code_challenge_method": "S256", "code_challenge": b64url(hashlib.sha256(verifier.encode()).digest()),
        "scope": SCOPES, "state": state})
    print("Opening Spotify in your browser. If it doesn't open, go to:\n" + url)
    webbrowser.open(url)
    with http.server.HTTPServer(("127.0.0.1", PORT), H) as srv:
        while "code" not in got:
            srv.handle_request()
    if not got["code"]:
        sys.exit(f"Spotify said no: {got['error']}")
    tok = post_form("https://accounts.spotify.com/api/token", {
        "grant_type": "authorization_code", "code": got["code"], "redirect_uri": REDIRECT,
        "client_id": CLIENT_ID, "code_verifier": verifier})
    return tok["refresh_token"]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--from-proof", action="store_true")
    a = ap.parse_args()
    if a.from_proof:
        if not PROOF.exists():
            sys.exit(f"No {PROOF}: run without --from-proof to log in.")
        refresh = json.loads(PROOF.read_text())["refresh_token"]
        # check it, and take the rotated one if Spotify hands one back
        tok = post_form("https://accounts.spotify.com/api/token", {
            "grant_type": "refresh_token", "refresh_token": refresh, "client_id": CLIENT_ID})
        refresh = tok.get("refresh_token") or refresh
        PROOF.unlink()   # one login, one owner: two places refreshing it would spend it twice
    else:
        refresh = login()
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps({"client_id": CLIENT_ID, "refresh_token": refresh}, indent=2))
    print(f"Saved to {a.out}")


if __name__ == "__main__":
    main()
