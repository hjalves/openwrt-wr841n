#!/usr/bin/env python3
"""Query huawei-trafficd's history over the OpenWrt router's /ubus HTTP bridge.

Examples:
  ./query.py                         # latest sample
  ./query.py --from 1h               # last hour
  ./query.py --from 2026-11-15T08:00 --to 2026-11-15T12:00 --format csv
  ./query.py --from 1d --usage       # bytes transferred over the last day
  ./query.py --from 30d --tier coarse --format csv   # hourly samples, 30 days

History has two tiers: "fine" (every sample, last few days) and "coarse"
(hourly, up to a year). The default "auto" returns coarse samples older than
the fine tier, followed by the fine samples.
"""

import argparse
import datetime as dt
import json
import re
import sys
import urllib.request

ANON = "0" * 32


def rpc(url, method, args=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": "call",
            "params": [ANON, "huawei.traffic", method, args or {}]}
    req = urllib.request.Request(url, json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        reply = json.load(resp)
    if "error" in reply:
        raise RuntimeError(reply["error"].get("message", reply["error"]))
    status, *data = reply["result"]
    if status != 0:
        raise RuntimeError(f"ubus status {status}" + (" (no data yet)" if status == 9 else ""))
    return data[0] if data else {}


def parse_time(text):
    """'90m', '6h', '2d', '1w' (ago) or an ISO date/time (local time zone)."""
    m = re.fullmatch(r"(\d+)([smhdw])", text)
    if m:
        mult = {"s": 1, "m": 60, "h": 3600, "d": 86400, "w": 604800}[m[2]]
        return int(dt.datetime.now().timestamp()) - int(m[1]) * mult
    return int(dt.datetime.fromisoformat(text).timestamp())


def fetch(url, start, end, tier):
    samples, args = [], {"from": start, "limit": 4000, "tier": tier}
    if end is not None:
        args["to"] = end
    while True:
        page = rpc(url, "query", args)
        samples += page["samples"]
        if not page["truncated"] or not page["samples"]:
            return samples
        args["from"] = page["samples"][-1]["time"] + 1


def iso(ts):
    return dt.datetime.fromtimestamp(ts).isoformat(sep=" ", timespec="seconds")


def usage(samples):
    """Bytes transferred between consecutive samples, summed.

    Uses the Huawei's lifetime totals, which keep counting across LTE
    reconnects. If a total ever goes backwards (counters cleared), falls back
    to the session counter, treating a drop as a new session.
    """
    def total(key):
        acc = 0
        for a, b in zip(samples, samples[1:]):
            if b["total_" + key] >= a["total_" + key] > 0:
                acc += b["total_" + key] - a["total_" + key]
            else:
                acc += b[key] - a[key] if b[key] >= a[key] else b[key]
        return acc
    return total("download"), total("upload")


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--router", default="192.168.0.254", help="OpenWrt address")
    ap.add_argument("--from", dest="start", help="start: 1h/2d/... ago or ISO time")
    ap.add_argument("--to", dest="end", help="end: same formats (default: now)")
    ap.add_argument("--tier", choices=("auto", "fine", "coarse"), default="auto",
                    help="history tier (default: auto)")
    ap.add_argument("--format", choices=("table", "csv", "json"), default="table")
    ap.add_argument("--usage", action="store_true", help="print totals for the range only")
    ap.add_argument("--status", action="store_true", help="show daemon status")
    a = ap.parse_args()
    url = f"http://{a.router}/ubus"

    if a.status:
        print(json.dumps(rpc(url, "status"), indent=2))
        return
    if a.start is None and a.end is None:
        samples = [rpc(url, "latest")["sample"]]
    else:
        samples = fetch(url, parse_time(a.start) if a.start else 0,
                        parse_time(a.end) if a.end else None, a.tier)

    if a.usage:
        if len(samples) < 2:
            sys.exit("need at least two samples in the range")
        d, u = usage(samples)
        print(f"{iso(samples[0]['time'])} .. {iso(samples[-1]['time'])}: "
              f"download {human(d)} ({d} B), upload {human(u)} ({u} B)")
    elif a.format == "json":
        print(json.dumps(samples, indent=2))
    elif a.format == "csv":
        print("time,iso_time,uptime_s,download_b,upload_b,total_download_b,total_upload_b")
        for s in samples:
            print(f"{s['time']},{iso(s['time'])},{s['uptime']},{s['download']},"
                  f"{s['upload']},{s['total_download']},{s['total_upload']}")
    else:
        print(f"{'time':19}  {'session up':>12}  {'download':>12}  {'upload':>12}")
        for s in samples:
            print(f"{iso(s['time'])}  {str(dt.timedelta(seconds=s['uptime'])):>12}  "
                  f"{human(s['download']):>12}  {human(s['upload']):>12}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        sys.exit(f"error: {exc}")
