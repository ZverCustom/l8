Import("env")
from pathlib import Path
import gzip
root=Path(env.subst("$PROJECT_DIR"))
out=Path(env.subst("$BUILD_DIR"))/"generated"
out.mkdir(parents=True,exist_ok=True)
data=gzip.compress((root/"src/web/index.html").read_bytes(),mtime=0)
header="#pragma once\n#include <Arduino.h>\nstatic const uint8_t L8_UI[] PROGMEM={"+",".join(map(str,data))+"};\nstatic constexpr size_t L8_UI_SIZE=sizeof(L8_UI);\n"
p=out/"l8_ui.h"
if not p.exists() or p.read_text()!=header: p.write_text(header)
env.Append(CPPPATH=[str(out)])
