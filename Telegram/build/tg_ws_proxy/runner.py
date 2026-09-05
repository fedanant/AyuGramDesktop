import argparse
import asyncio
import ctypes
import logging
import logging.handlers
import os
import sys
import threading

from proxy.config import coerce_domain_list, parse_dc_ip_list, proxy_config
from proxy.tg_ws_proxy import _run


class SecretRedactionFilter(logging.Filter):
    def __init__(self, secret):
        super().__init__()
        self._secret = secret

    def filter(self, record):
        record.msg = self._redact(record.msg)
        if isinstance(record.args, tuple):
            record.args = tuple(self._redact(value) for value in record.args)
        elif isinstance(record.args, dict):
            record.args = {
                key: self._redact(value)
                for key, value in record.args.items()
            }
        return True

    def _redact(self, value):
        return (
            value.replace(self._secret, "<redacted>")
            if isinstance(value, str)
            else value
        )


def exit_when_process_stops(pid):
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_uint32,
    ]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel32.OpenProcess(0x00100000, False, pid)
    if not handle:
        return
    kernel32.WaitForSingleObject(handle, 0xFFFFFFFF)
    kernel32.CloseHandle(handle)
    os._exit(0)


def watch_processes(parent_pid):
    if sys.platform != "win32":
        return
    for pid in {parent_pid, os.getppid()}:
        threading.Thread(
            target=exit_when_process_stops,
            args=(pid,),
            daemon=True,
        ).start()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--secret", required=True)
    parser.add_argument("--parent-pid", required=True, type=int)
    parser.add_argument("--log-file", required=True)
    parser.add_argument("--dc-ip", action="append", default=[])
    parser.add_argument("--cfproxy-domain", action="append", default=[])
    parser.add_argument("--cfproxy-worker-domain", action="append", default=[])
    parser.add_argument("--no-cfproxy", action="store_true")
    args = parser.parse_args()
    watch_processes(args.parent_pid)

    proxy_config.host = args.host
    proxy_config.port = args.port
    proxy_config.secret = args.secret
    dc_ip = args.dc_ip or [
        "2:149.154.167.220",
        "4:149.154.167.220",
    ]
    proxy_config.dc_redirects = parse_dc_ip_list(dc_ip)
    proxy_config.buffer_size = 256 * 1024
    proxy_config.pool_size = 4
    proxy_config.fallback_cfproxy = not args.no_cfproxy
    proxy_config.cfproxy_user_domains = coerce_domain_list(
        args.cfproxy_domain
    )
    proxy_config.cfproxy_worker_domains = coerce_domain_list(
        args.cfproxy_worker_domain
    )
    proxy_config.fake_tls_domain = ""
    proxy_config.proxy_protocol = False
    proxy_config.force_test_dc = False

    root = logging.getLogger()
    root.setLevel(logging.INFO)
    handler = logging.handlers.RotatingFileHandler(
        args.log_file,
        maxBytes=5 * 1024 * 1024,
        backupCount=1,
        encoding="utf-8",
    )
    handler.setFormatter(logging.Formatter(
        "%(asctime)s  %(levelname)-5s  %(message)s",
        datefmt="%H:%M:%S",
    ))
    handler.addFilter(SecretRedactionFilter(args.secret))
    root.addHandler(handler)
    logging.getLogger("asyncio").setLevel(logging.WARNING)

    asyncio.run(_run())


if __name__ == "__main__":
    main()
