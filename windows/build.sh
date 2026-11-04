#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export TZ=UTC
printf 'UTC=%s cwd=%s\n' "$(date -u +%FT%TZ)" "$PWD"
firmware=${1:-build/windows-x64/firmware.bin}
if [[ ! -f "$firmware" ]]; then
    printf 'Firmware input missing: %s\nUsage: bash windows/build.sh /path/to/firmware.bin\n' "$firmware" >&2
    exit 1
fi
cc=${CC_WINDOWS:-x86_64-w64-mingw32-gcc}
"$cc" --version
mkdir -p build/windows-x64
clang_msvc=${CLANG_MSVC:-/usr/bin/clang++}
"$clang_msvc" --target=x86_64-pc-windows-msvc -std=c++17 -O2 \
    -fno-exceptions -fno-stack-protector -c windows/tests/msvc_consumer.cpp \
    -o build/windows-x64/msvc_consumer.obj
cmake -S windows -B build/windows-x64 -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows-x64 --parallel 2
"${HOST_CC:-/usr/bin/cc}" -std=c11 -Wall -Wextra -Werror -Iinclude \
    windows/tests/test_xz_wire.c usb/window.c -o build/windows-x64/test-xz-wire
build/windows-x64/test-xz-wire
"${HOST_CC:-/usr/bin/cc}" -std=c11 -Wall -Wextra -Werror -Iwindows/winusb \
    windows/tests/test_ipc_protocol.c -o build/windows-x64/test-ipc-protocol
build/windows-x64/test-ipc-protocol
"${HOST_CC:-/usr/bin/cc}" -std=c11 -Wall -Wextra -Werror -Iinclude \
    windows/tests/validate_bundle.c core/loader.c core/recovery.c \
    core/session.c core/device_query.c core/resident_image.c core/protocol.c \
    -o build/windows-x64/validate-bundle
python3 windows/verify.py build/windows-x64
python3 windows/package.py --firmware "$firmware"
