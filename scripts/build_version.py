Import("env")
from pathlib import Path
import sys
root = Path(env.subst("$PROJECT_DIR"))
sys.path.insert(0, str(root / "scripts"))
from version_info import build_identity
revision, version = build_identity(root)
if revision == "unknown":
    print("Git metadata unavailable; building " + version)
# Generate a header instead of shell-escaped compiler definitions.
out = Path(env.subst("$BUILD_DIR")) / "generated"
out.mkdir(parents=True, exist_ok=True)
import json
content = "#pragma once\n#define GIT_REV " + json.dumps(revision) + "\n#define L8_FW_VERSION " + json.dumps(version) + "\n"
p = out / "l8_version.h"
if not p.exists() or p.read_text() != content:
    p.write_text(content)
env.Append(CPPPATH=[str(out)])
