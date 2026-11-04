#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SOURCE="$ROOT/build/pcsc/ifd-mb86H.bundle"
TARGET=/usr/local/libexec/SmartCardServices/drivers/ifd-mb86H.bundle
STATE='/Library/Application Support/mb86H/pcsc'
test "$(uname -s)" = Darwin
test -f "$SOURCE/Contents/Info.plist"
/usr/bin/plutil -lint "$SOURCE/Contents/Info.plist"
/usr/bin/codesign --verify --strict "$SOURCE/Contents/MacOS/ifd-mb86H"
/usr/bin/codesign --verify --strict "$SOURCE/Contents/MacOS/libusb-1.0.0.dylib"
if [ -e "$TARGET" ]; then
    ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TARGET/Contents/Info.plist")
    test "$ID" = org.mb86H.ifd || { echo 'Refusing to replace an unrelated bundle' >&2; exit 1; }
fi
sudo /bin/mkdir -p /usr/local/libexec/SmartCardServices/drivers "$STATE"
sudo /bin/chmod 1777 "$STATE"
sudo /usr/bin/ditto "$SOURCE" "$TARGET"
sudo /usr/sbin/chown -R root:wheel "$TARGET"
sudo /bin/chmod -R a+rX,go-w "$TARGET"
/usr/bin/codesign --verify --strict "$TARGET/Contents/MacOS/ifd-mb86H"
printf '%s\n' 'Close PC/SC clients and disconnect XZ3 before restarting the cached IFD broker.'
printf '%s\n' 'Then run: sudo /usr/bin/killall -KILL com.apple.ifdreader'
printf '%s\n' "Installed: $TARGET" 'Disconnect XZ3 USB/power, reconnect, then run pcsc_scan.'
