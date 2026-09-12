#!/usr/bin/env python3
"""Pure tests of the production shader emitter/validator; no compiler or GPU."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("shader_generator", ROOT / "scripts/generate_render_shaders.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ShaderGenerationTest(unittest.TestCase):
    def test_all_byte_values_round_trip_deterministically(self):
        data = bytes(range(256))
        first = generator.emit_array("test_bytes", data)
        self.assertEqual(first, generator.emit_array("test_bytes", data))
        self.assertEqual(generator.read_array(first, "test_bytes"), data)
        self.assertIn("0x80", first)
        self.assertIn("0xff", first)
        self.assertNotIn("0xffff0x", first)

    def test_malformed_signed_tokens_duplicates_and_size_drift_are_rejected(self):
        correct = generator.emit_array("data", b"\x80\xff\x00")
        for text in (correct.replace("0x80", "0xffff0x80"), correct + correct,
                     correct.replace("sizeof(data)", "2"), correct.replace("0xff,", ""),
                     correct.replace("0x00", "0x100")):
            with self.subTest(text=text):
                # A removed byte with sizeof still parses as shorter data, so
                # the production caller must also compare to the compiler bytes.
                try:
                    decoded = generator.read_array(text, "data")
                except ValueError:
                    continue
                self.assertNotEqual(decoded, b"\x80\xff\x00")

    def test_replacement_preserves_unrelated_arrays_and_namespace(self):
        unrelated = generator.emit_array("untouched", b"\x01\x02")
        original = "namespace Caesura {\n" + unrelated + generator.emit_array("changed", b"\x03") + "} // namespace Caesura\n"
        updated = generator.replace_array(original, "changed", b"\x80\xff")
        self.assertIn(unrelated, updated)
        self.assertEqual(generator.read_array(updated, "changed"), b"\x80\xff")
        added = generator.replace_array(updated, "new_shader", b"\x04", allow_new=True)
        self.assertIn(unrelated, added)
        self.assertEqual(generator.read_array(added, "new_shader"), b"\x04")
        with self.assertRaises(ValueError):
            generator.replace_array(original, "missing", b"\x01")

    def test_platform_manifest_cannot_drop_existing_or_compiled_programs(self):
        binaries = {name: b"\x80\x00\xff" for name in generator.PLATFORM_SHADERS}
        previous = "\n".join(generator.emit_array("kEmbeddedGL_" + name, data)
                             for name, data in binaries.items() if name != generator.NEW_SHADER)
        output = generator.platform_source(previous, "GL", binaries)
        for name, data in binaries.items():
            self.assertEqual(generator.read_array(output, "kEmbeddedGL_" + name), data)
        with self.assertRaises(ValueError):
            generator.platform_source(previous + generator.emit_array("kEmbeddedGL_unlisted", b"\x01"), "GL", binaries)
        with self.assertRaises(ValueError):
            generator.platform_source(previous, "GL", {name: data for name, data in binaries.items() if name != "fs_blend"})

    def test_utf8_bom_and_line_endings_are_explicitly_normalized(self):
        self.assertEqual(generator.normalize_source(b"\xef\xbb\xbf// source\r\n"), b"// source\n")
        self.assertEqual(generator.normalize_source("// 中文\n".encode()), "// 中文\n".encode())
        with self.assertRaises(UnicodeDecodeError):
            generator.normalize_source(b"\xff\xfe")


if __name__ == "__main__":
    unittest.main(verbosity=2)
