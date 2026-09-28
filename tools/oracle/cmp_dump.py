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
    """zero the masked byte ranges of the hero record on both sides.

    The record has two spellings: the oracle writes `oracle.hero_hex.XXXX` (64
    bytes a line, for a human) and `hero.record` (the whole 0x16CC as one hex
    key, which is what src/game/hero.c emits and the only one that is compared).
    Both are masked the same way."""
    if not ranges:
        return dump
    pat = re.compile(r"^oracle\.hero_hex\.([0-9A-Fa-f]{4})=(.*)$")

    def fix(m):
        off = int(m.group(1), 16)
        data = bytearray.fromhex(m.group(2))
        for a, b in ranges:
            for i in range(max(a, off), min(b + 1, off + len(data))):
                data[i - off] = 0
        return "oracle.hero_hex.%04X=%s" % (off, data.hex().upper())

    out = {}
    for k, v in dump.items():
        if k.startswith("oracle.hero_hex."):
            out[k] = pat.sub(fix, v)
        elif k == "hero.record":
            data = bytearray.fromhex(v)
            for a, b in ranges:
                for i in range(max(a, 0), min(b + 1, len(data))):
                    data[i] = 0
            out[k] = data.hex()
        else:
            out[k] = v
    return out


# --- key ownership -----------------------------------------------------------
# A key that only one side has is not automatically a bug: the oracle carries
# `oracle.*` diagnostics the port has no concept of, and the port carries keys
# for modules the hook has no read-out for yet.  Both classes are reported and
# both are named here, so a report says WHICH key is missing, never just that
# something differs.
ORACLE_ONLY_PREFIX = "oracle."
#: keys the port emits for modules the hook cannot read out of the original yet
#: A key in one of these namespaces that only ONE side has is not a defect: the hook
#: has no source for it, or the original has no counterpart at all.  Both are named in
#: docs/re/oracle.md section 5.4.4 with the reason, so this list is the short version of
#: a table, not a place to hide a missing read-out.
#:
#: `front.` is here because the original's front-end state is a 100-slot hotspot table
#: (DAT_005339F8) that src/game_main.c's dump table has no module for.  The hook reads
#: and emits it (see dump_hotspots) so the values are on the record, but there is no
#: `front_dump` on the port side to compare them against yet.
NO_SOURCE_PREFIXES = ("map.", "scene.", "battle.", "panels.", "items.",
                      "minigame.", "options.", "world.", "chat.", "editors.",
                      "missions.", "html.", "front.")


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
    bad, nosrc, extra = [], [], 0
    for k in order:
        if k.startswith(ORACLE_ONLY_PREFIX):
            extra += 1
        elif k not in oracle:
            if k.startswith(NO_SOURCE_PREFIXES):
                nosrc.append(k)
            else:
                bad.append("  %-22s only in port: %s" % (k, port[k][:60]))
        elif k not in port:
            if k.startswith(NO_SOURCE_PREFIXES):
                nosrc.append(k)
            else:
                bad.append("  %-22s only in oracle: %s" % (k, oracle[k][:60]))
        elif oracle[k] != port[k]:
            bad.append("  %-22s oracle=%s port=%s" % (k, oracle[k][:60], port[k][:60]))
    label = oracle.get("label", sys.argv[1])
    tail = ""
    if extra:
        tail += "  [%d oracle-only key(s) not compared]" % extra
    if nosrc:
        tail += "  [%d key(s) with no oracle source yet]" % len(nosrc)
    if not bad:
        print("%s: OK (%d compared%s)"
              % (label, len(order) - extra - len(nosrc), tail))
        if nosrc:
            print("    no oracle source for: %s" % ", ".join(sorted(set(nosrc))[:12]))
        return 0
    print("%s: MISMATCH (%d of %d keys%s)"
          % (label, len(bad), len(order) - extra, tail))
    for line in bad[:12]:
        print(line)
    if len(bad) > 12:
        print("  ... and %d more" % (len(bad) - 12))
    return 1


if __name__ == "__main__":
    sys.exit(main())
