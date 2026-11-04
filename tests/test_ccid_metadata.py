#!/usr/bin/env python3
"""CCID metadata lifecycle tests; no system files or services are touched."""
import copy
import json
import plistlib
import runpy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

helper = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           "linux/ccid_metadata.py"))


def fixture_document(rows):
    return {
        "CFBundleIdentifier": "org.libccid.ifd-ccid",
        "ifdVendorID": [row[0] for row in rows],
        "ifdProductID": [row[1] for row in rows],
        "ifdFriendlyName": [row[2] for row in rows],
        "ifdDriverOptions": "0x42",
    }


class MetadataTest(unittest.TestCase):
    def write_fixture(self, directory, rows):
        plist = directory / "Info.plist"
        plist.write_bytes(plistlib.dumps(fixture_document(rows)))
        return plist

    def read_fixture(self, plist):
        return plistlib.loads(plist.read_bytes())

    def test_registration_preserves_other_readers_and_is_idempotent(self):
        doc = fixture_document([("0x1234", "0x5678", "existing")])
        self.assertTrue(helper["add_identity"](doc))
        self.assertEqual(doc["ifdVendorID"], ["0x1234", "0x1209"])
        self.assertEqual(doc["ifdProductID"], ["0x5678", "0x0001"])
        self.assertEqual(doc["ifdFriendlyName"],
                         ["existing", helper["READER_NAME"]])
        self.assertEqual(doc["ifdDriverOptions"], "0x42")
        snapshot = copy.deepcopy(doc)
        self.assertFalse(helper["add_identity"](doc))
        self.assertEqual(doc, snapshot)

    def test_apply_restore_removes_only_created_row(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [("0x1234", "0x5678", "existing")])
            state = directory / "ccid-metadata.json"
            self.assertTrue(helper["apply_metadata"](plist, state))
            self.assertTrue(state.exists())
            installed = self.read_fixture(plist)
            self.assertEqual(installed["ifdFriendlyName"], ["existing", helper["READER_NAME"]])
            self.assertFalse(helper["apply_metadata"](plist, state))
            self.assertTrue(helper["restore_metadata"](plist, state))
            restored = self.read_fixture(plist)
            self.assertEqual(restored["ifdVendorID"], ["0x1234"])
            self.assertEqual(restored["ifdDriverOptions"], "0x42")
            self.assertFalse(state.exists())

    def test_apply_restore_restores_preexisting_target_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [
                ("0x1209", "0x0001", "preexisting target"),
                ("0x9999", "0x0001", "other"),
            ])
            state = directory / "ccid-metadata.json"
            self.assertTrue(helper["apply_metadata"](plist, state))
            self.assertEqual(self.read_fixture(plist)["ifdFriendlyName"][0], helper["READER_NAME"])
            self.assertTrue(helper["restore_metadata"](plist, state))
            self.assertEqual(self.read_fixture(plist)["ifdFriendlyName"],
                             ["preexisting target", "other"])

    def test_restore_preserves_user_edit_to_package_row(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [("0x1234", "0x5678", "existing")])
            state = directory / "ccid-metadata.json"
            helper["apply_metadata"](plist, state)
            document = self.read_fixture(plist)
            document["ifdFriendlyName"][1] = "user selected name"
            plist.write_bytes(plistlib.dumps(document))
            self.assertFalse(helper["restore_metadata"](plist, state))
            self.assertEqual(self.read_fixture(plist)["ifdFriendlyName"],
                             ["existing", "user selected name"])
            self.assertFalse(state.exists())

    def test_reapply_then_restore_preserves_user_renamed_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [("0x1234", "0x5678", "existing")])
            state = directory / "ccid-metadata.json"
            helper["apply_metadata"](plist, state)
            document = self.read_fixture(plist)
            document["ifdFriendlyName"][1] = "user selected reader name"
            plist.write_bytes(plistlib.dumps(document))
            self.assertTrue(helper["apply_metadata"](plist, state))
            recorded = json.loads(state.read_text(encoding="utf-8"))
            self.assertEqual(len(recorded["entries"]), 1)
            self.assertEqual(recorded["entries"][0]["action"], "restore")
            self.assertEqual(recorded["entries"][0]["name"], "user selected reader name")
            self.assertTrue(helper["restore_metadata"](plist, state))
            self.assertEqual(self.read_fixture(plist)["ifdFriendlyName"],
                             ["existing", "user selected reader name"])

    def test_reapply_preserves_identity_user_edit_and_removes_new_row(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [("0x1234", "0x5678", "existing")])
            state = directory / "ccid-metadata.json"
            helper["apply_metadata"](plist, state)
            document = self.read_fixture(plist)
            document["ifdVendorID"][1] = "0xbeef"
            plist.write_bytes(plistlib.dumps(document))
            self.assertTrue(helper["apply_metadata"](plist, state))
            helper["restore_metadata"](plist, state)
            restored = self.read_fixture(plist)
            self.assertEqual(restored["ifdVendorID"], ["0x1234", "0xbeef"])
            self.assertEqual(restored["ifdFriendlyName"], ["existing", helper["READER_NAME"]])

    def test_reapply_after_libccid_replacement_uses_new_baseline(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plist = self.write_fixture(directory, [("0x1234", "0x5678", "old")])
            state = directory / "ccid-metadata.json"
            helper["apply_metadata"](plist, state)
            plist.write_bytes(plistlib.dumps(fixture_document([
                ("0xabcd", "0xef01", "new libccid row"),
            ])))
            self.assertTrue(helper["apply_metadata"](plist, state))
            recorded = json.loads(state.read_text(encoding="utf-8"))
            self.assertEqual(recorded["entries"], [{
                "action": "remove", "index": 1, "name": helper["READER_NAME"],
                "product": "0x0001", "vendor": "0x1209",
            }])
            helper["restore_metadata"](plist, state)
            self.assertEqual(self.read_fixture(plist)["ifdFriendlyName"], ["new libccid row"])

    def test_malformed_arrays_fail_without_mutation(self):
        doc = {"ifdVendorID": ["0x1209"], "ifdProductID": [],
               "ifdFriendlyName": []}
        original = copy.deepcopy(doc)
        with self.assertRaises(ValueError):
            helper["add_identity"](doc)
        self.assertEqual(doc, original)

    def test_discovery_multiarch_and_duplicate_symlinks(self):
        root = Mock()
        path = Mock()
        path.is_file.return_value = True
        path.resolve.return_value = Path("/stage/usr/lib/x86_64-linux-gnu/"
                                        "pcsc/drivers/ifd-ccid.bundle/Contents/Info.plist")
        root.glob.side_effect = lambda pattern: [path] if pattern.startswith("usr/lib/") else []
        self.assertEqual(helper["find_plist"](root), path.resolve())
        self.assertTrue(any("usr/lib/*/pcsc/" in call.args[0]
                            for call in root.glob.call_args_list))

    def test_missing_and_ambiguous_bundles_fail(self):
        root = Mock()
        root.glob.return_value = []
        with self.assertRaisesRegex(ValueError, "install libccid"):
            helper["find_plist"](root)
        paths = []
        for name in ("one", "two"):
            path = Mock()
            path.is_file.return_value = True
            path.resolve.return_value = Path("/") / name
            paths.append(path)
        root.glob.return_value = paths
        with self.assertRaisesRegex(ValueError, "multiple libccid bundles"):
            helper["find_plist"](root)


if __name__ == "__main__":
    unittest.main()
