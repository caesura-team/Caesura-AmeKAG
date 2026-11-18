"""Run the real extractor against caller-controlled output paths and valid CARCs."""

import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


class CarcExtractCli(unittest.TestCase):
    payload = b"archived bytes"
    sentinel = b"keep the original"
    member = "audio/voice.bin"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="caesura_carc_extract_")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.outside = self.root / "outside"
        self.outside.mkdir()
        self.original = self.outside / "voice.bin"
        self.original.write_bytes(self.sentinel)

    def command(self, executable, *args):
        return subprocess.run(
            [str(executable), *map(str, args)], cwd=self.root,
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=15,
        )

    def create_archive(self, *names):
        encoding = "mbcs" if os.name == "nt" else sys.getfilesystemencoding()
        (self.root / "members.txt").write_bytes("\n".join(names).encode(encoding) + b"\n")
        result = self.command(fixture)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def extract(self, *args, accepted=True):
        result = self.command(packer, "extract", "crafted.carc", self.output, *args)
        details = result.stdout + result.stderr
        if accepted:
            self.assertEqual(result.returncode, 0, details)
        else:
            self.assertEqual(self.original.read_bytes(), self.sentinel, details)
            self.assertGreater(result.returncode, 0, details)
            self.assertIn("Error:", result.stderr)
        return result

    def test_nested_and_hash_extraction_keep_regular_overwrite(self):
        self.create_archive(self.member)
        self.extract("--path", self.member)
        target = self.output / self.member
        self.assertEqual(target.read_bytes(), self.payload)
        target.write_bytes(self.sentinel)
        self.extract("--path", self.member)
        self.assertEqual(target.read_bytes(), self.payload)
        self.extract()
        name = hashlib.sha256(self.member.encode("ascii")).hexdigest()
        self.assertEqual((self.output / name).read_bytes(), self.payload)

    def test_overwrite_preserves_existing_access_permissions(self):
        self.create_archive(self.member)
        target = self.output / self.member
        target.parent.mkdir()
        target.write_bytes(self.sentinel)

        if os.name == "nt":
            result = self.command("icacls.exe", target, "/inheritance:d")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            def access():
                saved = self.root / "permissions.acl"
                result = self.command("icacls.exe", target, "/save", saved)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return saved.read_bytes()
        else:
            target.chmod(0o600)

            def access():
                return stat.S_IMODE(target.stat().st_mode)

        original_access = access()
        self.extract("--path", self.member)
        self.assertEqual(target.read_bytes(), self.payload)
        self.assertEqual(access(), original_access)

    def test_parent_and_absolute_archive_members_cannot_overwrite_outside(self):
        names = ["../outside/voice.bin", str(self.original), "audio/../voice.bin"]
        if os.name == "nt":
            names += ["C:voice.bin", "audio/.. /voice.bin", "audio/voice.bin:stream"]
        for name in names:
            with self.subTest(name=name):
                archive = self.root / "crafted.carc"
                archive.unlink(missing_ok=True)
                self.create_archive(name)
                result = self.extract("--path", name, accepted=False)
                self.assertIn("invalid archive member path", result.stderr)
                self.assertEqual(list(self.output.iterdir()), [])

    def link_directory(self, link):
        if os.name == "nt":
            import _winapi
            _winapi.CreateJunction(str(self.outside), str(link))
        else:
            link.symlink_to(self.outside, target_is_directory=True)

    def test_directory_link_cannot_redirect_named_member(self):
        self.create_archive(self.member)
        self.link_directory(self.output / "audio")
        self.extract("--path", self.member, accepted=False)

    def test_output_root_link_cannot_redirect_hash_extraction(self):
        self.create_archive(self.member)
        self.output.rmdir()
        self.link_directory(self.output)
        self.extract(accepted=False)
        self.assertEqual(list(self.outside.iterdir()), [self.original])

    def test_existing_hash_hardlink_cannot_overwrite_outside(self):
        self.create_archive(self.member)
        name = hashlib.sha256(self.member.encode("ascii")).hexdigest()
        os.link(self.original, self.output / name)
        self.extract(accepted=False)

    @unittest.skipIf(os.name != "nt" and os.geteuid() == 0, "requires ordinary file permissions")
    def test_read_only_destination_keeps_original_and_cleans_staging(self):
        self.create_archive(self.member)
        target = self.output / self.member
        target.parent.mkdir()
        target.write_bytes(self.sentinel)
        target.chmod(stat.S_IREAD if os.name == "nt" else 0o400)
        try:
            self.extract("--path", self.member, accepted=False)
            self.assertEqual(target.read_bytes(), self.sentinel)
            self.assertEqual(list(target.parent.iterdir()), [target])
        finally:
            target.chmod(stat.S_IREAD | stat.S_IWRITE)

    def test_empty_or_missing_path_cannot_select_full_extraction(self):
        self.create_archive(self.member)
        for arguments in (("--path", ""), ("--path",)):
            with self.subTest(arguments=arguments):
                self.extract(*arguments, accepted=False)
                self.assertEqual(list(self.output.iterdir()), [])

    def import_carc(self, archive, member):
        tool = self.root / ("bin/Debug/carc_pack.exe" if os.name == "nt" else "bin/carc_pack")
        tool.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(packer, tool)
        importer = Path(__file__).resolve().parents[2] / "scripts/kag3_import.lua"
        return self.command(lua, importer, "--carc", archive, "--path", member, "-o", self.output)

    def test_failed_import_leaves_stale_scene_untouched(self):
        self.create_archive("different.ks")
        stale = self.root / "kag3_import_carc_tmp" / "legacy.ks"
        stale.parent.mkdir()
        stale.write_bytes(b"[waitse]\n")
        result = self.import_carc("crafted.carc", "legacy.ks")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(stale.read_bytes(), b"[waitse]\n")
        self.assertEqual(list(self.output.iterdir()), [])

    def test_import_uses_archive_bytes_and_preserves_literal_paths(self):
        member = "audio/legacy scene.ks"
        source = self.root / "source"
        (source / member).parent.mkdir(parents=True)
        (source / member).write_bytes(b"[waitse]\r\n")
        archive = self.root / "scene %TEMP% !.carc"
        result = self.command(packer, "pack", source, archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.import_carc(archive, member)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.output / "legacy scene.imported.ks").read_text(), "[waitsound]\n")
        self.assertFalse((self.root / "kag3_import_carc_tmp").exists())


if __name__ == "__main__":
    packer, fixture, lua = (Path(value).resolve(strict=True) for value in sys.argv[1:4])
    unittest.main(argv=[sys.argv[0]], verbosity=2)
