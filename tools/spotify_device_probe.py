"""Does Spotify's own desktop client id log in with a device code (the way a TV does), and is the one token enough for
everything Pocket does? No redirect, no dev app, nothing hosted: the 3DS would show a code, the person types it at
spotify.com/pair on their phone, and the 3DS polls for the token.

    python -I tools/spotify_device_probe.py

Asks for the scopes Pocket's two logins use together, waits for you to approve the code, then prints which scopes
Spotify really granted and whether the Web API answers with that token. Writes nothing to disk and prints no tokens.
"""
import json, sys, time, urllib.error, urllib.parse, urllib.request

CLIENT_ID = "65b708073fc0480ea92a077233ca87bd"   # Spotify's desktop app (the id librespot uses)
SCOPES = ("streaming user-read-private user-read-email user-read-playback-state user-modify-playback-state "
          "user-read-currently-playing playlist-read-private playlist-read-collaborative "
          "playlist-modify-private playlist-modify-public user-library-read user-library-modify app-remote-control")
DEVICE = "https://accounts.spotify.com/oauth2/device/authorize"
TOKEN = "https://accounts.spotify.com/api/token"
API = "https://api.spotify.com/v1"


def post(url, fields):
    req = urllib.request.Request(url, urllib.parse.urlencode(fields).encode(), {"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def get(path, tok):
    req = urllib.request.Request(API + path, headers={"Authorization": "Bearer " + tok})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.load(r), {}
    except urllib.error.HTTPError as e:
        body = e.read()[:200].decode("utf-8", "replace")
        return e.code, {"body": body}, {k: v for k, v in e.headers.items() if k.lower() in ("retry-after", "x-ratelimit-remaining", "www-authenticate")}


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    code, d = post(DEVICE, {"client_id": CLIENT_ID, "scope": SCOPES})
    if code != 200:
        sys.exit("device authorize: %s %s" % (code, d))
    print("CODE %s   open %s on your phone and type it in" % (d["user_code"], d["verification_uri"]), flush=True)
    end = time.time() + d["expires_in"]
    while time.time() < end:
        time.sleep(d.get("interval", 5))
        code, t = post(TOKEN, {"grant_type": "urn:ietf:params:oauth:grant-type:device_code", "device_code": d["device_code"], "client_id": CLIENT_ID})
        if code == 200:
            break
        if t.get("error") not in ("authorization_pending", "slow_down"):
            sys.exit("poll: %s %s" % (code, t))
    else:
        sys.exit("the code ran out")
    got = set(t.get("scope", "").split())
    want = set(SCOPES.split())
    print("approved. token_type=%s expires_in=%s refresh_token=%s" % (t.get("token_type"), t.get("expires_in"), "yes" if t.get("refresh_token") else "NO"))
    print("scopes granted:", " ".join(sorted(got)))
    print("scopes asked for but not granted:", " ".join(sorted(want - got)) or "none")
    tok = t["access_token"]
    for path in ("/me", "/me/playlists?limit=1", "/me/tracks?limit=1", "/me/player/devices", "/me/player"):
        c, j, h = get(path, tok)
        if c == 429:
            wait = min(int(h.get("retry-after", "5") or 5), 25)
            print("GET %-24s -> 429 %s %s; waiting %ss and trying once more" % (path, h, j.get("body", ""), wait), flush=True)
            time.sleep(wait)
            c, j, h = get(path, tok)
        extra = "  product=%s" % j.get("product") if path == "/me" and c == 200 else ("" if c == 200 else "  %s %s" % (h, j.get("body", "")))
        print("GET %-24s -> %s%s" % (path, c, extra), flush=True)
    if t.get("refresh_token"):
        c, r = post(TOKEN, {"grant_type": "refresh_token", "refresh_token": t["refresh_token"], "client_id": CLIENT_ID})
        print("refresh -> %s, rotated=%s" % (c, "yes" if r.get("refresh_token") and r["refresh_token"] != t["refresh_token"] else "no"))


if __name__ == "__main__":
    main()
