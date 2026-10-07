"""Exercise the benchmark client against an isolated CacheLite process."""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for_server(port, process):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError("server exited before accepting connections")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def run_benchmark(script, port, mode, keyspace):
    result = subprocess.run(
        [
            sys.executable, str(script),
            "--host", "127.0.0.1", "--port", str(port),
            "--command", mode, "--key", "smoke:key",
            "--keyspace", str(keyspace), "--clients", "8",
            "--requests", "1024", "--pipeline", "4",
        ],
        capture_output=True, text=True, timeout=30, check=False,
    )
    if result.returncode != 0 or "completed:     1024" not in result.stdout \
            or "errors:        0" not in result.stdout:
        raise AssertionError(
            f"{mode} benchmark failed:\n{result.stdout}\n{result.stderr}"
        )


def main():
    server_path = Path(sys.argv[1]).resolve()
    script_path = Path(sys.argv[2]).resolve()
    port = free_port()
    environment = os.environ.copy()
    environment["CACHELITE_PORT"] = str(port)
    environment["CACHELITE_AOF_POLICY"] = "no"

    with tempfile.TemporaryDirectory(prefix="cachelite-benchmark-") as workdir:
        server = subprocess.Popen(
            [str(server_path)], cwd=workdir, env=environment,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            wait_for_server(port, server)
            run_benchmark(script_path, port, "get", 128)
            run_benchmark(script_path, port, "set-get", 128)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)


if __name__ == "__main__":
    main()
