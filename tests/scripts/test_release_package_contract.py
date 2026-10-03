"""Pure filesystem fixtures for U22 identity/extraction; no runtime PASS claims."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts/package_verification.py"
if MODULE_PATH.exists():
    SPEC = importlib.util.spec_from_file_location("package_verification", MODULE_PATH)
    pv = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = pv
    SPEC.loader.exec_module(pv)
else:
    pv = None


class ReleasePackageContract(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(pv, "Missing U22 implementation: scripts/package_verification.py")
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-u22-fixture-")
        self.addCleanup(self.temp.cleanup)
        # macOS exposes its system temp directory through /var -> /private/var.
        # Give the checker a canonical controlled parent; explicit link tests
        # below still pass the unresolved attacker-controlled paths.
        self.root = Path(self.temp.name).resolve()

    def archive(self, members=None, name="final 中文 package.zip"):
        path = self.root / name
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(path, "w") as archive:
                for member, data in members or [("Caesura/scripts/main.lua", b"return 1"),
                                                ("Caesura/assets/中文 文件.txt", b"asset")]:
                    archive.writestr(member, data)
        return path

    def digest(self, path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def prepare(self, source, name="attempt 中文 01", **kwargs):
        if source.is_file() and "expected_sha256" not in kwargs:
            kwargs["expected_sha256"] = self.digest(source)
        return pv.prepare_package(source, self.root / name, **kwargs)

    def failed(self, source, name="attempt failed", **kwargs):
        with self.assertRaises(pv.PackageVerificationError) as caught:
            self.prepare(source, name, **kwargs)
        report = json.loads((self.root / name / "preparation.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertTrue(report["error"])
        self.assertFalse((self.root / name / "package").exists())
        self.assertEqual(caught.exception.report, report)
        return report

    def test_explicit_archive_identity_separate_from_content(self):
        source = self.archive()
        report = self.prepare(source)
        self.assertEqual(report["status"], "PREPARED")
        self.assertEqual(report["scope"], "package_identity_and_extraction")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertEqual(report["input"]["archive_sha256"], self.digest(source))
        self.assertNotEqual(report["input"]["archive_sha256"], report["inventory"]["sha256"])
        self.assertEqual((Path(report["package_path"]) / "Caesura/assets/中文 文件.txt").read_bytes(), b"asset")
        self.assertTrue(report["input_stable"])
        self.assertEqual(pv.verify_stable(report)["status"], "STABLE")

    def test_required_external_identity_and_changed_archive_bytes(self):
        source = self.archive()
        expected = self.digest(source)
        self.failed(source, "missing identity", expected_sha256=None)
        with source.open("ab") as stream:
            stream.write(b"changed after caller locked final bytes")
        report = self.failed(source, "changed final bytes", expected_sha256=expected)
        self.assertIn("identity", report["error"].lower())
        self.failed(source, "bad identity syntax", expected_sha256="not-a-hash")

    def test_existing_attempt_is_unchanged_even_for_invalid_source(self):
        attempt = self.root / "existing"
        attempt.mkdir()
        sentinel = attempt / "preparation.json"
        sentinel.write_bytes(b"old failure must survive")
        with self.assertRaises(pv.PackageVerificationError):
            pv.prepare_package(self.root / "missing.zip", attempt, expected_sha256="0" * 64)
        self.assertEqual(list(attempt.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"old failure must survive")

    def test_directory_requires_inventory_and_detects_entry_set_changes(self):
        source = self.root / "directory 输入"
        source.mkdir()
        (source / "empty").mkdir()
        (source / "file").write_bytes(b"locked")
        inventory = pv.inspect_inventory(source)
        self.failed(source, "missing directory identity")
        good = self.prepare(source, expected_inventory_sha256=inventory["sha256"])
        self.assertIsNone(good["input"]["archive_sha256"])
        self.assertEqual(good["inventory"]["sha256"], inventory["sha256"])
        (source / "new empty directory").mkdir()
        self.failed(source, "entry added", expected_inventory_sha256=inventory["sha256"])
        with self.assertRaises(pv.PackageVerificationError):
            pv.verify_stable(good)

    def test_inventory_order_is_stable_and_file_mutation_is_not(self):
        a, b = self.root / "a", self.root / "b"
        a.mkdir()
        b.mkdir()
        for name in ("z", "中文", "a"):
            (a / name).write_text(name, encoding="utf-8")
        for name in ("a", "中文", "z"):
            (b / name).write_text(name, encoding="utf-8")
        self.assertEqual(pv.inspect_inventory(a)["sha256"], pv.inspect_inventory(b)["sha256"])
        (b / "z").write_text("changed", encoding="utf-8")
        self.assertNotEqual(pv.inspect_inventory(a)["sha256"], pv.inspect_inventory(b)["sha256"])

    def test_executable_suffixes_survive_inventory_copy_and_stability(self):
        # Windows path stat infers execute bits from these suffixes, while
        # fstat on the very same open file handle does not. These bytes are
        # never executed: this is the package identity/copy contract.
        source = self.root / "executable suffixes"
        source.mkdir()
        for suffix in (".exe", ".bat", ".cmd", ".com", ".bin"):
            (source / ("tool" + suffix)).write_bytes(b"fixed package bytes")
        expected = pv.inspect_inventory(source)["sha256"]
        report = self.prepare(source, expected_inventory_sha256=expected)
        self.assertEqual(report["inventory"]["sha256"], expected)
        self.assertEqual(pv.verify_stable(report)["status"], "STABLE")
        (Path(report["package_path"]) / "tool.exe").write_bytes(b"other package bytes")
        with self.assertRaises(pv.PackageVerificationError):
            pv.verify_stable(report)

    def test_unsafe_zip_names_duplicates_and_parent_conflicts(self):
        cases = [
            [("../escape", b"x")], [("/absolute", b"x")], [("C:/drive", b"x")],
            [("a\\..\\escape", b"x")], [("file:stream", b"x")], [("NUL.txt", b"x")],
            [("same", b"a"), ("same", b"b")], [("A", b"a"), ("a", b"b")],
            [("parent", b"a"), ("parent/child", b"b")],
            [("./same", b"a"), ("same", b"b")],
        ]
        for kind in ("zip", "tar"):
            for index, members in enumerate(cases):
                with self.subTest(kind=kind, members=members):
                    source = (self.archive(members, f"unsafe-{index}.zip") if kind == "zip"
                              else self.tar(f"unsafe-{index}.tar", [("file", path, data)
                                            for path, data in members], mode="w"))
                    report = self.failed(source, f"unsafe-{kind}-{index}")
                    self.assertNotIn("not a gzip file", report["error"])
        self.assertFalse((self.root / "escape").exists())

    def tar(self, name, members, *, mode="w:gz"):
        source = self.root / name
        with tarfile.open(source, mode) as archive:
            for kind, path, value in members:
                member = tarfile.TarInfo(path)
                member.mode = 0o755 if kind == "dir" else 0o644
                if kind == "file":
                    member.size = len(value)
                    archive.addfile(member, io.BytesIO(value))
                else:
                    member.type = {"symlink": tarfile.SYMTYPE, "hardlink": tarfile.LNKTYPE,
                                   "dir": tarfile.DIRTYPE, "fifo": tarfile.FIFOTYPE}[kind]
                    member.linkname = value
                    archive.addfile(member)
        return source

    def test_plain_tar_root_and_exact_archive_identity(self):
        members = [("dir", ".", ""), ("file", "./index.html", b"<p>fixture</p>"),
                   ("dir", "empty", ""), ("file", ".nojekyll", b""),
                   ("file", "assets/中文 文件.txt", b"asset")]
        source = self.tar("artifact.tar", members, mode="w")
        report = self.prepare(source)
        self.assertEqual(report["status"], "PREPARED")
        self.assertEqual(report["runtime"], "NOT_RUN")
        self.assertEqual(report["input"]["archive_sha256"], self.digest(source))
        payload = Path(report["package_path"])
        self.assertEqual((payload / "assets/中文 文件.txt").read_bytes(), b"asset")
        self.assertEqual((payload / "index.html").read_bytes(), b"<p>fixture</p>")
        self.assertTrue((payload / "empty").is_dir())
        self.assertTrue((payload / ".nojekyll").is_file())
        self.assertEqual(pv.verify_stable(report)["status"], "STABLE")
        # Change only a real tar header, retaining the same payload inventory.
        with tarfile.open(source, "w") as archive:
            for kind, name, data in members:
                entry = tarfile.TarInfo(name)
                entry.mtime = 1
                entry.mode = 0o755 if kind == "dir" else 0o644
                entry.type = tarfile.DIRTYPE if kind == "dir" else tarfile.REGTYPE
                entry.size = len(data) if kind == "file" else 0
                archive.addfile(entry, io.BytesIO(data) if kind == "file" else None)
        with self.assertRaises(pv.PackageVerificationError):
            pv.verify_stable(report)
        self.failed(source, "old tar identity", expected_sha256=report["input"]["archive_sha256"])
        changed = self.prepare(source, "new tar identity")
        self.assertEqual(report["inventory"]["sha256"], changed["inventory"]["sha256"])
        self.assertNotEqual(report["input"]["archive_sha256"], changed["input"]["archive_sha256"])

    def test_tar_formats_keep_entry_and_byte_limits(self):
        for mode in ("w:gz", "w"):
            for limit, maximum, members in (
                    ("MAX_BYTES", 2, [("file", "large", b"123")]),
                    ("MAX_ENTRIES", 1, [("file", "a", b"x"), ("file", "b", b"y")])):
                name = mode.replace(":", "-") + limit
                with self.subTest(mode=mode, limit=limit), mock.patch.object(pv, limit, maximum):
                    source = self.tar(name + ".tar", members, mode=mode)
                    report = self.failed(source, "limited-" + name)
                    self.assertIn("limit", report["error"])

    def test_other_tar_compression_is_not_implicitly_enabled(self):
        for mode in ("w:bz2", "w:xz"):
            with self.subTest(mode=mode):
                name = mode.replace(":", "-")
                source = self.tar(name + ".tar", [("file", "index.html", b"fixture")], mode=mode)
                self.failed(source, "unsupported-" + name)

    def test_tgz_linux_and_framework_relative_links(self):
        source = self.tar("CPack Linux Mac.tgz", [
            ("file", "Caesura/libSDL.so.3.2", b"library"),
            ("symlink", "Caesura/libSDL.so.3", "libSDL.so.3.2"),
            ("symlink", "Caesura/libSDL.so", "libSDL.so.3"),
            ("file", "Caesura/Thing.framework/Versions/A/Resources/file", b"resource"),
            ("symlink", "Caesura/Thing.framework/Versions/Current", "A"),
            ("symlink", "Caesura/Thing.framework/Resources", "Versions/Current/Resources"),
            ("hardlink", "Caesura/lib-copy", "Caesura/libSDL.so.3.2"),
        ])
        report = self.prepare(source)
        out = Path(report["package_path"]) / "Caesura"
        self.assertTrue((out / "libSDL.so").is_symlink())
        self.assertEqual((out / "libSDL.so").read_bytes(), b"library")
        self.assertEqual((out / "Thing.framework/Resources/file").read_bytes(), b"resource")
        self.assertEqual((out / "lib-copy").read_bytes(), b"library")
        self.assertEqual(pv.verify_stable(report)["status"], "STABLE")

    def test_tar_unsafe_links_and_special_entries(self):
        base = [("file", "pkg/real", b"safe")]
        cases = [
            [("symlink", "pkg/link", "../../escape")],
            [("symlink", "pkg/link", "/tmp/escape")],
            [("symlink", "pkg/link", "missing")],
            [("symlink", "pkg/a", "b"), ("symlink", "pkg/b", "a")],
            [("symlink", "pkg/link", "."), ("file", "pkg/link/write", b"bad")],
            [("hardlink", "pkg/link", "../escape")],
            [("fifo", "pkg/pipe", "")],
        ]
        for mode in ("w:gz", "w"):
            for index, members in enumerate(cases):
                with self.subTest(mode=mode, members=members):
                    name = mode.replace(":", "-") + str(index)
                    report = self.failed(self.tar(f"links-{name}.tar", base + members, mode=mode),
                                         f"links-{name}")
                    self.assertNotIn("not a gzip file", report["error"])

    def test_plain_tar_preserves_safe_internal_links(self):
        source = self.tar("internal-links.tar", [("file", "pkg/real", b"same"),
                          ("symlink", "pkg/link", "real"),
                          ("hardlink", "pkg/hard", "pkg/real")], mode="w")
        report = self.prepare(source)
        payload = Path(report["package_path"]) / "pkg"
        self.assertTrue((payload / "link").is_symlink())
        self.assertEqual((payload / "link").read_bytes(), b"same")
        self.assertEqual((payload / "hard").read_bytes(), b"same")
        self.assertEqual(pv.verify_stable(report)["status"], "STABLE")

    def test_zip_symlinks_preserved_and_target_changes_detected(self):
        source = self.archive([("pkg/real", b"one"), ("pkg/other", b"two")])
        with zipfile.ZipFile(source, "a") as archive:
            link = zipfile.ZipInfo("pkg/link")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(link, "real")
        report = self.prepare(source)
        out = Path(report["package_path"]) / "pkg/link"
        self.assertEqual(os.readlink(out), "real")
        out.unlink()
        out.symlink_to("other")
        with self.assertRaises(pv.PackageVerificationError):
            pv.verify_stable(report)

    def test_destination_inside_source_and_symlink_destination_refused(self):
        source = self.root / "source"
        source.mkdir()
        (source / "file").write_bytes(b"original")
        expected = pv.inspect_inventory(source)["sha256"]
        with self.assertRaises(pv.PackageVerificationError):
            pv.prepare_package(source, source / "nested attempt", expected_inventory_sha256=expected)
        self.assertEqual(pv.inspect_inventory(source)["sha256"], expected)
        alias = self.root / "alias"
        alias.symlink_to(source, target_is_directory=True)
        with self.assertRaises(pv.PackageVerificationError):
            pv.prepare_package(source, alias / "attempt", expected_inventory_sha256=expected)
        self.assertEqual(pv.inspect_inventory(source)["sha256"], expected)

    def test_source_mutated_during_extraction_fails_and_retains_attempt(self):
        source = self.archive()
        copy_stream = pv._copy_stream
        mutated = False
        def change_after_copy(*args, **kwargs):
            nonlocal mutated
            result = copy_stream(*args, **kwargs)
            if not mutated:
                mutated = True
                with source.open("ab") as stream:
                    stream.write(b"concurrent change")
            return result
        with mock.patch.object(pv, "_copy_stream", side_effect=change_after_copy):
            report = self.failed(source)
        self.assertFalse(report["input_stable"])
        self.assertEqual(report["cleanup"]["status"], "REMOVED_PARTIAL_PACKAGE")

    def test_plain_tar_mutation_after_payload_copy_preserves_failure(self):
        source = self.tar("mutable.tar", [("file", "index.html", b"fixture")], mode="w")
        copy_stream = pv._copy_stream
        copied = False
        def mutate(*args, **kwargs):
            nonlocal copied
            result = copy_stream(*args, **kwargs)
            copied = True
            with source.open("ab") as stream:
                stream.write(b"changed final archive")
            return result
        with mock.patch.object(pv, "_copy_stream", side_effect=mutate):
            report = self.failed(source, "tar mutation")
        self.assertTrue(copied)
        self.assertFalse(report["input_stable"])
        self.assertEqual(report["cleanup"]["status"], "REMOVED_PARTIAL_PACKAGE")

    def test_directory_mutated_during_copy_fails(self):
        source = self.root / "source"
        source.mkdir()
        (source / "file").write_bytes(b"original")
        expected = pv.inspect_inventory(source)["sha256"]
        copy_stream = pv._copy_stream
        def add_entry(*args, **kwargs):
            result = copy_stream(*args, **kwargs)
            (source / "added").write_bytes(b"later")
            return result
        with mock.patch.object(pv, "_copy_stream", side_effect=add_entry):
            self.failed(source, expected_inventory_sha256=expected)

    def test_extracted_inventory_mismatch_and_cleanup_error_are_failure(self):
        source = self.archive()
        self.failed(source, "wrong inventory", expected_inventory_sha256="0" * 64)
        with mock.patch.object(pv.shutil, "rmtree", side_effect=OSError("fixture cleanup denied")):
            with self.assertRaises(pv.PackageVerificationError):
                self.prepare(source, "cleanup failed", expected_inventory_sha256="0" * 64)
        report = json.loads((self.root / "cleanup failed/preparation.json").read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["cleanup"]["status"], "FAILED")
        self.assertIn("fixture cleanup denied", report["cleanup"]["error"])

    def test_corrupt_archive_fails_without_picking_other_archive(self):
        self.archive(name="another valid package.zip")
        broken = self.root / "explicit broken.zip"
        broken.write_bytes(b"not a zip")
        self.failed(broken)

    def test_interrupt_records_failure_and_cleans_only_partial_package(self):
        source = self.archive()
        with mock.patch.object(pv, "_copy_stream", side_effect=KeyboardInterrupt("fixture cancellation")):
            report = self.failed(source, "cancelled")
        self.assertIn("fixture cancellation", report["error"])
        self.assertEqual(report["cleanup"]["status"], "REMOVED_PARTIAL_PACKAGE")
        self.assertTrue(source.is_file())

    def test_directory_source_link_target_changed_during_copy(self):
        source = self.root / "source"
        source.mkdir()
        (source / "a").write_bytes(b"same")
        (source / "b").write_bytes(b"same")
        (source / "link").symlink_to("a")
        expected = pv.inspect_inventory(source)["sha256"]
        copy_stream = pv._copy_stream
        def change_target(*args, **kwargs):
            result = copy_stream(*args, **kwargs)
            (source / "link").unlink()
            (source / "link").symlink_to("b")
            return result
        with mock.patch.object(pv, "_copy_stream", side_effect=change_target):
            report = self.failed(source, expected_inventory_sha256=expected)
        self.assertFalse(report["input_stable"])

    def test_archive_metadata_change_keeps_content_identity_but_invalidates_final_bytes(self):
        source = self.archive()
        report = self.prepare(source)
        with zipfile.ZipFile(source, "a") as archive:
            archive.comment = b"post-verification transformation"
        with self.assertRaises(pv.PackageVerificationError):
            pv.verify_stable(report)
        changed = self.prepare(source, "new final bytes")
        self.assertNotEqual(report["input"]["archive_sha256"], changed["input"]["archive_sha256"])
        self.assertEqual(report["inventory"]["sha256"], changed["inventory"]["sha256"])

    def test_cli_prepare_and_stability_have_no_runtime_acceptance(self):
        for kind, source in (("zip", self.archive()),
                             ("tar", self.tar("cli.tar", [("file", "index.html", b"fixture")], mode="w"))):
            with self.subTest(kind=kind):
                attempt = self.root / ("cli attempt " + kind)
                process = subprocess.run([sys.executable, "-X", "utf8", str(MODULE_PATH), "prepare",
                                          "--input", str(source), "--attempt", str(attempt),
                                          "--expected-sha256", self.digest(source)],
                                         capture_output=True, text=True, encoding="utf-8", check=False)
                self.assertEqual(process.returncode, 0, process.stderr + process.stdout)
                self.assertEqual(json.loads(process.stdout)["runtime"], "NOT_RUN")
                receipt = attempt / "preparation.json"
                before = receipt.read_bytes()
                process = subprocess.run([sys.executable, "-X", "utf8", str(MODULE_PATH), "verify-stable",
                                          "--receipt", str(receipt)],
                                         capture_output=True, text=True, encoding="utf-8", check=False)
                self.assertEqual(process.returncode, 0, process.stderr + process.stdout)
                self.assertEqual(json.loads(process.stdout)["status"], "STABLE")
                self.assertEqual(json.loads(process.stdout)["runtime"], "NOT_RUN")
                self.assertEqual(receipt.read_bytes(), before)

    def test_stability_rejects_same_size_edit_to_previously_hashed_file(self):
        source = self.archive([("a.bin", b"GOOD"), ("z.bin", b"KEEP")])
        source_digest = self.digest(source)
        report = self.prepare(source)
        package = Path(report["package_path"])
        receipt = Path(report["attempt_path"]) / "preparation.json"
        receipt_bytes = receipt.read_bytes()
        original_hash = pv._sha256_file
        changed = False
        def change_previously_hashed_file(path):
            nonlocal changed
            digest = original_hash(path)
            if Path(path) == package / "z.bin" and not changed:
                changed = True
                victim = package / "a.bin"
                before = victim.stat()
                victim.write_bytes(b"EVIL")
                # Ensure a distinct mtime even on coarse-resolution filesystems;
                # the injected mutation is real and keeps the byte count equal.
                os.utime(victim, ns=(before.st_atime_ns, before.st_mtime_ns + 1_000_000_000))
            return digest
        with mock.patch.object(pv, "_sha256_file", side_effect=change_previously_hashed_file):
            with self.assertRaises(pv.PackageVerificationError):
                pv.verify_stable(report)
        self.assertTrue(changed)
        self.assertEqual((package / "a.bin").read_bytes(), b"EVIL")
        self.assertNotEqual(pv.inspect_inventory(package)["sha256"], report["inventory"]["sha256"])
        self.assertEqual(self.digest(source), source_digest)
        self.assertEqual(receipt.read_bytes(), receipt_bytes)


if __name__ == "__main__":
    unittest.main()
