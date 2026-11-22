Import("env")
from pathlib import Path
import subprocess
root = Path(env.subst("$PROJECT_DIR"))
def git(*args):
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()
revision = git("describe", "--tags", "--always", "--dirty")
version = root.joinpath("VERSION").read_text().strip() + "+" + revision
# Generate a header instead of shell-escaped compiler definitions.
out = Path(env.subst("$BUILD_DIR")) / "generated"
out.mkdir(parents=True, exist_ok=True)
import json
content = "#pragma once\n#define GIT_REV " + json.dumps(revision) + "\n#define L8_FW_VERSION " + json.dumps(version) + "\n"
p = out / "l8_version.h"
if not p.exists() or p.read_text() != content:
    p.write_text(content)
env.Append(CPPPATH=[str(out)])
