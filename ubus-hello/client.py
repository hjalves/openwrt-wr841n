#!/usr/bin/env python3
"""Call the example through an SSH-forwarded uhttpd ubus endpoint."""

import getpass
import json
import sys
import urllib.error
import urllib.request


URL = "http://127.0.0.1:18080/ubus"


def call(params):
    request = urllib.request.Request(
        URL,
        data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "call", "params": params}).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        payload = json.load(response)
    if "error" in payload:
        raise RuntimeError("RPC error: " + str(payload["error"]))
    result = payload.get("result")
    if not isinstance(result, list) or len(result) < 2 or result[0] != 0:
        raise RuntimeError("ubus call failed or returned no response")
    return result[1]


def main():
    password = getpass.getpass("Router root password: ")
    login = call(["0" * 32, "session", "login", {
        "username": "root", "password": password, "timeout": 60,
    }])
    del password
    token = login.get("ubus_rpc_session")
    if not token:
        raise RuntimeError("login did not return a session")
    result = call([token, "example.hello", "say", {}])
    print(json.dumps(result))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, urllib.error.URLError) as exc:
        print("Error:", exc, file=sys.stderr)
        sys.exit(1)
