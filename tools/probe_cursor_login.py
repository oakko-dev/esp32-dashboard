"""Cursor PKCE login + refresh probe (same flow the board would use).

  python probe_cursor_login.py start    -> prints login URL, saves verifier/uuid
  python probe_cursor_login.py poll     -> waits for login, saves tokens, tests usage + one refresh

Tokens go to ~/.cursor-board-tokens.json and are never printed.
Flow taken from OpenUsage (MIT): Sources/OpenUsage/Services/DeviceProviderLogins.swift, CursorUsageClient.swift.
"""
import base64, hashlib, json, os, pathlib, secrets, sys, time, urllib.error, urllib.parse, urllib.request, uuid

CLIENT_ID = "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB"
STATE = pathlib.Path.home() / ".cursor-board-login-state.json"
TOKENS = pathlib.Path.home() / ".cursor-board-tokens.json"
UA = "esp32-quota-probe"


def b64url(b):
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


def jwt_payload(tok):
    p = tok.split(".")[1]
    return json.loads(base64.urlsafe_b64decode(p + "=" * (-len(p) % 4)))


def req(method, url, headers=None, body=None):
    data = None if body is None else json.dumps(body).encode()
    h = {"User-Agent": UA, **(headers or {})}
    if data is not None:
        h.setdefault("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=data, headers=h, method=method), timeout=15) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def save_private(path, obj):
    path.write_text(json.dumps(obj))
    os.chmod(path, 0o600)


def describe(tok, label):
    p = jwt_payload(tok)
    exp = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(p["exp"]))
    print(f"{label}: exp {exp}, type={p.get('type')}, scope={p.get('scope')}, len={len(tok)}")


def test_usage(tok):
    sub = jwt_payload(tok)["sub"]
    user = sub.split("|")[1] if "|" in sub else sub
    code, body = req("GET", "https://cursor.com/api/usage-summary",
                     {"Cookie": f"WorkosCursorSessionToken={user}%3A%3A{tok}", "Accept": "application/json"})
    print(f"usage-summary (cookie from new token): HTTP {code}")
    if code == 200:
        plan = json.loads(body).get("individualUsage", {}).get("plan", {})
        print("  plan:", {k: plan.get(k) for k in ("totalPercentUsed", "autoPercentUsed", "apiPercentUsed")})
    else:
        print("  ", body[:200].replace("\n", " "))
    code, body = req("POST", "https://api2.cursor.sh/aiserver.v1.DashboardService/GetCurrentPeriodUsage",
                     {"Authorization": f"Bearer {tok}", "Connect-Protocol-Version": "1"}, {})
    print(f"GetCurrentPeriodUsage (Bearer): HTTP {code}")
    print("  ", body[:1200])


def start():
    verifier = b64url(secrets.token_bytes(32))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    u = str(uuid.uuid4())
    save_private(STATE, {"verifier": verifier, "uuid": u})
    q = urllib.parse.urlencode({"challenge": challenge, "uuid": u, "mode": "login", "redirectTarget": "cli"})
    print("https://cursor.com/loginDeepControl?" + q)


def poll():
    st = json.loads(STATE.read_text())
    url = "https://api2.cursor.sh/auth/poll?" + urllib.parse.urlencode({"uuid": st["uuid"], "verifier": st["verifier"]})
    deadline = time.time() + 600
    while time.time() < deadline:
        code, body = req("GET", url)
        if code == 200:
            t = json.loads(body)
            if t.get("accessToken") and t.get("refreshToken"):
                break
        elif code not in (202, 404):
            print(f"poll failed: HTTP {code} {body[:200]}")
            return
        time.sleep(2)
    else:
        print("poll timed out (600 s)")
        return
    STATE.unlink()
    save_private(TOKENS, {"accessToken": t["accessToken"], "refreshToken": t["refreshToken"]})
    print("login OK; keys in reply:", sorted(t.keys()))
    print("refreshToken == accessToken:", t["refreshToken"] == t["accessToken"])
    describe(t["accessToken"], "access")
    test_usage(t["accessToken"])

    code, body = req("POST", "https://api2.cursor.sh/oauth/token", body={
        "grant_type": "refresh_token", "client_id": CLIENT_ID, "refresh_token": t["refreshToken"]})
    print(f"oauth/token refresh: HTTP {code}")
    r = json.loads(body) if body.startswith("{") else {}
    print("  keys:", sorted(r.keys()), "shouldLogout:", r.get("shouldLogout"))
    new = r.get("access_token")
    if code == 200 and new:
        save_private(TOKENS, {"accessToken": new, "refreshToken": r.get("refresh_token") or new})
        print("  new token differs:", new != t["accessToken"])
        describe(new, "refreshed")
        test_usage(new)


if __name__ == "__main__":
    {"start": start, "poll": poll}[sys.argv[1]]()
