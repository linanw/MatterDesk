"""Expose bounded test/build failures in check annotations as well as job logs."""
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
if not path.exists():
    raise SystemExit(0)
lines = path.read_text(errors="replace").splitlines()
errors = [line for line in lines if "error:" in line or "failed" in line.lower() or "AssertionError" in line]
summary = "\n".join(errors[:45] + ["--- final output ---"] + lines[-65:])[-18000:]
encoded = summary.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
print("::error title=Test or build failure::" + encoded)
