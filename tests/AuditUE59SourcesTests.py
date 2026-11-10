"""Source-audit orchestration fixtures; no licensed Unreal source required."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile


SPEC = importlib.util.spec_from_file_location("ue59_audit", Path(__file__).parents[1] / "tools/audit_ue59_sources.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class SourceAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def archive(self, name, files):
        path = self.root / name
        with zipfile.ZipFile(path, "w") as archive:
            for key, value in files.items():
                archive.writestr("SourceRoot/" + key, value)
        return path

    def manifest(self, a, b, old=b"old", new=b"new"):
        return {"baseline_sha256": AUDIT.digest_file(a), "candidate_sha256": AUDIT.digest_file(b),
                "critical_files": [{"path": "Engine/header.h", "baseline_sha256": hashlib.sha256(old).hexdigest(),
                                    "candidate_sha256": hashlib.sha256(new).hexdigest()}]}

    def run_audit(self, a, b, manifest):
        # Structural source checks also run against the real archives locally;
        # these fixtures test hashes, missing files and inventory independently.
        with mock.patch.object(AUDIT, "source_checks", return_value=[]):
            return AUDIT.audit(a, b, manifest)

    def test_inventory_and_reviewed_hashes(self):
        a = self.archive("old.zip", {"Engine/header.h": b"old", "deleted": b"x", "same": b"s"})
        b = self.archive("new.zip", {"Engine/header.h": b"new", "added": b"y", "same": b"s"})
        result = self.run_audit(a, b, self.manifest(a, b))
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["inventory_crc_size"], {"modified": 1, "added": 1, "removed": 1})
        self.assertFalse(result["runtime_validated"])

    def test_changed_archive_is_not_implicitly_reviewed(self):
        a = self.archive("old.zip", {"Engine/header.h": b"old"})
        b = self.archive("new.zip", {"Engine/header.h": b"new"})
        manifest = self.manifest(a, b)
        b = self.archive("new.zip", {"Engine/header.h": b"future"})
        result = self.run_audit(a, b, manifest)
        self.assertEqual(len(result["errors"]), 2)

    def test_missing_critical_file(self):
        a = self.archive("old.zip", {"Engine/header.h": b"old"})
        b = self.archive("new.zip", {"Engine/unrelated": b"new"})
        result = self.run_audit(a, b, self.manifest(a, b))
        self.assertIsNone(result["critical_files"][0]["candidate_sha256"])
        self.assertTrue(result["errors"])

    def test_duplicate_relative_paths(self):
        path = self.root / "duplicate.zip"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("RootA/Engine/header.h", b"a")
            archive.writestr("RootB/Engine/header.h", b"b")
        with self.assertRaisesRegex(ValueError, "duplicate archive path"):
            AUDIT.Sources(path)


if __name__ == "__main__":
    unittest.main()
