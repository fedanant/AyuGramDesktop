import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import venv


def source_digest(source, runner):
    digest = hashlib.sha256()
    inputs = [
        Path(__file__),
        runner,
        source / "pyproject.toml",
        source / "icon.ico",
        source / "packaging" / "version_info.txt",
    ]
    inputs.extend(sorted((source / "proxy").glob("*.py")))
    for path in inputs:
        digest.update(path.as_posix().encode())
        digest.update(path.read_bytes())
    digest.update(sys.version.encode())
    return digest.hexdigest()


def venv_python(directory):
    return directory / "Scripts" / "python.exe"


def ensure_environment(directory):
    python = venv_python(directory)
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(directory)
    check = subprocess.run(
        [
            python,
            "-c",
            (
                "import PyInstaller, certifi, cryptography; "
                "raise SystemExit(not ("
                "certifi.__version__ == '2026.07.22' and "
                "cryptography.__version__ == '46.0.5' and "
                "PyInstaller.__version__ == '6.10.0'))"
            ),
        ],
        capture_output=True,
        text=True,
    )
    if check.returncode:
        subprocess.check_call([
            python,
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "certifi==2026.07.22",
            "cryptography==46.0.5",
            "pyinstaller==6.10.0",
        ])
    return python


def build(source, directory, output, runner):
    digest = source_digest(source, runner)
    stamp = directory / "stamp.json"
    if output.exists() and stamp.exists():
        data = json.loads(stamp.read_text(encoding="utf-8"))
        if data.get("digest") == digest:
            return

    directory.mkdir(parents=True, exist_ok=True)
    python = ensure_environment(directory / "venv")
    work = directory / "work"
    dist = directory / "dist"
    spec = directory / "spec"
    subprocess.check_call([
        python,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onefile",
        "--console",
        "--noupx",
        "--name",
        "AyuWsProxy",
        "--paths",
        source,
        "--collect-data",
        "certifi",
        "--exclude-module",
        "tkinter",
        "--exclude-module",
        "_tkinter",
        "--exclude-module",
        "PIL",
        "--exclude-module",
        "customtkinter",
        "--exclude-module",
        "pystray",
        "--hidden-import",
        "cryptography.hazmat.primitives.ciphers",
        "--hidden-import",
        "cryptography.hazmat.primitives.ciphers.algorithms",
        "--hidden-import",
        "cryptography.hazmat.primitives.ciphers.modes",
        "--hidden-import",
        "cryptography.hazmat.backends.openssl",
        "--icon",
        source / "icon.ico",
        "--version-file",
        source / "packaging" / "version_info.txt",
        "--workpath",
        work,
        "--distpath",
        dist,
        "--specpath",
        spec,
        runner,
    ])
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dist / "AyuWsProxy.exe", output)
    stamp.write_text(json.dumps({"digest": digest}), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    args = parser.parse_args()
    build(
        args.source.resolve(),
        args.build.resolve(),
        args.output.resolve(),
        args.runner.resolve(),
    )


if __name__ == "__main__":
    main()
