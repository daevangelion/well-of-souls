#!/usr/bin/env python3
"""tools/oracle/split_port_log.py -- the port's event log into per-label dumps.

    split_port_log.py <port.log> <outdir>

The port answers `at <ms> dump <label>` in its event log, not into a file:
`dump_one()` in src/game_main.c emits each key through `log_emit`, which writes
`EVT <key> <key>=<value>`, preceded by `EVT dump label=<name>`.  There is no
per-label file anywhere on the port side, so this is what turns the port's log
into something `cmp_dump.py` can be pointed at.

Output is `<outdir>/<label>.txt`, flat `key=value`, in the order the keys were
emitted -- the same shape the oracle writes.  A `dump_unknown` event means the
port did not know that label; it is reported and no file is written, because a
label the port cannot produce is a finding, not something to paper over.
"""
import os
import sys


def flush(label, buf, outdir):
    if label is None or not buf:
        return 0
    with open(os.path.join(outdir, "%s.txt" % label), "w", encoding="utf-8") as out:
        out.write("\n".join(buf) + "\n")
    return 1


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, outdir = sys.argv[1], sys.argv[2]
    try:
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError as e:
        sys.stderr.write("split_port_log: %s\n" % e)
        return 2

    os.makedirs(outdir, exist_ok=True)
    label, buf, written, unknown = None, [], 0, []
    for line in lines:
        if not line.startswith("EVT "):
            continue
        event, _, msg = line[4:].rstrip("\n").partition(" ")
        if event == "dump":
            written += flush(label, buf, outdir)
            label = msg.partition("label=")[2] or None
            buf = ["label=%s" % label] if label else []
        elif event == "dump_unknown":
            unknown.append(msg)
        elif label is not None and msg.startswith(event + "="):
            # ONLY log_emit lines: it is the one caller that writes the key as
            # the event name AND repeats it in the message.  A game event such
            # as "EVT world_tick ms=12 count=3" must not be swallowed as a dump
            # key, or the last label of a run inherits every event after it.
            buf.append(msg)

    written += flush(label, buf, outdir)

    for u in unknown:
        print("split_port_log: the port does not know the label %r" % u)
    print("split_port_log: %d label dump(s) -> %s" % (written, outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
