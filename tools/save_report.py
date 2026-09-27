#!/usr/bin/env python3
"""Convert a subagent JSON result (summary/architecture/files/report) into a markdown doc."""
import json
import sys


def main() -> None:
    src, dst, title = sys.argv[1], sys.argv[2], sys.argv[3]
    raw = open(src, encoding="utf-8").read()
    try:
        d = json.loads(raw)
    except json.JSONDecodeError:
        open(dst, "w", encoding="utf-8").write(f"# {title}\n\n{raw}\n")
        return
    out = [f"# {title}\n"]
    if isinstance(d, dict):
        if d.get("summary"):
            out.append(f"## Summary\n\n{d['summary']}\n")
        if d.get("architecture"):
            out.append(f"## Architecture\n\n{d['architecture']}\n")
        if d.get("report"):
            out.append(d["report"] if isinstance(d["report"], str) else json.dumps(d["report"], indent=1))
        if d.get("files"):
            out.append("\n## Sources\n")
            for f in d["files"]:
                if isinstance(f, dict):
                    out.append(f"- `{f.get('path')}`: {f.get('description', '')}")
                else:
                    out.append(f"- {f}")
        for k, v in d.items():
            if k not in ("summary", "architecture", "report", "files"):
                out.append(f"\n## {k}\n\n{v if isinstance(v, str) else json.dumps(v, indent=1)}\n")
    else:
        out.append(json.dumps(d, indent=1))
    open(dst, "w", encoding="utf-8").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
