#!/usr/bin/env python3
"""Small RESP2 benchmark client for CacheLite.

It intentionally uses only Python's standard library so it can run in a
minimal WSL installation.  Latency is measured after all client sockets have
connected, so connection setup is not included in RT.
"""

from __future__ import annotations

import argparse
import socket
import statistics
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import BinaryIO


def bulk(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return b"$" + str(len(encoded)).encode() + b"\r\n" + encoded + b"\r\n"


def command(name: str, *arguments: str) -> bytes:
    parts = (name,) + arguments
    return b"*" + str(len(parts)).encode() + b"\r\n" + b"".join(
        bulk(part) for part in parts
    )


def read_exact(reader: BinaryIO, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        chunk = reader.read(length - len(result))
        if not chunk:
            raise ConnectionError("server closed the connection")
        result.extend(chunk)
    return bytes(result)


def read_line(reader: BinaryIO) -> bytes:
    result = bytearray()
    while True:
        byte = read_exact(reader, 1)
        result.extend(byte)
        if result.endswith(b"\r\n"):
            return bytes(result[:-2])


def read_resp(reader: BinaryIO) -> bytes:
    prefix = read_exact(reader, 1)
    if prefix in (b"+", b"-", b":"):
        return prefix + read_line(reader) + b"\r\n"
    if prefix == b"$":
        length = int(read_line(reader))
        if length == -1:
            return b"$-1\r\n"
        payload = read_exact(reader, length + 2)
        if not payload.endswith(b"\r\n"):
            raise ValueError("invalid bulk-string terminator")
        return prefix + str(length).encode() + b"\r\n" + payload
    raise ValueError(f"unsupported RESP response prefix: {prefix!r}")


@dataclass
class WorkerResult:
    completed_operations: int
    completed_commands: int
    errors: int
    latencies_ms: list[float]


def run_worker(
    host: str,
    port: int,
    requests: int,
    pipeline: int,
    request: bytes,
    responses_per_operation: int,
    timeout: float,
) -> WorkerResult:
    completed_operations = 0
    completed_commands = 0
    errors = 0
    latencies: list[float] = []

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with sock.makefile("rb") as reader:
            remaining = requests
            while remaining:
                batch_size = min(pipeline, remaining)
                payload = request * batch_size
                expected_responses = batch_size * responses_per_operation
                started = time.perf_counter_ns()
                try:
                    sock.sendall(payload)
                    responses_read = 0
                    batch_errors = 0
                    for _ in range(expected_responses):
                        response = read_resp(reader)
                        if response.startswith(b"-"):
                            batch_errors += 1
                        responses_read += 1
                    completed_commands += responses_read - batch_errors
                    errors += batch_errors
                    # With pipelining this is the batch round-trip time. For
                    # pipeline=1 it is the operation round-trip time.
                    batch_latency = (time.perf_counter_ns() - started) / 1e6
                    if responses_read == expected_responses and batch_errors == 0:
                        completed_operations += batch_size
                        latencies.extend([batch_latency] * batch_size)
                except (OSError, ValueError, ConnectionError):
                    completed_commands += responses_read - batch_errors
                    errors += batch_errors + expected_responses - responses_read
                    break
                remaining -= batch_size

    return WorkerResult(
        completed_operations,
        completed_commands,
        errors,
        latencies,
    )


def percentile(values: list[float], percentage: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentage / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="CacheLite RESP2 benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--clients", type=int, default=100)
    parser.add_argument("--requests", type=int, default=100_000)
    parser.add_argument("--pipeline", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--command",
        choices=("ping", "set", "get", "set-get"),
        default="ping",
        help="command or operation to benchmark",
    )
    parser.add_argument("--key", default="bench:key")
    parser.add_argument("--value", default="bench:value")
    args = parser.parse_args()
    if args.clients < 1 or args.requests < 1 or args.pipeline < 1:
        parser.error("clients, requests and pipeline must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.command == "ping":
        request = command("PING")
        responses_per_operation = 1
    elif args.command == "set":
        request = command("SET", args.key, args.value)
        responses_per_operation = 1
    elif args.command == "get":
        request = command("GET", args.key)
        responses_per_operation = 1
    else:
        request = command("SET", args.key, args.value) + command(
            "GET", args.key
        )
        responses_per_operation = 2

    client_count = min(args.clients, args.requests)
    request_counts = [args.requests // client_count] * client_count
    for index in range(args.requests % client_count):
        request_counts[index] += 1

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=client_count) as executor:
        futures = [
            executor.submit(
                run_worker,
                args.host,
                args.port,
                count,
                args.pipeline,
                request,
                responses_per_operation,
                args.timeout,
            )
            for count in request_counts
        ]
        results = [future.result() for future in futures]
    elapsed = time.perf_counter() - started

    completed_operations = sum(
        result.completed_operations for result in results
    )
    completed_commands = sum(
        result.completed_commands for result in results
    )
    errors = sum(result.errors for result in results)
    latencies = [
        latency
        for result in results
        for latency in result.latencies_ms
    ]
    operations_per_second = (
        completed_operations / elapsed if elapsed > 0 else 0.0
    )
    commands_per_second = (
        completed_commands / elapsed if elapsed > 0 else 0.0
    )

    print(f"command:       {args.command.upper()}")
    print(f"clients:       {client_count}")
    print(f"pipeline:      {args.pipeline}")
    print(f"completed:     {completed_operations}")
    if responses_per_operation > 1:
        print(f"commands:      {completed_commands}")
    print(f"errors:        {errors}")
    print(f"elapsed:       {elapsed:.3f} s")
    if responses_per_operation > 1:
        print(f"TPS:           {operations_per_second:.2f}")
        print(f"command QPS:   {commands_per_second:.2f}")
    else:
        print(f"QPS:           {commands_per_second:.2f}")
    if latencies:
        print(f"RT average:    {statistics.fmean(latencies):.3f} ms")
        print(f"RT P50:        {percentile(latencies, 50):.3f} ms")
        print(f"RT P95:        {percentile(latencies, 95):.3f} ms")
        print(f"RT P99:        {percentile(latencies, 99):.3f} ms")
        print(f"RT max:        {max(latencies):.3f} ms")
        if args.pipeline > 1:
            print("note: pipeline > 1 reports batch round-trip time")
    return 0 if errors == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
