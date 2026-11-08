"""Test whether a `claude setup-token` token can read Claude plan usage.

Reads the token from the file %USERPROFILE%\\.claude-usage-token (never printed).
Calls the undocumented endpoint Claude Code uses and prints only status + percentages.
Usage: python check_usage.py
"""
import json
import pathlib
import sys
import urllib.error
import urllib.request

TOKEN_FILE = pathlib.Path.home() / ".claude-usage-token"
URL = "https://api.anthropic.com/api/oauth/usage"


def main():
    if not TOKEN_FILE.exists():
        sys.exit(f"Token file not found: {TOKEN_FILE}")
    token = TOKEN_FILE.read_text(encoding="utf-8").strip()
    print(f"Token loaded ({len(token)} chars, starts with {token[:10]!r})")

    req = urllib.request.Request(URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
        "User-Agent": "esp32-quota-test",
    })
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.load(resp)
            print(f"HTTP {resp.status}: OK")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")[:300]
        sys.exit(f"HTTP {e.code}: {body}")

    for key in ("five_hour", "seven_day"):
        w = data.get(key) or {}
        print(f"{key}: utilization={w.get('utilization')} resets_at={w.get('resets_at')}")
    print("Other keys:", sorted(k for k in data if k not in ("five_hour", "seven_day")))


if __name__ == "__main__":
    main()
