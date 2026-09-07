import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time


HOST = "127.0.0.1"
SECRET = "0123456789abcdef0123456789abcdef"


def unused_port():
    with socket.socket() as listener:
        listener.bind((HOST, 0))
        return listener.getsockname()[1]


def wait_for_listener(process, port):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Proxy exited during startup: {process.returncode}")
        try:
            return socket.create_connection((HOST, port), timeout=1)
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("Proxy did not listen within 30 seconds")


def stop_process_tree(process):
    if process is not None and process.poll() is None:
        result = subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW,
            timeout=10,
            check=False,
        )
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            result.check_returncode()
            raise


def run_scenario(executable, directory, fallback):
    port = unused_port()
    log_file = directory / "proxy.log"
    console_file = directory / "console.log"
    environment = os.environ.copy()
    environment.pop("PYTHONPATH", None)
    environment.pop("PYTHONHOME", None)
    environment["PYTHONNOUSERSITE"] = "1"
    try:
        with console_file.open("wb") as console, ExitStack() as cleanup:
            parent = subprocess.Popen(
                [sys.executable, "-I", "-c", "import sys; sys.stdin.buffer.read(1)"],
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=console,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            cleanup.callback(stop_process_tree, parent)
            cleanup.callback(parent.stdin.close)
            command = [
                str(executable),
                "--host", HOST,
                "--port", str(port),
                "--secret", SECRET,
                "--parent-pid", str(parent.pid),
                "--log-file", str(log_file),
                "--cfproxy-domain", "first.example.invalid",
                "--cfproxy-domain", "second.example.invalid",
                "--cfproxy-worker-domain", "worker.example.invalid",
                "--cfproxy-worker-domain", "other-worker.example.invalid",
            ]
            for dc in (1, 2, 3, 4, 5, 203):
                command.extend(["--dc-ip", f"{dc}:{HOST}"])
            if not fallback:
                command.append("--no-cfproxy")
            proxy = subprocess.Popen(
                command,
                cwd=directory,
                env=environment,
                stdout=console,
                stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            cleanup.callback(stop_process_tree, proxy)
            with wait_for_listener(proxy, port) as connection:
                connection.settimeout(5)
                connection.sendall(bytes(64))
                connection.shutdown(socket.SHUT_WR)
                try:
                    response = connection.recv(1)
                except ConnectionResetError:
                    response = b""
                if response:
                    raise RuntimeError("Proxy accepted an invalid handshake")
            with socket.create_connection((HOST, port), timeout=5):
                pass
            time.sleep(1)
            if proxy.poll() is not None:
                raise RuntimeError("Proxy stopped after an invalid handshake")
            parent.stdin.close()
            parent.wait(timeout=5)
            proxy.wait(timeout=10)
            if proxy.returncode != 0:
                raise RuntimeError(f"Proxy shutdown failed: {proxy.returncode}")
            with socket.socket() as connection:
                connection.settimeout(1)
                if connection.connect_ex((HOST, port)) == 0:
                    raise RuntimeError("Proxy listener survived parent shutdown")
        log = log_file.read_text(encoding="utf-8")
        console = console_file.read_text(encoding="utf-8", errors="replace")
        if not log.strip():
            raise RuntimeError("Proxy did not write its startup log")
        if SECRET in log or SECRET in console:
            raise RuntimeError("Proxy exposed its secret in a log")
        for marker in ("Traceback (most recent call last)", "ERROR", "CRITICAL"):
            if marker in log or marker in console:
                raise RuntimeError(f"Proxy logged a runtime failure: {marker}")
    except Exception:
        for path in (log_file, console_file):
            if path.exists():
                print(f"--- {path.name} ---", file=sys.stderr)
                print(
                    path.read_text(encoding="utf-8", errors="replace").replace(
                        SECRET, "<test-secret>"
                    ),
                    file=sys.stderr,
                )
        raise
    print(f"PASS: fallback={fallback}, listener, handshake rejection, logs, shutdown")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("The packaged proxy smoke test requires Windows")
    with tempfile.TemporaryDirectory(prefix="ayu proxy smoke ") as temporary:
        root = Path(temporary)
        executable = root / "AyuWsProxy.exe"
        shutil.copy2(args.executable.resolve(), executable)
        for fallback in (False, True):
            directory = root / f"fallback-{fallback}"
            directory.mkdir()
            run_scenario(executable, directory, fallback)


if __name__ == "__main__":
    main()
