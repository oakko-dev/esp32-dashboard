"""Claude Code status line that also sends plan usage to the ESP32 quota board.

Claude Code pipes session JSON to stdin. We read rate_limits.five_hour / seven_day,
print a short status line, and POST the numbers to the board (at most once a minute
unless they change). Board address: env QUOTA_BOARD, default http://claude-quota.local
"""
import json
import os
import pathlib
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from datetime import datetime

BOARD = os.environ.get("QUOTA_BOARD", "http://claude-quota.local").rstrip("/")
STATE = pathlib.Path(tempfile.gettempdir()) / "claude-quota-last.json"
RESEND_S = 60


def to_epoch(v):
    """resets_at has been seen both as epoch seconds and as an ISO-8601 string."""
    if v is None:
        return 0
    if isinstance(v, (int, float)):
        return int(v)
    try:
        return int(datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp())
    except ValueError:
        return 0


def window(rl, key):
    w = rl.get(key) or {}
    pct = w.get("used_percentage", w.get("utilization"))
    return (None if pct is None else float(pct)), to_epoch(w.get("resets_at", w.get("reset_at")))


def build_params(data):
    rl = data.get("rate_limits") or {}
    params = {}
    for name, key in (("h5", "five_hour"), ("d7", "seven_day")):
        pct, reset = window(rl, key)
        if pct is not None:
            params[name] = f"{pct:.1f}"
            params[name + "r"] = str(reset)
    return params


def status_text(params):
    parts = []
    if "h5" in params:
        parts.append(f"5h {float(params['h5']):.0f}%")
    if "d7" in params:
        parts.append(f"7d {float(params['d7']):.0f}%")
    return " | ".join(parts)


def send(params):
    try:
        last = json.loads(STATE.read_text())
    except (OSError, ValueError):
        last = {}
    if last.get("params") == params and time.time() - last.get("t", 0) < RESEND_S:
        return
    try:
        body = urllib.parse.urlencode(params).encode()
        urllib.request.urlopen(f"{BOARD}/quota", data=body, timeout=2).read()
        STATE.write_text(json.dumps({"params": params, "t": time.time()}))
    except Exception:
        pass  # board offline must never break the status line


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        data = {}
    params = build_params(data)
    print(status_text(params))
    if params:
        send(params)


if __name__ == "__main__":
    main()
