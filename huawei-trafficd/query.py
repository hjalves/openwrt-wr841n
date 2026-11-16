#!/usr/bin/env python3
"""Query huawei-trafficd's history over the OpenWrt router's /ubus HTTP bridge.

Examples:
  ./query.py                         # latest sample
  ./query.py --from 1h               # last hour
  ./query.py --from 2026-11-15T08:00 --to 2026-11-15T12:00 --format csv
  ./query.py --from 1d --usage       # bytes transferred over the last day
  ./query.py --from 30d --tier coarse --format csv   # hourly samples, 30 days
  ./query.py --from 7d --events      # connection log: new sessions, gaps, changes

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


def same_session(a, b):
    """The Huawei's session uptime advanced by the elapsed time."""
    return b["uptime"] >= a["uptime"] and \
        (b["time"] - a["time"]) - (b["uptime"] - a["uptime"]) < 90


NET = {19: "LTE", 101: "LTE", 0: "no service"}
CONN = {900: "connecting", 901: "connected", 902: "disconnected", 903: "disconnecting"}


def net_name(t):
    if t in NET:
        return NET[t]
    return "2G" if 1 <= t <= 3 else "3G" if 4 <= t <= 18 or 41 <= t <= 46 else f"type {t}"


def cell_name(c):
    return f"{c // 256}/{c % 256}"  # eNodeB / sector


def events(samples, spacing, cells=True):
    """Connection log derived from consecutive samples (as the dashboard does).

    spacing(t): expected seconds between samples at time t; longer is a gap.
    """
    ev = []
    for a, b in zip(samples, samples[1:]):
        dt_ = b["time"] - a["time"]
        if dt_ <= 0:
            continue
        gap = dt_ > 3 * spacing(a["time"])
        if not same_session(a, b):
            ev.append((b["time"] - b["uptime"], "New LTE session",
                       (f"no samples for {dt.timedelta(seconds=dt_)} before it; " if gap else "") +
                       f"previous session lasted at least {dt.timedelta(seconds=a['uptime'])}"))
        elif gap:
            ev.append((a["time"], "Collection gap", f"no samples for {dt.timedelta(seconds=dt_)}; session continued"))
        ca, cb = a.get("conn_status"), b.get("conn_status")
        if ca and cb and ca != cb:
            ev.append((b["time"], "Connected" if cb == 901 else "Not connected",
                       f"status {CONN.get(ca, ca)} -> {CONN.get(cb, cb)}"))
        na, nb = a.get("net_type"), b.get("net_type")
        if na is not None and nb is not None and na != nb:
            ev.append((b["time"], "Network type", f"{net_name(na)} -> {net_name(nb)}"))
        xa, xb = a.get("cell_id"), b.get("cell_id")
        pla = a.get("gw_sent") and a["gw_lost"] >= a["gw_sent"]   # Huawei unreachable over the powerline
        plb = b.get("gw_sent") and b["gw_lost"] >= b["gw_sent"]
        neta = a.get("ping_sent") and not a.get("ping_ok") and not pla
        netb = b.get("ping_sent") and not b.get("ping_ok") and not plb
        if plb and not pla:
            ev.append((a["time"], "Powerline down", "no replies from the Huawei itself"))
        if pla and b.get("gw_sent") and not plb:
            ev.append((b["time"], "Powerline back", "the Huawei answers again"))
        if netb and not neta:
            ev.append((a["time"], "Internet down", "no replies from any latency target (Huawei still answering)"))
        if neta and b.get("ping_sent") and b.get("ping_ok"):
            ev.append((b["time"], "Internet back", "latency targets answering again"))
        if cells and xa and xb and xa != xb:
            ev.append((b["time"], "Cell change", f"{cell_name(xa)} -> {cell_name(xb)} (eNodeB/sector)"))
    return sorted(ev)


def usage(samples):
    """Bytes transferred between consecutive samples, summed.

    Within one LTE session (uptime advanced by the elapsed time) the session
    counter is exact. Across a new session, the Huawei's totals cover a clean
    reconnect, but a Huawei crash/reboot rolls them back to the last saved
    value, so the new session's counter is the lower bound; take the larger.
    """
    def step(a, b, key):
        if same_session(a, b) and b[key] >= a[key]:
            return b[key] - a[key]
        tot = b["total_" + key] - a["total_" + key]
        return max(b[key], tot if a["total_" + key] > 0 and tot > 0 else 0)

    def total(key):
        return sum(step(a, b, key) for a, b in zip(samples, samples[1:]))
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
    ap.add_argument("--events", action="store_true",
                    help="print the connection log for the range (default range: 7 days)")
    ap.add_argument("--no-cells", action="store_true", help="with --events: omit cell changes")
    ap.add_argument("--status", action="store_true", help="show daemon status")
    a = ap.parse_args()
    url = f"http://{a.router}/ubus"

    if a.status:
        print(json.dumps(rpc(url, "status"), indent=2))
        return
    if a.events:
        st = rpc(url, "status")
        fine_oldest = st["fine"].get("oldest", 2**62)
        coarse_iv = st.get("coarse", {}).get("interval", 3600)
        samples = fetch(url, parse_time(a.start or "7d"), parse_time(a.end) if a.end else None, a.tier)
        spacing = (lambda t: coarse_iv) if a.tier == "coarse" else \
            (lambda t: coarse_iv if t < fine_oldest else st["fine"]["interval"])
        ev = events(samples, spacing, cells=not a.no_cells)
        for t, what, detail in ev:
            print(f"{iso(t)}  {what:16}  {detail}")
        if not ev:
            print("no reconnects, gaps or changes in this range")
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
        extra = ("rsrp", "rsrq", "sinr", "rssi", "signal_samples", "cell_id", "conn_status", "net_type",
                 "ping_sent", "ping_lost", "ping_ok", "rtt_min", "rtt_avg", "rtt_max",
                 "gw_sent", "gw_lost", "gw_rtt_avg", "gw_rtt_max")
        print("time,iso_time,uptime_s,download_b,upload_b,total_download_b,total_upload_b,"
              "rsrp_dbm,rsrq_db,sinr_db,rssi_dbm,signal_samples,cell_id,conn_status,net_type,"
              "ping_sent,ping_lost,ping_ok_mask,rtt_min_ms,rtt_avg_ms,rtt_max_ms,"
              "gw_sent,gw_lost,gw_rtt_avg_ms,gw_rtt_max_ms")
        for s in samples:
            print(f"{s['time']},{iso(s['time'])},{s['uptime']},{s['download']},"
                  f"{s['upload']},{s['total_download']},{s['total_upload']}," +
                  ",".join(str(s.get(k, "")) for k in extra))
    else:
        print(f"{'time':19}  {'session up':>16}  {'download':>10}  {'upload':>10}  {'SINR':>6}  {'RSRP':>8}  {'latency':>8}  {'loss':>5}  {'powerline':>9}  {'loss':>5}  cell")
        for s in samples:
            sig = (f"{s['sinr']:>3} dB  {s['rsrp']:>4} dBm" if "sinr" in s else f"{'-':>6}  {'-':>8}")
            lat = f"{s['rtt_avg']:>5} ms" if "rtt_avg" in s else f"{'-':>8}"
            loss = f"{100 * s['ping_lost'] // s['ping_sent']:>4}%" if s.get("ping_sent") else f"{'-':>5}"
            pl = f"{s['gw_rtt_avg']:>6} ms" if "gw_rtt_avg" in s else f"{'-':>9}"
            pll = f"{100 * s['gw_lost'] // s['gw_sent']:>4}%" if s.get("gw_sent") else f"{'-':>5}"
            print(f"{iso(s['time'])}  {str(dt.timedelta(seconds=s['uptime'])):>16}  "
                  f"{human(s['download']):>10}  {human(s['upload']):>10}  {sig}  {lat}  {loss}  {pl}  {pll}  "
                  f"{cell_name(s['cell_id']) if s.get('cell_id') else '-'}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        sys.exit(f"error: {exc}")
