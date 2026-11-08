"""Test reading Cursor plan usage from cursor.com/api/usage-summary.

Reads the WorkosCursorSessionToken cookie value from %USERPROFILE%\\.cursor-session-cookie
(never printed). Accepts either `<user_id>::<jwt>` (as copied from the browser, maybe
URL-encoded as %3A%3A) or a bare JWT, in which case the user id is taken from its `sub`.
Prints only HTTP status, token expiry date and the usage fields.
Usage: python probe_cursor.py
"""
import base64
import json
import pathlib
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

COOKIE_FILE = pathlib.Path.home() / ".cursor-session-cookie"
URL = "https://cursor.com/api/usage-summary"


def jwt_payload(jwt):
    part = jwt.split(".")[1]
    return json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))


def main():
    raw = COOKIE_FILE.read_text(encoding="utf-8").strip().replace("%3A%3A", "::")
    if raw.startswith("WorkosCursorSessionToken="):
        raw = raw.split("=", 1)[1]
    jwt = raw.split("::")[-1]
    try:
        claims = jwt_payload(jwt)
        exp = claims.get("exp")
        print("token expires:", datetime.fromtimestamp(exp, timezone.utc).isoformat() if exp else "no exp claim")
        if "::" not in raw:
            raw = claims["sub"].split("|")[-1] + "::" + jwt
            print("added user id prefix from JWT sub")
    except Exception as e:  # not fatal: still try the request as given
        print("could not decode JWT:", type(e).__name__)

    req = urllib.request.Request(URL, headers={
        "Cookie": f"WorkosCursorSessionToken={raw}",
        "User-Agent": "esp32-quota-test",
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            status, body = resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        status, body = e.code, e.read().decode("utf-8", "replace")
    print(f"HTTP {status}")
    try:
        data = json.loads(body)
    except ValueError:
        sys.exit("non-JSON body (first 200 chars): " + body[:200])
    if status != 200:
        sys.exit(f"error body: {json.dumps(data)[:300]}")

    for k in ("billingCycleStart", "billingCycleEnd", "membershipType", "limitType", "isUnlimited"):
        print(f"{k}: {data.get(k)}")
    iu = data.get("individualUsage") or {}
    print("individualUsage.plan:", json.dumps(iu.get("plan"), indent=1))
    print("individualUsage.onDemand:", json.dumps(iu.get("onDemand")))
    print("teamUsage present:", bool(data.get("teamUsage")))


if __name__ == "__main__":
    main()
