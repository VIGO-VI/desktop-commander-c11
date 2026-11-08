#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
"""The wire contract and the file contract of the server.

The wire: a notification is never answered, a request without an id is one, a
request whose id cannot be a JSON-RPC id gets -32600, every line of stdout is
valid JSON, and the protocol version is one this build speaks. The files: a
write keeps the mode of the file it replaces, a string with a NUL keeps all its
bytes, a file without a final newline keeps its last line, and a search row
longer than the row buffer does not read past it.

  python3 tests/test_protocol.py ./dcm
"""

import json
import os
import stat
import subprocess
import sys
import tempfile


class Rpc:
    """One server, line by line, without waiting for a reply on a notification."""

    def __init__(self, binary):
        self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def send_raw(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def reply(self, timeout=60.0):
        """The next line, or None when the server says nothing within timeout."""
        # readline has no timeout; a reply that never comes shows up as an empty
        # read only after the server exits, so tests that expect silence send a
        # ping afterwards and look at what came first.
        line = self.proc.stdout.readline()
        return line if line else None

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def tool(self, name, args):
        self.send({"jsonrpc": "2.0", "id": 99, "method": "tools/call",
                   "params": {"name": name, "arguments": args}})
        line = self.reply()
        if not line:
            raise RuntimeError(f"{name}: the server closed stdout")
        out = json.loads(line)
        if "error" in out:
            raise RuntimeError(f"{name}: {out['error']}")
        r = out["result"]
        text = "\n".join(b.get("text", "") for b in r["content"] if b.get("type") == "text")
        return text, r.get("structuredContent") or {}, bool(r.get("isError"))


def check_silence(binary, failures):
    """A message with no id is a notification, and a notification has no reply."""
    proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    lines = [
        '{"jsonrpc":"2.0","method":"notifications/initialized"}',
        '{"jsonrpc":"2.0","method":"notifications/roots/list_changed"}',
        '{"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":1}}',
        '{"jsonrpc":"2.0","method":"tools/list"}',  # a request without an id
        '{"jsonrpc":"2.0","id":1,"method":"ping"}',
    ]
    out, _ = proc.communicate("\n".join(lines) + "\n", timeout=30)
    replies = [l for l in out.splitlines() if l.strip()]
    if len(replies) != 1:
        failures.append(f"a notification was answered: {len(replies)} replies")
        return
    try:
        msg = json.loads(replies[0])
    except ValueError as e:
        failures.append(f"the answer is not JSON: {e}")
        return
    if msg.get("id") != 1 or msg.get("result") != {}:
        failures.append(f"the ping answer is wrong: {replies[0][:120]}")


def check_ids(binary, failures):
    """An id that JSON-RPC does not allow, and a message that is not an object."""
    proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    lines = [
        '{"jsonrpc":"2.0","id":null,"method":"ping"}',
        '{"jsonrpc":"2.0","id":true,"method":"ping"}',
        '{"jsonrpc":"2.0","id":1e999,"method":"ping"}',
        '"a bare string"',
        '[]',
        '{"jsonrpc":"2.0","id":7,"method":"ping"}',
    ]
    out, _ = proc.communicate("\n".join(lines) + "\n", timeout=30)
    replies = [l for l in out.splitlines() if l.strip()]
    if len(replies) != 6:
        failures.append(f"expected 6 replies, got {len(replies)}: {out[:200]}")
        return
    # Workers run at once, so a reply carries its id and not a place in the queue.
    errors = 0
    answered = None
    for r in replies:
        try:
            msg = json.loads(r)
        except ValueError:
            failures.append(f"a line is not valid JSON: {r[:120]}")
            continue
        if msg.get("id") == 7:
            answered = msg
        elif msg.get("error", {}).get("code") == -32600:
            errors += 1
        else:
            failures.append(f"an invalid request was not refused: {r[:120]}")
    if errors != 5:
        failures.append(f"the invalid requests did not get -32600: {errors} of 5")
    if not answered or answered.get("result") != {}:
        failures.append("the valid request after them was not answered")


def check_version(binary, failures):
    """The version of the answer is one this build speaks."""
    def init(asked):
        msg = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
               "params": {"protocolVersion": asked, "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}}}
        out = subprocess.run([binary], input=json.dumps(msg) + "\n",
                             capture_output=True, text=True, timeout=30).stdout
        return json.loads(out.splitlines()[0])["result"]["protocolVersion"]

    if init("2024-11-05") != "2024-11-05":
        failures.append("a supported version was not echoed")
    if init("banana") != "2025-06-18":
        failures.append("an unknown version did not get the newest supported one")


def check_batch(binary, failures):
    """A batch answers each request and nothing for its notification."""
    batch = [
        {"jsonrpc": "2.0", "id": 1, "method": "ping"},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": "b", "method": "ping"},
    ]
    out = subprocess.run([binary], input=json.dumps(batch) + "\n",
                         capture_output=True, text=True, timeout=30).stdout
    replies = [json.loads(l) for l in out.splitlines() if l.strip()]
    ids = sorted((str(r.get("id")) for r in replies), key=str)
    if ids != ["1", "b"] or len(replies) != 2:
        failures.append(f"the batch answers are wrong: {out[:200]}")


def check_file_contract(binary, failures):
    tmp = tempfile.mkdtemp(prefix="dcm-proto-")
    srv = Rpc(binary)
    try:
        # A mode of its own survives a rewrite and an edit.
        script = os.path.join(tmp, "script.sh")
        with open(script, "w") as f:
            f.write("#!/bin/sh\necho hi\n")
        os.chmod(script, 0o755)
        _, _, err = srv.tool("edit_block", {"file_path": script,
                                            "old_string": "hi", "new_string": "bye"})
        if err or stat.S_IMODE(os.stat(script).st_mode) != 0o755:
            failures.append("edit_block cleared the mode of the file")
        _, _, err = srv.tool("write_file", {"path": script, "content": "x\n",
                                            "mode": "rewrite"})
        if err or stat.S_IMODE(os.stat(script).st_mode) != 0o755:
            failures.append("write_file cleared the mode of the file")

        # A NUL in the content is a byte of the file, not its end.
        nul = os.path.join(tmp, "nul.bin")
        srv.tool("write_file", {"path": nul, "content": "aaa\u0000bbb", "mode": "rewrite"})
        with open(nul, "rb") as f:
            if f.read() != b"aaa\x00bbb":
                failures.append("write_file stopped at a NUL byte")

        # A file without a final newline keeps its last line whole.
        no_nl = os.path.join(tmp, "nonl.txt")
        with open(no_nl, "w") as f:
            f.write("one\ntwo\nthree")
        text, _, _ = srv.tool("read_file", {"path": no_nl})
        if not text.endswith("one\ntwo\nthree"):
            failures.append("read_file truncated a file with no final newline: " + repr(text[-40:]))

        # A row longer than the row buffer is kept whole and does not crash.
        deep = tmp
        for _ in range(4):
            deep = os.path.join(deep, "d" * 70)
        os.makedirs(deep)
        with open(os.path.join(deep, "f.txt"), "w") as f:
            for _ in range(3):
                f.write("needle " + "z" * 800 + "\n")
        text, _, err = srv.tool("start_search", {"path": tmp, "pattern": "needle",
                                                 "searchType": "content",
                                                 "contextLines": 0})
        if err or "3 matches" not in text:
            failures.append("a long search row was lost: " + text[:120])

        # A pattern that does not compile is refused, not read as a substring.
        text, _, err = srv.tool("start_search", {"path": tmp, "pattern": "[unclosed",
                                                 "searchType": "content"})
        if not err:
            failures.append("an invalid pattern was accepted")
    finally:
        srv.close()


def check_child_fds(binary, failures):
    """A later session inherits none of an earlier session's pipe ends."""
    srv = Rpc(binary)
    try:
        first, st, _ = srv.tool("start_process", {"command": "sleep 30", "timeout_ms": 500})
        pid = st.get("pid")
        cmd = "ls -l /dev/fd 2>/dev/null || ls -l /proc/self/fd"
        text, _, _ = srv.tool("start_process", {"command": cmd, "timeout_ms": 3000})
        inherited = [l for l in text.splitlines() if l.startswith("p") and len(l) > 3]
        # The child's own 0, 1 and 2 are pipes. Anything more is another session's.
        extra = [l for l in inherited[3:]]
        if extra:
            failures.append(f"a session inherited {len(extra)} pipe end(s): {extra[:2]}")
        if pid:
            srv.tool("force_terminate", {"pid": pid})
    finally:
        srv.close()


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./dcm"
    failures = []
    check_silence(binary, failures)
    check_ids(binary, failures)
    check_version(binary, failures)
    check_batch(binary, failures)
    check_file_contract(binary, failures)
    check_child_fds(binary, failures)
    if failures:
        for f in failures:
            print(f"FAIL protocol: {f}")
        return 1
    print("protocol test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
