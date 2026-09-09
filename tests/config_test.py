"""Quick configuration checks; reads only public CI placeholders, never local secrets."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
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
            assert "  enable_on_boot: false\n" in source
            assert "    continuous: true\n" in source
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
print("Production/discovery settings and invalid-capacity schema checks passed")
