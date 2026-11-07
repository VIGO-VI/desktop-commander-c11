#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
"""Speaks the protocol the way a busy client does: several requests on one
write, a blocking call among them, and a check that the answers overlap.

The server must keep 32 requests in flight, run them on its worker threads, and
answer each with the id it was given. This test is not compared with the
original; it is about this build's own contract.
"""

import json
import os
import subprocess
import sys
import tempfile
import time


def send(proc, messages):
    for m in messages:
        proc.stdin.write(json.dumps(m) + "\n")
    proc.stdin.flush()


def read_one(proc, timeout=30.0):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError("the server closed stdout")
    return json.loads(line)


def call(i, name, args):
    return {"jsonrpc": "2.0", "id": i, "method": "tools/call",
            "params": {"name": name, "arguments": args}}


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./dcm"
    tmp = tempfile.mkdtemp(prefix="dcm-pool-")
    path = os.path.join(tmp, "f.txt")
    with open(path, "w") as f:
        f.write("hello\n")

    proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, bufsize=1)
    failures = []
    try:
        send(proc, [{"jsonrpc": "2.0", "id": 0, "method": "initialize",
                     "params": {"protocolVersion": "2024-11-05"}}])
        read_one(proc)

        # 31 quick reads and one call that waits a second: the quick ones must
        # not queue behind it.
        messages = [call(1000 + i, "read_file", {"path": path}) for i in range(31)]
        messages.append(call(2000, "start_process",
                             {"command": "sleep 1; echo done", "timeout_ms": 1500}))
        messages.append(call(3000, "ping", {}))
        start = time.monotonic()
        send(proc, messages)

        slow_done = None
        seen = set()
        for _ in range(len(messages)):
            msg = read_one(proc, timeout=30)
            seen.add(msg.get("id"))
            if msg.get("id") == 2000:
                slow_done = time.monotonic() - start
            if msg.get("id") == 1000 and not slow_done:
                # The first fast answer must arrive while the slow call runs.
                quick = time.monotonic() - start
                if quick > 0.8:
                    failures.append(f"a quick call waited {quick:.2f}s behind a blocking one")
        if len(seen) != len(messages):
            failures.append(f"answered {len(seen)} of {len(messages)} requests")
        if slow_done is None:
            failures.append("the blocking call produced no answer")

        # All 32 slots at once, each a blocking call that ends quickly.
        batch = [call(4000 + i, "start_process",
                      {"command": "sleep 0.2", "timeout_ms": 400}) for i in range(32)]
        send(proc, batch)
        ids = {read_one(proc, timeout=30).get("id") for _ in range(len(batch))}
        if ids != {4000 + i for i in range(len(batch))}:
            failures.append("a batch of 32 concurrent calls lost an answer")

        # A finished session gives its slot back. More short processes than
        # there are slots must all start, one after another.
        time.sleep(0.5)
        reused = 0
        for i in range(36):
            send(proc, [call(5000 + i, "start_process",
                             {"command": "true", "timeout_ms": 500})])
            msg = read_one(proc, timeout=30)
            text = "".join(b.get("text", "") for b in msg.get("result", {}).get("content", []))
            if "No free process session" in text:
                reused += 1
        if reused:
            failures.append(f"{reused} short processes could not take a finished slot")
    finally:
        proc.stdin.close()
        proc.wait(timeout=30)

    if failures:
        for f in failures:
            print(f"FAIL pool: {f}")
        return 1
    print("pool test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
