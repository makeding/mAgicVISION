#!/usr/bin/env python3
"""Maintain this package's virtual CCID identity in an existing libccid plist."""
import argparse
import json
import os
import plistlib
from pathlib import Path

KEYS = ("ifdVendorID", "ifdProductID", "ifdFriendlyName")
READER_NAME = "mb86H Series Card Reader"
TARGET_VENDOR = 0x1209
TARGET_PRODUCT = 0x0001
STATE_VERSION = 1
MAX_PLIST_SIZE = 8 * 1024 * 1024


def identity_arrays(document):
    arrays = [document[k] for k in KEYS]
    if any(not isinstance(array, list) for array in arrays) or \
            len({len(array) for array in arrays}) != 1:
        raise ValueError("libccid identity arrays must be parallel lists")
    return arrays


def target_indices(document):
    vendor, product, _ = identity_arrays(document)
    try:
        return [index for index, (value, pid) in enumerate(zip(vendor, product))
                if int(str(value), 0) == TARGET_VENDOR and
                int(str(pid), 0) == TARGET_PRODUCT]
    except ValueError as error:
        raise ValueError("libccid identity values must be integer strings") from error


def add_identity(document):
    arrays = identity_arrays(document)
    matches = target_indices(document)
    if matches:
        changed = False
        for index in matches:
            if arrays[2][index] != READER_NAME:
                arrays[2][index] = READER_NAME
                changed = True
        return changed
    for values, item in zip(arrays, ("0x1209", "0x0001", READER_NAME)):
        values.append(item)
    return True


def snapshot_state(document, plist):
    """Capture only rows this package may later restore or remove."""
    arrays = identity_arrays(document)
    matches = target_indices(document)
    if matches:
        entries = [{
            "index": index,
            "action": "restore",
            "vendor": arrays[0][index],
            "product": arrays[1][index],
            "name": arrays[2][index],
        } for index in matches]
    else:
        entries = [{
            "index": len(arrays[0]),
            "action": "remove",
            "vendor": "0x1209",
            "product": "0x0001",
            "name": READER_NAME,
        }]
    return {"version": STATE_VERSION, "plist": str(plist.resolve()), "entries": entries}


def validate_state(state, plist):
    if not isinstance(state, dict) or state.get("version") != STATE_VERSION:
        raise ValueError("unsupported CCID metadata state")
    if state.get("plist") != str(plist.resolve()):
        raise ValueError("CCID metadata state belongs to a different plist")
    entries = state.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("CCID metadata state has no entries")
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("action") not in ("restore", "remove") or \
                not isinstance(entry.get("index"), int) or entry["index"] < 0 or \
                not isinstance(entry.get("vendor"), str) or \
                not isinstance(entry.get("product"), str) or \
                not isinstance(entry.get("name"), str):
            raise ValueError("malformed CCID metadata state entry")


def load_state(path, plist):
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("cannot read CCID metadata state: " + str(error)) from error
    validate_state(state, plist)
    return state


def _fsync_directory(path):
    descriptor = os.open(str(path), os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_state(path, state):
    """Durably publish state before the separate plist mutation begins."""
    encoded = (json.dumps(state, sort_keys=True, indent=2) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    try:
        if temporary.exists():
            temporary.unlink()
        descriptor = os.open(str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def remove_state(path):
    if path.exists():
        path.unlink()
        _fsync_directory(path.parent)


def read_document(plist):
    if plist.stat().st_size > MAX_PLIST_SIZE:
        raise ValueError("unexpected plist size exceeds 8 MiB")
    try:
        document = plistlib.loads(plist.read_bytes())
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        raise ValueError("cannot read libccid plist: " + str(error)) from error
    if not isinstance(document, dict) or "ccid" not in str(document.get("CFBundleIdentifier", "")).lower():
        raise ValueError("target is not a libccid bundle")
    identity_arrays(document)
    return document


def write_document_in_place(plist, document):
    """Do not replace the plist or restore it from a broad backup."""
    encoded = plistlib.dumps(document, sort_keys=False)
    with plist.open("r+b") as target:
        target.write(encoded)
        target.truncate()
        target.flush()
        os.fsync(target.fileno())


def _entry_is_owned(arrays, entry):
    index = entry["index"]
    return index < len(arrays[0]) and arrays[0][index] == entry["vendor"] and \
        arrays[1][index] == entry["product"] and arrays[2][index] == READER_NAME


def _merge_current_entries(state, document):
    """Record newly observed target rows before making them package-owned."""
    arrays = identity_arrays(document)
    known = {entry["index"] for entry in state["entries"]
             if _entry_is_owned(arrays, entry)}
    for index in target_indices(document):
        if index not in known:
            # A user may have renamed a row that an earlier apply created or
            # normalized. Its new name is now the restoration baseline; keep
            # one action for this slot so restore cannot remove it first.
            state["entries"] = [entry for entry in state["entries"]
                                if entry["index"] != index]
            state["entries"].append({
                "index": index,
                "action": "restore",
                "vendor": arrays[0][index],
                "product": arrays[1][index],
                "name": arrays[2][index],
            })


def _state_row_was_edited(document, state):
    """Recognize an in-place user edit before treating a missing row as an upgrade."""
    arrays = identity_arrays(document)
    for entry in state["entries"]:
        index = entry["index"]
        if index < len(arrays[0]) and arrays[2][index] == READER_NAME and \
                (arrays[0][index] != entry["vendor"] or
                 arrays[1][index] != entry["product"]):
            return True
    return False


def apply_metadata(plist, state_path=None):
    """Apply metadata, optionally recording a reversible package lifecycle state."""
    document = read_document(plist)
    if state_path is None:
        changed = add_identity(document)
        if changed:
            write_document_in_place(plist, document)
        return changed

    if state_path.exists():
        state = load_state(state_path, plist)
        # A libccid package replacement has removed every target row. Its old
        # positions no longer describe this plist, so take a fresh baseline.
        matches = target_indices(document)
        if not matches:
            if _state_row_was_edited(document, state):
                state["entries"].append({
                    "index": len(identity_arrays(document)[0]),
                    "action": "remove",
                    "vendor": "0x1209",
                    "product": "0x0001",
                    "name": READER_NAME,
                })
            else:
                state = snapshot_state(document, plist)
        else:
            _merge_current_entries(state, document)
    else:
        state = snapshot_state(document, plist)
    validate_state(state, plist)
    write_state(state_path, state)
    changed = add_identity(document)
    if changed:
        write_document_in_place(plist, document)
    return changed


def restore_metadata(plist, state_path):
    """Undo only unchanged rows recorded by this package's apply operation."""
    if not state_path.exists():
        return False
    document = read_document(plist)
    state = load_state(state_path, plist)
    arrays = identity_arrays(document)
    changed = False
    for entry in sorted(state["entries"], key=lambda item: item["index"], reverse=True):
        if not _entry_is_owned(arrays, entry):
            continue
        index = entry["index"]
        if entry["action"] == "remove":
            for values in arrays:
                del values[index]
        else:
            arrays[2][index] = entry["name"]
        changed = True
    if changed:
        write_document_in_place(plist, document)
    remove_state(state_path)
    return changed


def find_plist(root):
    patterns = (
        "usr/lib/pcsc/drivers/*ccid.bundle/Contents/Info.plist",
        "usr/lib/*/pcsc/drivers/*ccid.bundle/Contents/Info.plist",
        "usr/lib64/pcsc/drivers/*ccid.bundle/Contents/Info.plist",
        "usr/local/lib/pcsc/drivers/*ccid.bundle/Contents/Info.plist",
        "usr/local/lib64/pcsc/drivers/*ccid.bundle/Contents/Info.plist",
    )
    candidates = sorted({path.resolve() for pattern in patterns
                         for path in root.glob(pattern) if path.is_file()})
    if not candidates:
        raise ValueError("libccid Info.plist not found; install libccid or set "
                         "CCID_PLIST=/absolute/path/to/Info.plist")
    if len(candidates) != 1:
        raise ValueError("multiple libccid bundles found; set CCID_PLIST to one of: "
                         + ", ".join(map(str, candidates)))
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument("--apply", action="store_true", help="apply reader metadata")
    operation.add_argument("--restore", action="store_true", help="restore rows owned by this package")
    parser.add_argument("--state", type=Path, help="durable package lifecycle state path")
    parser.add_argument("--root", type=Path, default=Path("/"),
                        help="root for automatic bundle discovery")
    parser.add_argument("plist", type=Path, nargs="?")
    args = parser.parse_args()
    try:
        plist = args.plist if args.plist is not None else find_plist(args.root)
        if args.restore:
            if args.state is None:
                parser.error("--restore requires --state")
            changed = restore_metadata(plist, args.state)
            print("Restored package-owned CCID metadata" if changed else
                  "No package-owned CCID metadata needed restoration")
            return
        document = read_document(plist)
        needs_update = bool(add_identity(document))
        if not args.apply:
            print("1209:0001 reader metadata needs updating; use --apply" if needs_update else
                  "1209:0001 reader metadata is current")
            return
        changed = apply_metadata(plist, args.state)
        print("Updated 1209:0001 reader metadata; all other readers/options preserved" if changed else
              "1209:0001 reader metadata is current")
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
