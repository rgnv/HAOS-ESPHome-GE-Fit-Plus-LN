"""Quick configuration checks; reads only public CI placeholders, never local secrets."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# Source contracts only: no firmware/host compilation or local secrets required.
recording = (ROOT / "components/ge_scale/recording.cpp").read_text()
scale = (ROOT / "components/ge_scale/ge_scale.cpp").read_text()
assert "WIFI_IDLE_GRACE_MS = 600000" in recording
assert "publish_(this->body_fat_sensor_, fat_pct);" in scale
assert "no impedance -> publish calculated estimates" in scale
request = recording.split("void GEScale::request_wifi_() {", 1)[1].split(
    "void GEScale::maybe_disable_wifi_()", 1)[0]
assert request.index("!wifi->is_ready()") < request.index("wifi->enable()")
assert "wifi->is_disabled() && this->wifi_retry_after_ms_ &&" in request
assert request.index("this->wifi_retry_after_ms_ = 0;") < request.index("wifi->enable()")
setup = recording.split("void GEScale::setup() {", 1)[1].split(
    "bool GEScale::save_recordings_", 1)[0]
assert "this->request_wifi_();" not in setup
replay = recording.split("void GEScale::replay_() {", 1)[1]
assert "this->request_wifi_();" in replay  # Restored outbox still wakes Wi-Fi.
print("Wi-Fi setup/backoff source contracts passed (not C++ execution)")

ESPHOME = shutil.which("esphome")
if ESPHOME is None:
    for candidate in (Path(sys.executable).with_name("esphome"), Path("/var/lib/hermes/.venv-esphome/bin/esphome")):
        if candidate.is_file():
            ESPHOME = str(candidate)
            break
if ESPHOME is None:
    print("ESPHome executable unavailable; configuration checks skipped")
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix="ge-scale-config-") as directory:
    target = Path(directory)
    shutil.copytree(ROOT / "components", target / "components")
    shutil.copyfile(ROOT / "secrets.ci.yaml", target / "secrets.yaml")
    for name in ("ge-fit-plus-ln.yaml", "ge-fit-plus-ln-discovery.yaml"):
        source = (ROOT / name).read_text()
        assert "  deassert_rts_dtr: true\n" in source
        assert "  power_save_mode: none\n" in source
        assert "  post_connect_roaming: false\n" in source
        if "discovery" in name:
            assert "  enable_on_boot: false\n" not in source
            assert source.count("  reboot_timeout: 5min\n") == 2
            assert "ge_scale:" not in source and "actions:" not in source
        else:
            assert "  - mac_address: !secret ge_scale_mac\n" in source
            assert "    auto_connect: true\n" in source
            assert "  enable_on_boot: false\n" in source
            assert "    continuous: true\n" in source
            assert "    active: false\n" in source  # ADV must not wait for SCAN_RSP.
            assert "    - mac_address: !secret ge_scale_mac\n" in source
            assert "x.get_address_type()" in source
            assert source.count("  reboot_timeout: 0s\n") == 2
            assert "  batch_delay: 0ms\n" in source
        config = target / name
        config.write_text(source)
        result = subprocess.run([ESPHOME, "config", str(config)],
                                capture_output=True, timeout=30)
        assert result.returncode == 0, f"{name} validation failed (output withheld)"
    source = (ROOT / "ge-fit-plus-ln.yaml").read_text()
    config = target / "ge-fit-plus-ln.yaml"
    for capacity in (0, 33):
        config.write_text(source.replace("recording_capacity: 16", f"recording_capacity: {capacity}"))
        result = subprocess.run([ESPHOME, "config", str(config)],
                                capture_output=True, timeout=30)
        assert result.returncode != 0 and b"recording_capacity" in result.stdout + result.stderr
print("Production/discovery settings, BLE target binding, and invalid-capacity schema checks passed")
