"""Pair Pocket (the 3DS player) with a Spotify account, once, on the PC.

Authorization Code + PKCE: no client secret exists anywhere. You log in in your
own browser; the refresh token lands in S:\\3ds\\player\\spotify.json and nowhere
else. Re-run it whenever the token is revoked.

    python tools/spotify_pair.py            # pair, write to the SD card
    python tools/spotify_pair.py --out x    # write somewhere else (then ftpd it over)
    python tools/spotify_pair.py --check    # refresh the saved token, show account + devices
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
CLIENT_ID_FILE = HERE.parent / "player" / "spotify-client-id.txt"
DEFAULT_OUT = Path("S:/3ds/player/spotify.json")
PORT = 8765
REDIRECT = f"http://127.0.0.1:{PORT}/callback"
SCOPES = ("user-read-playback-state user-modify-playback-state "
          "user-read-currently-playing playlist-read-private playlist-read-collaborative "
          "user-library-read user-library-modify "   # likes, both ways
          "playlist-modify-public playlist-modify-private")   # renaming and adding to lists on the 3DS
AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API = "https://api.spotify.com/v1"


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def post_form(url: str, fields: dict) -> dict:
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, body, {"Content-Type": "application/x-www-form-urlencoded"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def api_get(path: str, token: str) -> dict:
    req = urllib.request.Request(API + path, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def read_client_id() -> str:
    if not CLIENT_ID_FILE.exists():
        sys.exit(f"Put the app's Client ID in {CLIENT_ID_FILE} first "
                 "(developer.spotify.com/dashboard, redirect URI " + REDIRECT + ").")
    cid = CLIENT_ID_FILE.read_text().strip()
    if len(cid) != 32:
        sys.exit(f"{CLIENT_ID_FILE} doesn't look like a Client ID (expected 32 hex characters).")
    return cid


def wait_for_code(state: str) -> str:
    got = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            if url.path != "/callback":
                self.send_error(404)
                return
            q = urllib.parse.parse_qs(url.query)
            ok = q.get("state", [""])[0] == state and "code" in q
            got["code"] = q["code"][0] if ok else None
            got["error"] = q.get("error", ["state mismatch" if not ok else ""])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = "Paired. You can close this tab." if ok else f"Pairing failed: {got['error']}"
            self.wfile.write(f"<p style='font:16px sans-serif'>{msg}</p>".encode())

        def log_message(self, *a):
            pass

    with http.server.HTTPServer(("127.0.0.1", PORT), Handler) as srv:
        while "code" not in got:
            srv.handle_request()
    if not got["code"]:
        sys.exit(f"Spotify said no: {got['error']}")
    return got["code"]


def pair(out: Path) -> None:
    cid = read_client_id()
    verifier = b64url(secrets.token_bytes(64))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = b64url(secrets.token_bytes(16))
    url = AUTH_URL + "?" + urllib.parse.urlencode({
        "client_id": cid, "response_type": "code", "redirect_uri": REDIRECT,
        "code_challenge_method": "S256", "code_challenge": challenge,
        "scope": SCOPES, "state": state,
    })
    print("Opening Spotify in your browser. If it doesn't open, go to:\n" + url)
    webbrowser.open(url)
    code = wait_for_code(state)
    tok = post_form(TOKEN_URL, {
        "grant_type": "authorization_code", "code": code, "redirect_uri": REDIRECT,
        "client_id": cid, "code_verifier": verifier,
    })
    save(out, cid, tok["refresh_token"])
    report(tok["access_token"])
    print(f"Saved to {out}")


def save(out: Path, cid: str, refresh: str) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"client_id": cid, "refresh_token": refresh}, indent=2))


def check(out: Path) -> None:
    saved = json.loads(out.read_text())
    tok = post_form(TOKEN_URL, {"grant_type": "refresh_token",
                                "refresh_token": saved["refresh_token"],
                                "client_id": saved["client_id"]})
    if tok.get("refresh_token") and tok["refresh_token"] != saved["refresh_token"]:
        save(out, saved["client_id"], tok["refresh_token"])  # Spotify rotated it
    report(tok["access_token"])


def report(token: str) -> None:
    me = api_get("/me", token)
    print(f"Account: {me.get('display_name') or me.get('id')}, product: {me.get('product', '?')}")
    if me.get("product") not in ("premium", None):   # Spotify stopped telling apps in 2026
        print("  Not Premium: Pocket can show what's playing but Spotify refuses play/skip/seek.")
    devices = api_get("/me/player/devices", token).get("devices", [])
    for d in devices:
        print(f"  device: {d['name']} ({d['type']}){'  <- active' if d['is_active'] else ''}")
    if not devices:
        print("  No devices awake right now (open Spotify somewhere to see one).")


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # names with emoji in a Windows console
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    check(args.out) if args.check else pair(args.out)


if __name__ == "__main__":
    main()
