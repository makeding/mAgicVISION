"""Create matching ZIP and copyable Windows deliverables from one file list."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
import zipfile
import subprocess
root = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument("--firmware", type=Path, required=True)
a = p.parse_args()
fw = a.firmware.resolve()
assert fw.stat().st_size <= 1024*1024
assert fw.read_bytes()[:4] in (b"XZFB", b"SBC2")
build = root / "build/windows-x64"
subprocess.run([str(build / "validate-bundle"), str(fw)], check=True)
manifest = json.loads((build / "manifest.json").read_text())
entries = {name: build / name for name in ("BonDriver_mb86H.dll", "xz-capture.exe", "bon-abi-probe.exe", "DriverHost_mb86H.exe", "host-ipc-probe.exe")}
entries.update({"firmware.bin": fw, "BonDriver_mb86H.ini": root / "windows/BonDriver_mb86H.ini",
                "mb86H-xz-winusb.inf": root / "windows/mb86H-xz-winusb.inf"})
manifest["firmware_input"] = str(fw)
manifest["package_files"] = {}
for name, path in entries.items():
    data = path.read_bytes()
    manifest["package_files"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if name in manifest["outputs"]:
        assert manifest["outputs"][name]["sha256"] == manifest["package_files"][name]["sha256"]
archive = build / "mb86H-xz-winusb-x64.zip"
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
    for name, path in entries.items():
        z.write(path, name)
    z.writestr("manifest.json", json.dumps(manifest, indent=2)+"\n")
assert archive.stat().st_size < 8*1024*1024
copy_dir = build / "TVTest-copy"
# This exact directory is wholly generated. Rebuild it to remove stale payloads.
if copy_dir.exists():
    shutil.rmtree(copy_dir)
copy_dir.mkdir()
with zipfile.ZipFile(archive) as z:
    for name in (*entries, "manifest.json"):
        data = z.read(name)
        if name in entries:
            assert hashlib.sha256(data).hexdigest() == manifest["package_files"][name]["sha256"]
        (copy_dir / name).write_bytes(data)
assert sum(p.stat().st_size for p in build.rglob("*") if p.is_file()) < 64*1024*1024
print(archive, archive.stat().st_size, "bytes SHA256="+hashlib.sha256(archive.read_bytes()).hexdigest())
print("firmware:", fw, manifest["package_files"]["firmware.bin"])
print("Copy all files from:", copy_dir)
