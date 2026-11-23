"""Let IDLE run while Stream waits for data in pinned Arduino 2.0.17.

Stream::timedRead/timedPeek busy-wait up to the stream timeout. The L8 loop runs on
core 0 (ARDUINO_RUNNING_CORE=0), so WebServer::_parseRequest -> readStringUntil on a
partial HTTP request starved IDLE0 and the task watchdog reset the board (E1 run,
2026-11-23). One tick of delay per empty poll keeps the same timeout semantics."""
Import("env")
from pathlib import Path

framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
path = framework / "cores/esp32/Stream.cpp"
source = path.read_text()
MARK = "delay(1); // L8: let IDLE run (task WDT) while waiting"
blocks = [
    ("        c = read();\n        if(c >= 0) {\n            return c;\n        }\n    } while(millis() - _startMillis < _timeout);",
     "        c = read();\n        if(c >= 0) {\n            return c;\n        }\n        " + MARK + "\n    } while(millis() - _startMillis < _timeout);"),
    ("        c = peek();\n        if(c >= 0) {\n            return c;\n        }\n    } while(millis() - _startMillis < _timeout);",
     "        c = peek();\n        if(c >= 0) {\n            return c;\n        }\n        " + MARK + "\n    } while(millis() - _startMillis < _timeout);"),
]
if source.count(MARK) != 2:
    if MARK in source or any(source.count(old) != 1 for old, _ in blocks):
        raise RuntimeError("Stream yield patch no longer matches; review the new core")
    for old, new in blocks:
        source = source.replace(old, new)
    path.write_text(source)
