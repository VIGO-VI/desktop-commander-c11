# Parity with the original

The original has 25 tools. This build has 23. Two are missing on purpose:

| Original tool | Here | Why |
| --- | --- | --- |
| `write_pdf` | absent | A PDF renderer (Chromium, markdown, HTML, SVG) is a project of its own. |
| `give_feedback_to_desktop_commander` | absent | It opens a feedback form that uploads data. There is no telemetry. |

Everything else keeps its name, its arguments and the shape of its answer.

## Tools

| Tool | State | Notes |
| --- | --- | --- |
| `get_config` | same | Same keys. `telemetryEnabled` is replaced by `telemetry: "none"`. |
| `set_config_value` | same | Same keys; `telemetryEnabled` is refused with a message. |
| `read_file` | same | Text, images (PNG/JPEG/GIF/WebP), offset (negative = tail), length. No URL, PDF, Excel or DOCX. |
| `read_multiple_files` | same | Text and image blocks; a failed file does not stop the others. |
| `write_file` | same | `rewrite` and `append`; an existing non-empty file needs an explicit mode. |
| `create_directory` | same | Creates parents. |
| `list_directory` | same | `[DIR]`/`[FILE]`, depth, the 100-item cap for nested folders. |
| `move_file` | same | `rename(2)` across the same file system. |
| `get_file_info` | same | Size, times, permissions, type. Dates are ISO 8601. |
| `edit_block` | same effect | Exact replace with `expected_replacements`; CRLF is kept. The fuzzy report is a similarity score and the closest text, not the original's per-character diff. |
| `start_search` | same | Session id, background walk, `files` and `content`, glob or POSIX ERE, context lines. |
| `get_more_search_results` | same | `offset`/`length` paging. |
| `stop_search` | same | |
| `list_searches` | same | |
| `start_process` | same | Shell session, `timeout_ms` wait, initial output, `structuredContent.pid`. |
| `read_process_output` | same | `offset` 0 = new lines, positive = absolute, negative = tail. |
| `interact_with_process` | same | Writes the input with a newline and returns the new output. |
| `force_terminate` | same | Signals the whole session group, then SIGKILL. |
| `list_sessions` | same | |
| `list_processes` | same shape | Reads `/proc` on Linux; `ps` elsewhere. |
| `kill_process` | same | Refuses the server's own PID. |
| `get_usage_stats` | local only | Counts calls in memory. Nothing leaves the machine. |
| `get_recent_tool_calls` | local only | In-memory ring of 200 calls. |

## Deliberate differences

- Messages are one line where the original writes a paragraph. The prompts the
  model reads are 10 to 40 times shorter, which is the point of the request.
- Search does not use ripgrep. The walker is in the binary, so the server starts
  without a download and works on a machine with no ripgrep. `.gitignore` is not
  parsed; `includeHidden` and `filePattern` cover the common cases.
- `list_processes` reports `CPU: n/a` and the process state, not `ps aux` CPU
  and memory columns. It is one pass over `/proc` with no fork.
- A process session merges stdout and stderr into one stream, as the original's
  output buffer does.

## Conformance result

`tests/conformance.py` runs 16 cases against both servers and compares file
contents, result sets, tool names and blocklist decisions.

```
$ python3 tests/conformance.py --binary ./dcm --original .../dist/index.js
tools: 23
original tools: 25
tools not reimplemented here: give_feedback_to_desktop_commander, write_pdf
SKIP search_files: the original cannot search here (no ripgrep binary)
SKIP search_content: the original cannot search here (no ripgrep binary)
conformance: ok
```

On a machine with ripgrep installed, all 16 cases match. In the CI job, which
installs ripgrep and builds the original, the run prints no `SKIP` and no
`DIFF` line. Without ripgrep the original cannot search, so the two search
cases are skipped there and our results are checked against fixed
expectations in the fixture instead.

The same run passes under `-fsanitize=address,undefined` on macOS.

The CI runs on Ubuntu with gcc and clang: both build the server, run the unit
tests, run the 16 cases, and the `compare` job runs all 16 against the
original.
