import argparse
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path


MACH_O_MAGIC = {
    b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
}


def is_mach_o(path):
    with path.open("rb") as stream:
        return stream.read(4) in MACH_O_MAGIC


def lipo(*arguments):
    return subprocess.check_output(
        ["xcrun", "lipo", *map(str, arguments)], text=True).strip()


def merge_apps(arm, intel, output):
    if output.exists():
        raise ValueError(f"Output already exists: {output}")
    for app in (arm, intel):
        if not (app / "Contents/MacOS/AyuGram").is_file():
            raise ValueError(f"AyuGram executable missing from {app}")

    # Generated resources may contain build timestamps, so use the arm64
    # resource files while requiring the same bundle layout and identity.
    # Combine every Mach-O slice, including Swift libraries that may
    # already contain both architectures, without losing executable bits.
    arm_paths = {path.relative_to(arm) for path in arm.rglob("*")}
    intel_paths = {path.relative_to(intel) for path in intel.rglob("*")}
    if arm_paths != intel_paths:
        raise ValueError(f"Bundle contents differ: {arm_paths ^ intel_paths}")

    binaries = []
    for relative in sorted(arm_paths):
        left, right = arm / relative, intel / relative
        if left.is_symlink() or right.is_symlink():
            if not (left.is_symlink() and right.is_symlink()
                    and left.readlink() == right.readlink()):
                raise ValueError(f"Symlinks differ: {relative}")
        elif left.is_dir() and right.is_dir():
            continue
        elif not (left.is_file() and right.is_file()):
            raise ValueError(f"File types differ: {relative}")
        elif is_mach_o(left) or is_mach_o(right):
            if not (is_mach_o(left) and is_mach_o(right)):
                raise ValueError(f"Binary types differ: {relative}")
            binaries.append(relative)
        elif relative.name == "Info.plist":
            with left.open("rb") as a, right.open("rb") as b:
                left_info, right_info = plistlib.load(a), plistlib.load(b)
            for key in ("CFBundleIdentifier", "CFBundleExecutable",
                        "CFBundleVersion", "CFBundleShortVersionString",
                        "LSMinimumSystemVersion"):
                if left_info.get(key) != right_info.get(key):
                    raise ValueError(f"{key} differs in {relative}")

    shutil.copytree(arm, output, symlinks=True)
    with tempfile.TemporaryDirectory() as temporary:
        for relative in binaries:
            slices = []
            for arch, app in (("arm64", arm), ("x86_64", intel)):
                binary = app / relative
                arches = lipo("-archs", binary).split()
                if arch not in arches:
                    raise ValueError(f"Missing {arch} slice: {binary}")
                if len(arches) > 1:
                    thin = Path(temporary) / arch
                    lipo(binary, "-thin", arch, "-output", thin)
                    slices.append(thin)
                else:
                    slices.append(binary)
            destination = output / relative
            lipo("-create", *slices, "-output", destination)
            shutil.copymode(arm / relative, destination)
            lipo(destination, "-verify_arch", "arm64", "x86_64")
            print(f"Universal: {relative}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("arm64", type=Path)
    parser.add_argument("x86_64", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    merge_apps(arguments.arm64, arguments.x86_64, arguments.output)


if __name__ == "__main__":
    main()
