import plistlib
import stat
import struct
import tempfile
import unittest
from pathlib import Path

from merge_macos_apps import lipo, merge_apps


def write_mach_o(path, arch):
    cpu, subtype = (0x0100000C, 0) if arch == "arm64" else (0x01000007, 3)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<IiiIIIII", 0xFEEDFACF, cpu, subtype, 1, 0, 0, 0, 0))
    path.chmod(0o755)


class MergeAppsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.arm, self.intel = root / "arm64.app", root / "x86_64.app"
        self.output = root / "universal.app"
        for arch, app in (("arm64", self.arm), ("x86_64", self.intel)):
            write_mach_o(app / "Contents/MacOS/AyuGram", arch)
            resources = app / "Contents/Resources"
            resources.mkdir()
            (resources / "generated.rcc").write_text(f"Resource timestamp: {arch}")
            (resources / "alias.rcc").symlink_to("generated.rcc")
            with (app / "Contents/Info.plist").open("wb") as stream:
                plistlib.dump({"CFBundleIdentifier": "one.ayugram.AyuGramDesktop",
                               "CFBundleVersion": "7.2.5",
                               "BuildMachineOSBuild": arch}, stream)

    def merge(self):
        merge_apps(self.arm, self.intel, self.output)

    def test_merges_architectures_and_preserves_bundle_metadata(self):
        self.merge()
        binary = self.output / "Contents/MacOS/AyuGram"
        self.assertEqual(set(lipo("-archs", binary).split()), {"arm64", "x86_64"})
        self.assertTrue(binary.stat().st_mode & stat.S_IXUSR)
        alias = self.output / "Contents/Resources/alias.rcc"
        self.assertTrue(alias.is_symlink())
        self.assertEqual(alias.readlink(), Path("generated.rcc"))
        self.assertEqual(alias.read_text(), "Resource timestamp: arm64")

    def test_merges_already_universal_libraries(self):
        library = self.arm / "Contents/Frameworks/libswift.dylib"
        library.parent.mkdir()
        lipo("-create", self.arm / "Contents/MacOS/AyuGram",
             self.intel / "Contents/MacOS/AyuGram", "-output", library)
        other = self.intel / "Contents/Frameworks/libswift.dylib"
        other.parent.mkdir()
        other.write_bytes(library.read_bytes())
        self.merge()
        self.assertEqual(set(lipo("-archs", self.output / library.relative_to(self.arm)).split()),
                         {"arm64", "x86_64"})

    def test_rejects_missing_architecture(self):
        write_mach_o(self.intel / "Contents/MacOS/AyuGram", "arm64")
        with self.assertRaisesRegex(ValueError, "Missing x86_64 slice"):
            self.merge()

    def test_rejects_missing_bundle_file(self):
        (self.intel / "Contents/Resources/extra").write_text("Intel-only")
        with self.assertRaisesRegex(ValueError, "Bundle contents differ"):
            self.merge()

    def test_rejects_mismatched_version(self):
        info = self.intel / "Contents/Info.plist"
        with info.open("rb") as stream:
            values = plistlib.load(stream)
        values["CFBundleVersion"] = "7.2.4"
        with info.open("wb") as stream:
            plistlib.dump(values, stream)
        with self.assertRaisesRegex(ValueError, "CFBundleVersion differs"):
            self.merge()

    def test_rejects_mismatched_symlink(self):
        alias = self.intel / "Contents/Resources/alias.rcc"
        alias.unlink()
        alias.symlink_to("missing.rcc")
        with self.assertRaisesRegex(ValueError, "Symlinks differ"):
            self.merge()

    def test_rejects_non_mach_o_executable(self):
        (self.intel / "Contents/MacOS/AyuGram").write_text("not a binary")
        with self.assertRaisesRegex(ValueError, "Binary types differ"):
            self.merge()


if __name__ == "__main__":
    unittest.main()
