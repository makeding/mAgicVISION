"""Validate retained Windows deliverables, imports, exports and source hashes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parent.parent
build = Path(sys.argv[1]).resolve()
objdump = os.environ.get("OBJDUMP_WINDOWS", "x86_64-w64-mingw32-objdump")
manifest = {"target": "Windows x64", "device": "04bb:0530", "sources": {}, "outputs": {}}
for name in ("BonDriver_mb86H.dll", "xz-capture.exe", "bon-abi-probe.exe", "DriverHost_mb86H.exe", "host-ipc-probe.exe"):
    path = build / name
    report = subprocess.check_output([objdump, "-p", str(path)], text=True)
    assert "pei-x86-64" in report, report
    imports = [line.split(":", 1)[1].strip() for line in report.splitlines() if "DLL Name:" in line]
    forbidden = ("libgcc", "libstdc++", "libwinpthread", "libusb", "libmcfgthread")
    assert not any(d.lower().startswith(forbidden) for d in imports), imports
    if name.endswith(".dll"):
        assert "CreateBonDriver" in report
    if name in ("BonDriver_mb86H.dll", "xz-capture.exe", "host-ipc-probe.exe"):
        assert not any(d.lower() in ("winusb.dll", "user32.dll") for d in imports), imports
    if name == "DriverHost_mb86H.exe":
        assert any(d.lower() == "winusb.dll" for d in imports), imports
    (build / (name + ".pe.txt")).write_text(report)
    manifest["outputs"][name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "imports": imports}
    print(name, path.stat().st_size, "bytes; imports:", ", ".join(imports))
for directory in ("windows/winusb", "windows/tests", "core", "usb", "include", "device/gv_mvp_xz"):
    for path in sorted((root / directory).rglob("*")):
        if path.suffix in (".c", ".h", ".cpp"):
            manifest["sources"][str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
for name in ("windows/CMakeLists.txt", "windows/build.sh", "windows/shell.nix"):
    manifest["sources"][name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
(build / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
assert sum(p.stat().st_size for p in build.rglob("*") if p.is_file()) < 64*1024*1024
print("PE/export/import checks passed; manifest:", build / "manifest.json")
