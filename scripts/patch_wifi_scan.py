"""Initialize every scan field in pinned Arduino 2.0.17, including passive dwell."""
Import("env")
from pathlib import Path

framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
path = framework / "libraries/WiFi/src/WiFiScan.cpp"
source = path.read_text()
old = "wifi_scan_config_t config;"
new = "wifi_scan_config_t config = {}; // L8: initialize passive/home dwell too"
if new not in source:
    if source.count(old) != 1:
        raise RuntimeError("WiFi scan initialization patch no longer matches; review the new core")
    path.write_text(source.replace(old, new))
