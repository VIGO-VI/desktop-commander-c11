#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
"""The two contracts of a process answer: the output is capped as head and tail
with the whole stream in a file, and a call that waits for output returns as
soon as output exists, newline or not.

A capped answer must not stop the reader: a command that prints more than the
cap still reaches its last line and exits 0, because the read end of its pipe
stays open until it ends.

  python3 tests/test_process.py ./dcm
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import time

ELISION = re.compile(r"\[\.\.\. (\d+) of (\d+) lines omitted; [^;]+ (\S+) \.\.\.\]")


def call(i, name, args):
    return {"jsonrpc": "2.0", "id": i, "method": "tools/call",
            "params": {"name": name, "arguments": args}}


class Server:
    def __init__(self, binary):
        self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 0
        self.request("initialize", {"protocolVersion": "2024-11-05"})
        self.notify("notifications/initialized")

    def notify(self, method):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params, timeout=60.0):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the server closed stdout")
        out = json.loads(line)
        if "error" in out:
            raise RuntimeError(f"{method}: {out['error']}")
        return out["result"]

    def tool(self, name, args):
        r = self.request("tools/call", {"name": name, "arguments": args})
        text = "\n".join(b.get("text", "") for b in r["content"] if b.get("type") == "text")
        return text, r.get("structuredContent") or {}, bool(r.get("isError"))

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def read_lines(path):
    with open(path, "r", errors="replace") as f:
        return f.read().splitlines()


def check_cap(server, failures):
    """The answer keeps the head and the tail, and the file keeps everything."""
    text, st, err = server.tool("start_process",
                                {"command": "seq 1 5000; echo last-line",
                                 "timeout_ms": 20000, "max_output_lines": 40})
    if err:
        failures.append(f"capped start failed: {text}")
        return
    lines = text.splitlines()
    if "1" not in lines or "last-line" not in lines:
        failures.append("the answer does not hold both ends of the output")
    if "2500" in text:
        failures.append("the answer holds the middle of a capped output")
    if st.get("maxOutputLines") != 40 or st.get("shownLines") != 40:
        failures.append(f"structuredContent counts are wrong: {st}")
    if st.get("outputLines") != 5001:
        failures.append(f"outputLines is {st.get('outputLines')}, expected 5001")
    if st.get("outputFile") != (ELISION.search(text).group(3) if ELISION.search(text) else None):
        failures.append("outputFile and the elision line name different files")
    m = ELISION.search(text)
    if not m:
        failures.append("no elision line: " + text[:200])
    else:
        if int(m.group(1)) != 5001 - 40 or int(m.group(2)) != 5001:
            failures.append(f"the elision line counts {m.group(1)} of {m.group(2)}")
        path = m.group(3)
        if not os.path.exists(path):
            failures.append(f"the output file {path} does not exist")
        else:
            whole = read_lines(path)
            if len(whole) != 5001:
                failures.append(f"the file holds {len(whole)} lines, expected 5001")
            if whole[0] != "1" or whole[-1] != "last-line":
                failures.append("the file does not start and end with the output")
    if "exit code 0" not in text:
        failures.append("the command did not exit 0: " + text[-120:])


def check_no_sigpipe(server, failures):
    """A cap on the answer never closes the pipe of the child."""
    text, st, err = server.tool("start_process",
                                {"command": "seq 1 200000; echo end-of-output",
                                 "timeout_ms": 30000, "max_output_lines": 20})
    if err:
        failures.append(f"flood start failed: {text[:200]}")
        return
    if st.get("status") != "finished" or "exit code 0" not in text:
        failures.append("a capped flood did not end with exit code 0: " + text[-160:])
    if "end-of-output" not in text:
        failures.append("the last line of a capped flood is missing")
    path = st.get("outputFile", "")
    if not path or not os.path.exists(path):
        failures.append("a capped flood wrote no output file")
    else:
        whole = read_lines(path)
        if len(whole) != 200001 or whole[-1] != "end-of-output":
            failures.append(f"the flood file holds {len(whole)} lines, last {whole[-1:]}")


def check_cap_argument(server, failures):
    """max_output_lines overrides the default, and a negative value keeps all."""
    text, st, err = server.tool("start_process",
                                {"command": "seq 1 10", "timeout_ms": 5000,
                                 "max_output_lines": -1})
    if err or st.get("shownLines") != 10 or st.get("outputLines") != 10:
        failures.append(f"max_output_lines -1 did not keep all 10 lines: {st} {text[:120]}")
    text, st, err = server.tool("start_process",
                                {"command": "seq 1 10", "timeout_ms": 5000,
                                 "max_output_lines": 0})
    if err or st.get("shownLines") != 0 or st.get("outputLines") != 10:
        failures.append(f"max_output_lines 0 showed {st.get('shownLines')} lines")


def check_partial_wait(server, failures):
    """A prompt with no newline wakes the wait instead of holding it.

    The line arrives after the start call and the process stays alive after
    it, so neither the start nor the end of the process can produce it.
    """
    text, st, err = server.tool(
        "start_process",
        {"command": "sleep 0.5; printf 'second'; sleep 10", "timeout_ms": 100})
    if err:
        failures.append(f"partial start failed: {text}")
        return
    pid = st.get("pid")
    if "second" in text:
        failures.append("the tail arrived before the start call returned: " + repr(text))

    started = time.monotonic()
    text, st, err = server.tool("read_process_output", {"pid": pid, "timeout_ms": 2500})
    waited = time.monotonic() - started
    if "second" not in text:
        failures.append("the wait did not return the line without a newline: " + repr(text))
    elif waited > 1.8:
        failures.append(f"the wait held for {waited:.2f}s on output it already had")

    text, st, err = server.tool("read_process_output", {"pid": pid, "timeout_ms": 300})
    if "second" in text:
        failures.append("the same tail came back twice")
    server.tool("force_terminate", {"pid": pid})


def check_prompt_wait(server, failures):
    """interact_with_process returns the prompt of a REPL that has no newline."""
    text, st, err = server.tool("start_process",
                                {"command": "printf 'name? '; read answer; echo got:$answer",
                                 "timeout_ms": 100})
    if err:
        failures.append(f"prompt start failed: {text}")
        return
    pid = st.get("pid")
    started = time.monotonic()
    text, st, err = server.tool("interact_with_process",
                                {"pid": pid, "input": "ping", "timeout_ms": 5000})
    waited = time.monotonic() - started
    if "got:ping" not in text:
        failures.append("the REPL answer is missing: " + repr(text))
    if waited > 2.0:
        failures.append(f"interact held for {waited:.2f}s")
    server.tool("force_terminate", {"pid": pid})


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./dcm"
    server = Server(binary)
    failures = []
    try:
        check_cap(server, failures)
        check_no_sigpipe(server, failures)
        check_cap_argument(server, failures)
        check_partial_wait(server, failures)
        check_prompt_wait(server, failures)
    finally:
        server.close()

    if failures:
        for f in failures:
            print(f"FAIL process: {f}")
        return 1
    print("process test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
