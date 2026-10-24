"""Expose bounded test/build failures in check annotations as well as job logs."""
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
if not path.exists():
    raise SystemExit(0)
lines = path.read_text(errors="replace").splitlines()
markers = [i for i, line in enumerate(lines)
           if re.search(r"^(?:not ok \d+|✖ )|:\d+(?::\d+)?: (?:fatal )?error:", line)]
if not markers:
    markers = [i for i, line in enumerate(lines) if "error:" in line or "AssertionError" in line]
chunks = ["\n".join(lines[max(0, i - 2):i + 35])[:3200] for i in markers[:6]]
chunks.append("Final output:\n" + "\n".join(lines[-22:])[-2600:])
for summary in chunks:
    encoded = summary.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print("::error title=Test or build failure::" + encoded)
