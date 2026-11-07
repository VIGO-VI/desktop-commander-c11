#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 the Desktop Commander C contributors
"""Conformance test for the C11 Desktop Commander.

Runs the same tool calls against this build, and with --original against the
original DesktopCommanderMCP (node dist/index.js). It compares only the parts
that must agree: file contents, result sets, tool names and error decisions.
Status lines, timings, PIDs and session ids differ by design.

  python3 tests/conformance.py --binary ./dcm
  python3 tests/conformance.py --binary ./dcm \
      --original /path/to/DesktopCommanderMCP/dist/index.js
"""

import argparse
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

TOOL_CASES = []


def case(fn):
    TOOL_CASES.append(fn)
    return fn


class Client:
    def __init__(self, argv, env=None, cwd=None):
        self.argv = argv
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
            cwd=cwd,
        )
        self.buf = b""
        self.next_id = 0
        self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "conformance", "version": "1"},
            },
        )
        self.notify("notifications/initialized")

    def _readline(self, timeout):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no answer from the server")
            r, _, _ = select.select([self.proc.stdout], [], [], left)
            if not r:
                raise TimeoutError("no answer from the server")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("the server closed its output")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

    def request(self, method, params=None, timeout=60):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while True:
            line = self._readline(max(0.1, deadline - time.time()))
            if not line.strip():
                continue
            out = json.loads(line)
            if out.get("id") == self.next_id:
                if "error" in out:
                    raise RuntimeError(f"{method}: {out['error']}")
                return out["result"]

    def call(self, name, args, timeout=60):
        return self.request(
            "tools/call", {"name": name, "arguments": args}, timeout=timeout
        )

    def text(self, name, args, timeout=60):
        result = self.call(name, args, timeout=timeout)
        parts = []
        for block in result.get("content", []):
            if block.get("type") == "text":
                parts.append(block.get("text", ""))
        return "\n".join(parts), bool(result.get("isError"))

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def strip_status(text):
    """Removes a leading [Reading ...] line and keeps the payload."""
    if text.startswith("[Reading"):
        nl = text.find("\n\n")
        if nl >= 0:
            return text[nl + 2 :]
    return text


def fixture_tree(root):
    os.makedirs(os.path.join(root, "sub"), exist_ok=True)
    with open(os.path.join(root, "a.txt"), "w") as f:
        f.write("alpha\nbeta\ngamma\ndelta\n")
    with open(os.path.join(root, "sub", "b.txt"), "w") as f:
        f.write("needle here\nsecond needle\n")
    with open(os.path.join(root, "c.md"), "w") as f:
        f.write("# title\nneedle in markdown\n")


def rel_paths(text):
    """Pulls the file paths out of a search answer of either server."""
    out = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("[", "Pattern", "Path", "Status", "Runtime")):
            continue
        if line.startswith(("Started", "Total", "Initial results", "More results",
                            "Search", "Still", "Stopped", "✅", "📖", "⏱️", "⏹️")):
            continue
        for prefix in ("📄 ", "📁 ", "   "):
            if line.startswith(prefix):
                line = line[len(prefix) :]
                break
        if " - " in line:
            line = line.split(" - ", 1)[0]
        if " · " in line:
            continue
        out.add(os.path.basename(line.split(":")[0]))
    return out


def content_hits(text):
    hits = set()
    for line in text.splitlines():
        line = line.strip()
        if " - " in line and ":" in line:
            loc = line.split(" - ", 1)[0]
            path, _, num = loc.rpartition(":")
            if num.isdigit():
                hits.add((os.path.basename(path), int(num)))
    return hits


@case
def read_file_full(c, root):
    write(root, "read.txt", "one\ntwo\nthree\n")
    text, err = c.text("read_file", {"path": os.path.join(root, "read.txt")})
    assert not err, text
    return strip_status(text)


@case
def read_file_range(c, root):
    write(root, "range.txt", "l0\nl1\nl2\nl3\nl4\n")
    text, err = c.text(
        "read_file", {"path": os.path.join(root, "range.txt"), "offset": 1, "length": 2}
    )
    assert not err, text
    return strip_status(text)


@case
def read_file_tail(c, root):
    write(root, "tail.txt", "t0\nt1\nt2\nt3\n")
    text, err = c.text(
        "read_file", {"path": os.path.join(root, "tail.txt"), "offset": -2}
    )
    assert not err, text
    return strip_status(text)


@case
def write_file_new(c, root):
    path = os.path.join(root, "written.txt")
    text, err = c.text("write_file", {"path": path, "content": "a\nb\n"})
    assert not err, text
    return read(path)


@case
def write_file_append(c, root):
    path = os.path.join(root, "append.txt")
    c.text("write_file", {"path": path, "content": "a\n", "mode": "rewrite"})
    c.text("write_file", {"path": path, "content": "b\n", "mode": "append"})
    return read(path)


@case
def edit_block_replace(c, root):
    path = os.path.join(root, "edit.txt")
    write(root, "edit.txt", "keep\nchange me\nkeep\n")
    text, err = c.text(
        "edit_block",
        {"file_path": path, "old_string": "change me", "new_string": "changed"},
    )
    assert not err, text
    return read(path)


@case
def edit_block_mismatch(c, root):
    path = os.path.join(root, "edit2.txt")
    write(root, "edit2.txt", "x\nx\n")
    text, err = c.text(
        "edit_block",
        {
            "file_path": path,
            "old_string": "x",
            "new_string": "y",
            "expected_replacements": 1,
        },
    )
    assert not err, text
    return read(path)


@case
def edit_block_missing(c, root):
    path = os.path.join(root, "edit3.txt")
    write(root, "edit3.txt", "nothing like it here\n")
    text, err = c.text(
        "edit_block",
        {"file_path": path, "old_string": "zzz not present", "new_string": "y"},
    )
    assert not err, text
    return read(path)


@case
def list_directory(c, root):
    text, err = c.text("list_directory", {"path": root, "depth": 2})
    assert not err, text
    return sorted(line for line in text.splitlines() if line.strip())


@case
def get_file_info(c, root):
    path = os.path.join(root, "info.txt")
    write(root, "info.txt", "12345")
    text, err = c.text("get_file_info", {"path": path})
    assert not err, text
    info = {}
    for line in text.splitlines():
        if ":" in line:
            k, _, v = line.partition(":")
            info[k.strip()] = v.strip()
    return {"size": info.get("size"), "isFile": info.get("isFile")}


@case
def search_files(c, root):
    text, err = c.text(
        "start_search", {"path": root, "pattern": "*.txt", "searchType": "files"}
    )
    assert not err, text
    session = None
    for line in text.splitlines():
        if line.startswith("Started") and "session:" in line:
            session = line.rsplit(":", 1)[1].strip()
    assert session, text
    more, _ = collect(c, session, text)
    found = rel_paths(text) | rel_paths(more)
    assert "a.txt" in found, found
    assert "b.txt" in found, found
    assert "c.md" not in found, found
    assert all(name.endswith(".txt") for name in found), found
    return sorted(found)


@case
def search_content(c, root):
    text, err = c.text(
        "start_search",
        {"path": root, "pattern": "needle", "searchType": "content", "contextLines": 0},
    )
    assert not err, text
    session = None
    for line in text.splitlines():
        if line.startswith("Started") and "session:" in line:
            session = line.rsplit(":", 1)[1].strip()
    assert session, text
    more, _ = collect(c, session, text)
    hits = content_hits(text) | content_hits(more)
    expected = {("b.txt", 1), ("b.txt", 2), ("c.md", 2)}
    assert hits == expected, hits
    return sorted(hits)


@case
def process_echo(c, root):
    text, err = c.text(
        "start_process", {"command": "echo hello-from-test", "timeout_ms": 5000}
    )
    assert not err, text
    return "hello-from-test" in text


@case
def process_interact(c, root):
    text, err = c.text("start_process", {"command": "cat", "timeout_ms": 300})
    assert not err, text
    pid = extract_pid(text)
    if pid is None:
        raise RuntimeError("no PID in: " + text)
    out, err = c.text(
        "interact_with_process", {"pid": pid, "input": "ping", "timeout_ms": 2000}
    )
    c.text("force_terminate", {"pid": pid})
    return "ping" in out


@case
def process_blocked(c, root):
    text, err = c.text("start_process", {"command": "sudo ls", "timeout_ms": 1000})
    return err


@case
def config_shape(c, root):
    text, err = c.text("get_config", {})
    assert not err, text
    try:
        cfg = json.loads(text)
    except json.JSONDecodeError:
        return {"not_json": True}
    keys = {"blockedCommands", "defaultShell", "allowedDirectories",
            "fileReadLineLimit", "fileWriteLineLimit"}
    return sorted(k for k in keys if k in cfg)


def collect(client, session, first):
    """Reads result pages until the search says it is complete."""
    text = first
    for _ in range(20):
        more, err = client.text(
            "get_more_search_results", {"sessionId": session, "length": 100}
        )
        assert not err, more
        text += "\n" + more
        if "Search completed" in more or "No results at offset" in more:
            break
    return text, False


def extract_pid(text):
    for token in text.replace("(", " ").replace(")", " ").split():
        pass
    marker = "PID "
    idx = text.find(marker)
    if idx < 0:
        return None
    digits = ""
    for ch in text[idx + len(marker) :]:
        if ch.isdigit():
            digits += ch
        elif digits:
            break
    return int(digits) if digits else None


def write(root, name, content):
    path = os.path.join(root, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    return path


def read(path):
    with open(path) as f:
        return f.read()


def run_cases(client, root):
    results = {}
    for fn in TOOL_CASES:
        try:
            results[fn.__name__] = fn(client, root)
        except Exception as exc:  # noqa: BLE001 - each failure is one report line
            results[fn.__name__] = ("ERROR", str(exc)[:200])
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./dcm")
    ap.add_argument("--original", default=None,
                    help="path to the original build's dist/index.js")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="dcm-conformance-")
    failures = 0

    mine = Client([args.binary])
    try:
        tools = {t["name"] for t in mine.request("tools/list")["tools"]}
        root = os.path.join(tmp, "mine")
        os.makedirs(root)
        fixture_tree(root)
        mine_results = run_cases(mine, root)
    finally:
        mine.close()

    for name in [fn.__name__ for fn in TOOL_CASES]:
        value = mine_results[name]
        if isinstance(value, tuple) and value and value[0] == "ERROR":
            print(f"FAIL {name}: {value[1]}")
            failures += 1

    print(f"tools: {len(tools)}")

    if args.original:
        env = dict(os.environ)
        home = os.path.join(tmp, "home")
        os.makedirs(home)
        env.update({
            "HOME": home,
            "XDG_CONFIG_HOME": os.path.join(home, ".config"),
            "DESKTOP_COMMANDER_DISABLE_TELEMETRY": "1",
        })
        # --no-onboarding stops the original from opening its welcome page in
        # the browser. The test gives it a fresh HOME every run, so without the
        # flag every run looks like a new install and opens
        # https://desktopcommander.app/welcome/?utm_source=conformance on the
        # developer's machine.
        original = Client(
            ["node", args.original, "--no-onboarding"], env=env, cwd=tmp
        )
        try:
            other_tools = {t["name"] for t in original.request("tools/list")["tools"]}
            root = os.path.join(tmp, "orig")
            os.makedirs(root)
            fixture_tree(root)
            orig_results = run_cases(original, root)
        finally:
            original.close()

        print(f"original tools: {len(other_tools)}")
        missing = sorted(other_tools - tools)
        if missing:
            print(f"tools not reimplemented here: {', '.join(missing)}")
        extra = sorted(tools - other_tools)
        if extra:
            print(f"tools only here: {', '.join(extra)}")

        for name in [fn.__name__ for fn in TOOL_CASES]:
            a = mine_results[name]
            b = orig_results[name]
            if a == b:
                continue
            if isinstance(b, tuple) and b and "ripgrep binary not found" in str(b[1]):
                print(f"SKIP {name}: the original cannot search here (no ripgrep binary)")
                continue
            print(f"DIFF {name}:\n  c11:  {a!r}\n  orig: {b!r}")
            failures += 1

    mine_proc = mine.proc.poll()
    if mine_proc not in (0, None):
        print(f"FAIL the c11 server exited with {mine_proc}")
        failures += 1

    shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print(f"{failures} conformance check(s) failed")
        return 1
    print("conformance: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
