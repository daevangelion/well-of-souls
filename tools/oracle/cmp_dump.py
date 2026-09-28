#!/usr/bin/env python3
"""tools/oracle/cmp_dump.py -- compare one oracle dump against one port dump.

    cmp_dump.py <oracle.txt> <port.txt> [hero_mask.txt]

Prints `label: OK` or `label: MISMATCH` plus the first differing keys, and exits 0 when
the two dumps agree, 1 when they do not, 2 when a file is missing or unreadable.

Both sides write flat `key=value` lines.  Keys present on only one side are reported as
missing, because a silently dropped key is the failure mode that hides a real regression.
`hero.hex.*` keys are the raw 0x16CC-byte hero record, 64 bytes per line; the byte
ranges listed in the mask file are zeroed on BOTH sides before comparison, so a live RAM
pointer cannot swamp the diff.  Every named `hero.*` key is compared unmasked.
"""
import re
import sys


def load(path):
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            out[k] = v
    return out


def load_mask(path):
    """-> list of (first, last) inclusive byte ranges"""
    ranges = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            ranges.append((int(parts[0], 16), int(parts[1], 16)))
    return ranges


def apply_mask(dump, ranges):
    """zero the masked bytes of every hero.hex.* line on both sides"""
    if not ranges:
        return dump
    pat = re.compile(r"^hero\.hex\.([0-9A-Fa-f]{4})=(.*)$")

    def fix(m):
        off = int(m.group(1), 16)
        data = bytearray.fromhex(m.group(2))
        for a, b in ranges:
            for i in range(max(a, off), min(b + 1, off + len(data))):
                data[i - off] = 0
        return "hero.hex.%04X=%s" % (off, data.hex().upper())

    return {k: (pat.sub(fix, v) if k.startswith("hero.hex.") else v) for k, v in dump.items()}


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    try:
        oracle = load(sys.argv[1])
        port = load(sys.argv[2])
    except OSError as e:
        sys.stderr.write("cmp_dump: %s\n" % e)
        return 2
    mask = sys.argv[3] if len(sys.argv) > 3 else None
    if mask:
        try:
            ranges = load_mask(mask)
        except OSError as e:
            sys.stderr.write("cmp_dump: %s\n" % e)
            return 2
        oracle = apply_mask(oracle, ranges)
        port = apply_mask(port, ranges)

    # rng.* first: if the LCG diverged, every other key in the dump is meaningless.
    order = [k for k in oracle if k.startswith("rng.")] + \
            [k for k in oracle if not k.startswith("rng.")] + \
            [k for k in port if k not in oracle]
    bad = []
    for k in order:
        if k not in oracle:
            bad.append("  %-22s only in port: %s" % (k, port[k][:60]))
        elif k not in port:
            bad.append("  %-22s only in oracle: %s" % (k, oracle[k][:60]))
        elif oracle[k] != port[k]:
            bad.append("  %-22s oracle=%s port=%s" % (k, oracle[k][:60], port[k][:60]))
    label = oracle.get("label", sys.argv[1])
    if not bad:
        print("%s: OK (%d keys)" % (label, len(oracle)))
        return 0
    print("%s: MISMATCH (%d of %d keys)" % (label, len(bad), len(order)))
    for line in bad[:12]:
        print(line)
    if len(bad) > 12:
        print("  ... and %d more" % (len(bad) - 12))
    return 1


if __name__ == "__main__":
    sys.exit(main())
