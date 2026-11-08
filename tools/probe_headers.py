"""Test reading Claude plan usage from rate-limit response headers.

Sends a 1-token Messages request with the `claude setup-token` token from
%USERPROFILE%\\.claude-usage-token (never printed) and prints only the HTTP status
and the anthropic-ratelimit-* response headers. Each run spends about 1 output token.
Usage: python probe_headers.py [model]
"""
import json
import pathlib
import sys
import urllib.error
import urllib.request

TOKEN_FILE = pathlib.Path.home() / ".claude-usage-token"
URL = "https://api.anthropic.com/v1/messages"


def main():
    model = sys.argv[1] if len(sys.argv) > 1 else "claude-haiku-5-5"
    token = TOKEN_FILE.read_text(encoding="utf-8").strip()
    body = json.dumps({
        "model": model,
        "max_tokens": 1,
        # Subscription OAuth tokens are only accepted with Claude Code's identity prompt.
        "system": "You are Claude Code, Anthropic's official CLI for Claude.",
        "messages": [{"role": "user", "content": "hi"}],
    }).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Authorization": f"Bearer {token}",
        "anthropic-version": "2023-06-01",
        "anthropic-beta": "oauth-2025-04-20",
        "Content-Type": "application/json",
        "User-Agent": "esp32-quota-test",
    })
    try:
        resp = urllib.request.urlopen(req, timeout=20)
        status, headers, err = resp.status, resp.headers, ""
    except urllib.error.HTTPError as e:  # headers can still carry usage on errors
        status, headers, err = e.code, e.headers, e.read().decode("utf-8", "replace")[:300]
    print(f"model {model}: HTTP {status}")
    if err:
        print("error body:", err)
    for k, v in sorted(headers.items()):
        if k.lower().startswith("anthropic-ratelimit"):
            print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
