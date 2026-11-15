#!/usr/bin/env python3
"""Build lanwatch's compact MAC vendor table from nmap's nmap-mac-prefixes
(derived from the IEEE OUI registry). Only 24-bit OUIs of the vendors below
are kept.

File format (all little-endian):
  "LWOUI1"  u16 vendor count  then vendor names, each u8 length + bytes
  u32 entry count  then entries sorted by OUI: varint(OUI - previous OUI), u8 vendor index
"""
import re, struct, sys

# display name -> regex matched against the registry's organisation name
VENDORS = [
    ("Apple", r"^Apple"), ("Samsung", r"Samsung"), ("Google", r"^Google"), ("Amazon", r"^Amazon"),
    ("Microsoft", r"^Microsoft"), ("Xiaomi", r"Xiaomi|Beijing Xiaomi"), ("Huawei", r"Huawei"),
    ("Honor", r"Honor Device"), ("OnePlus", r"OnePlus"), ("OPPO", r"\bOPPO\b|Guangdong Oppo"),
    ("vivo", r"\bvivo Mobile"), ("realme", r"realme"), ("Motorola", r"^Motorola Mobility"),
    ("Lenovo", r"^Lenovo"), ("LG", r"^LG Electronics|^LG Innotek"), ("Sony", r"^Sony"),
    ("Nintendo", r"^Nintendo"), ("Intel", r"^Intel Corporate|^Intel Corp"), ("Realtek", r"^Realtek"),
    ("MediaTek", r"^MediaTek|^Ralink"), ("Qualcomm", r"^Qualcomm"), ("Broadcom", r"^Broadcom"),
    ("Espressif", r"^Espressif"), ("TP-Link", r"TP-LINK|Tp-Link|TP-Link"), ("Netgear", r"^Netgear"),
    ("ASUS", r"^ASUSTek"), ("D-Link", r"^D-Link"), ("Ubiquiti", r"^Ubiquiti"),
    ("Raspberry Pi", r"^Raspberry Pi"), ("Texas Instruments", r"^Texas Instruments"),
    ("Murata", r"^Murata"), ("AzureWave", r"^AzureWave"), ("Lite-On", r"^Liteon|^Lite-On"),
    ("Foxconn", r"^Hon Hai|^Foxconn|Fugui Precision"), ("Dell", r"^Dell"), ("HP", r"^Hewlett Packard|^HP Inc"),
    ("Acer", r"^Acer"), ("Canon", r"^Canon"), ("Epson", r"^Seiko Epson"), ("Brother", r"^Brother Industries"),
    ("Philips", r"^Philips|^Signify"), ("Sonos", r"^Sonos"), ("Roku", r"^Roku"), ("Nvidia", r"^Nvidia"),
    ("Valve", r"^Valve"), ("Garmin", r"^Garmin"), ("Fitbit", r"^Fitbit"), ("Ring", r"^Ring$"),
    ("Arlo", r"^Arlo"), ("Tuya", r"Tuya"), ("Bosch", r"^Robert Bosch|^BSH"),
    ("Panasonic", r"^Panasonic"), ("Sharp", r"^Sharp"), ("Hisense", r"Hisense"), ("TCL", r"^TCL"),
    ("Synology", r"^Synology"), ("QNAP", r"^QNAP"), ("Western Digital", r"^Western Digital"),
    ("Arcadyan", r"^Arcadyan"), ("Sagemcom", r"^Sagemcom"), ("ZTE", r"^zte|^ZTE"),
    ("Sercomm", r"^Sercomm"), ("Technicolor", r"^Technicolor"), ("AVM", r"^AVM "),
    ("Zyxel", r"^Zyxel|^ZyXEL"), ("Belkin", r"^Belkin"), ("eero", r"^eero"), ("Wistron", r"^Wistron"),
    ("Quanta", r"^Quanta"), ("Compal", r"^Compal"), ("USI", r"^Universal Global Scientific"),
    ("Gemtek", r"^Gemtek"), ("Askey", r"^Askey"), ("Chromecast/Nest", r"^Nest Labs"),
]

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n: return bytes(out)

def main(src, dst):
    pats = [(i, re.compile(rx, re.I)) for i, (_, rx) in enumerate(VENDORS)]
    entries = {}
    for line in open(src, encoding="utf-8", errors="replace"):
        if line.startswith("#"): continue
        parts = line.rstrip("\n").split(" ", 1)
        if len(parts) != 2 or len(parts[0]) != 6: continue
        for i, p in pats:
            if p.search(parts[1]):
                entries[int(parts[0], 16)] = i
                break
    out = bytearray(b"LWOUI1") + struct.pack("<H", len(VENDORS))
    for name, _ in VENDORS:
        b = name.encode(); out += bytes([len(b)]) + b
    out += struct.pack("<I", len(entries))
    prev = 0
    for oui in sorted(entries):
        out += varint(oui - prev) + bytes([entries[oui]]); prev = oui
    open(dst, "wb").write(out)
    counts = {}
    for v in entries.values(): counts[VENDORS[v][0]] = counts.get(VENDORS[v][0], 0) + 1
    print(f"{len(entries)} OUIs from {len(VENDORS)} vendors -> {len(out)} bytes", file=sys.stderr)
    print("unmatched vendors:", [n for n, _ in VENDORS if n not in counts], file=sys.stderr)

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/nmap/nmap-mac-prefixes",
         sys.argv[2] if len(sys.argv) > 2 else "oui.bin")
