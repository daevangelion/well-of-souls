#!/usr/bin/env python3
"""Decompile every labels.csv function of Souls.exe with radare2 + r2ghidra (pdg).

No Ghidra install needed. Writes work/decomp/all.c (one "// ==== <addr> <name> ====" block
per function, the same header DumpDecomp.java used) and work/decomp/functions.tsv.

    tools/r2/decomp.py                      # every function, ~minutes
    tools/r2/decomp.py 0x4223d8 HotspotDraw # just these, to stdout

Needs r2 on PATH with the r2ghidra plugin (`r2 -qc Lc -- | grep r2ghidra`).
"""
import csv, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(ROOT, "extracted", "Souls.exe")


def load_labels():
    rows = []
    with open(os.path.join(ROOT, "labels.csv"), newline="") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["addr"], 16), r["name"]))
    return rows


def flag_name(name, addr):
    # r2 flag names: no spaces, quotes or backticks. Keep the label recognisable.
    s = re.sub(r"[^A-Za-z0-9_.]", "_", name).strip("_")
    return s or "fcn_%08x" % addr


def run_r2(script_lines):
    with tempfile.NamedTemporaryFile("w", suffix=".r2", delete=False) as t:
        t.write("\n".join(script_lines) + "\n")
        path = t.name
    try:
        out = subprocess.run(
            ["r2", "-q", "-e", "scr.color=0", "-e", "scr.interactive=false",
             "-e", "bin.relocs.apply=true", "-i", path, EXE],
            capture_output=True, text=True, errors="replace")
    finally:
        os.unlink(path)
    return out.stdout


def main(argv):
    labels = load_labels()
    by_addr = {a: n for a, n in labels}
    by_name = {n: a for a, n in labels}
    targets = []
    for a in argv:
        if a in by_name:
            targets.append(by_name[a])
        else:
            targets.append(int(a, 16))
    dump_all = not targets
    if dump_all:
        targets = [a for a, _ in labels]

    # Define every labelled function first so pdg prints callee names, then decompile.
    script = ["aa"]
    for a, n in labels:
        script.append("af %s 0x%x" % (flag_name(n, a), a))
    for a in targets:
        name = by_addr.get(a, "fcn_%08x" % a)
        if a not in by_addr:
            script.append("af %s 0x%x" % (name, a))
        # A whole command in double quotes is taken literally: label names carry `, ' and <>.
        script.append('"?e // ==== %08X %s ===="' % (a, name.replace('"', "'")))
        script.append("s 0x%x" % a)
        script.append("pdg")  # `pdg @ addr` prints nothing; seek first
    out = run_r2(script)

    if not dump_all:
        sys.stdout.write(out)
        return 0
    outdir = os.path.join(ROOT, "work", "decomp")
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "all.c"), "w") as f:
        f.write(out)
    with open(os.path.join(outdir, "functions.tsv"), "w") as f:
        for a, n in labels:
            f.write("%08x\t%s\n" % (a, n))
    print("wrote %s (%d functions)" % (os.path.join(outdir, "all.c"), len(targets)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
