# Desktop Commander, rewritten in C11

An MCP server for files, searches and long-running terminal sessions. It is a
rewrite of [DesktopCommanderMCP](https://github.com/wonderwhy-er/DesktopCommanderMCP)
in C11 for Linux, with no Node.js, no runtime dependencies and no telemetry.

- One binary, configured at build time. Only the C library and pthreads.
- Same tool names and arguments as the original, so an existing client works.
- Same result text and file effects for the tools it covers. The conformance
  test runs both servers side by side and compares them.
- No network code at all: no telemetry, no update check, no remote device.

```
$ make
$ ./dcm          # speaks MCP on stdin/stdout
```

## Why the rewrite

The original is 15,000 lines of TypeScript and needs Node.js 22, 1,043 npm
packages and a ripgrep download. Its first request sends telemetry events, and
it starts a background updater. This build is one process, one file per tool
family, and the only things it writes are the files a tool was asked to write.

## Size and memory

The layout is data oriented: a request is an arena of nodes addressed by 4-byte
indices, sessions are slots of static arrays, and output buffers are rings that
are written once and never moved. Nothing is allocated for each JSON node, each
line of output or each search row, and a completed request hands its arena back
for the next one.

| Measure | This build | The previous build of this rewrite |
| --- | --- | --- |
| Binary, default `make`, macOS arm64 | 118 KB | 138 KB |
| Code section | 38 KB | 51 KB |
| Peak RSS, one process that prints 20 MB | 21 MB | 100 MB |
| Peak RSS, a content search with 50,000 results | 3 MB | 47 MB |

A 32-bit build (`make EXTRA=-m32`) uses the same handles of 4 bytes and the same
`size_t` bounds, so a document, a session slot and a search row cost less there,
not more.

## What it does

| Area | Tools |
| --- | --- |
| Files | `read_file`, `read_multiple_files`, `write_file`, `create_directory`, `list_directory`, `move_file`, `get_file_info` |
| Edit | `edit_block` |
| Search | `start_search`, `get_more_search_results`, `stop_search`, `list_searches` |
| Processes | `start_process`, `read_process_output`, `interact_with_process`, `force_terminate`, `list_sessions`, `list_processes`, `kill_process` |
| Config | `get_config`, `set_config_value` |
| Local history | `get_usage_stats`, `get_recent_tool_calls` |

A process session is a child in its own session group with one output pipe, a
reader thread and a reaper. `start_process` collects the first output, and the
session stays alive for `interact_with_process` and `read_process_output`:
REPLs, SSH, database clients and servers all work. `force_terminate` ends the
group, not just the shell. Output is kept in a ring of `DCM_SESSION_BYTES`, and
the oldest lines go first, so a process that floods costs a fixed amount.

A search is a session too. `start_search` returns at once, a thread walks the
tree, and `get_more_search_results` pages the rows. Patterns are POSIX extended
regular expressions, or literal strings with `literalSearch`.

The reader takes 32 requests before it waits (`DCM_MAX_INFLIGHT`) and runs them
on 8 worker threads (`DCM_WORKERS`), so the calls of two or three agents do not
queue behind a command that waits on a prompt. Each worker keeps its JSON
document and its output buffer between requests.

## Build and test

```sh
make                # builds ./dcm, -Oz or -Os
make check          # unit tests, the 32-request pool test, the conformance run
make debug          # the same with debug information
make config         # writes src/config.h from src/config.def.h, once
make install        # /usr/local/bin/dcm
```

The build is C11 with `-Werror`, strict aliasing on, and no `-fwrapv`; arithmetic
that can wrap is done in unsigned types. `-Wstrict-aliasing=2` is part of the
flags, and a pointer is never used to read an object of another type.

`make check` runs `tests/test_json`, `tests/test_util`,
`tests/test_pool.py --binary ./dcm` and `tests/conformance.py --binary ./dcm`.
The pool test sends 32 requests at once, one of them blocking, and checks that
the others answer while it runs.

For a special target:

```sh
make EXTRA="-m32"                                   # a 32-bit client
make EXTRA="-fsanitize=address,undefined" EXTRA_LDFLAGS="-fsanitize=address,undefined"
```

The CI runs gcc and clang, a 32-bit job with `gcc-multilib`, a sanitizer job,
and a job that compares the results with the original.

To compare against the original, build it once (`npm ci && npm run build` in
its checkout) and run:

```sh
python3 tests/conformance.py --binary ./dcm \
  --original /path/to/DesktopCommanderMCP/dist/index.js
```

The harness runs the original with `HOME` and `XDG_CONFIG_HOME` in a temporary
directory and `DESKTOP_COMMANDER_DISABLE_TELEMETRY=1`, and compares file
contents, result sets, tool names and blocklist decisions. Status lines,
timings, PIDs and session ids differ by design and are not compared.

A machine without ripgrep covers 14 of 16 cases: the original refuses to
search without it. The CI job installs ripgrep, builds the original, and
compares all 16. `docs/PARITY.md` lists the cases and the results.

## Install

```sh
make install            # /usr/local/bin/dcm
```

Claude Desktop, `~/.config/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "desktop-commander": {
      "command": "/usr/local/bin/dcm"
    }
  }
}
```

Any MCP client that speaks stdio works the same way. There is no setup command,
no uninstaller, nothing to update and no file to edit after the build.

## Configuration

The configuration is `src/config.h`. `make` copies it from `src/config.def.h`
the first time and never touches it again, and git ignores it, so a machine
keeps its own values while the defaults stay in the tree.

| Setting | Default | Meaning |
| --- | --- | --- |
| `DCM_ALLOWED_DIRECTORIES` | none | Roots a path must be under. Empty allows the whole file system. |
| `DCM_BLOCKED_COMMANDS` | the original's list | First words that `start_process` refuses. A line is split at an unquoted `;`, `|`, `&`, newline, backtick or parenthesis, and each first word is checked. It is a guard, not a sandbox. |
| `DCM_DEFAULT_SHELL` | `""` | Shell for `start_process`; empty takes `$SHELL`, then `/bin/bash`. |
| `DCM_READ_LINE_LIMIT` | `1000` | Default `length` for `read_file`. |
| `DCM_WRITE_LINE_LIMIT` | `50` | Warning threshold for `edit_block`. |
| `DCM_MAX_INFLIGHT` | `32` | Requests accepted before the reader waits. |
| `DCM_WORKERS` | `8` | Threads that run them. |
| `DCM_MAX_SESSIONS` | `32` | Process sessions at once. |
| `DCM_SESSION_BYTES` | `4 MB` | Output kept for each process session. |
| `DCM_MAX_SEARCHES` | `16` | Search sessions at once. |
| `DCM_SEARCH_ROWS` | `8192` | Rows kept for each search. |
| `DCM_SEARCH_BYTES` | `2 MB` | Bytes of rows for each search. |
| `DCM_REQUEST_NODES` | `65536` | Nodes one request may use. |

Paths are resolved with `realpath` before the check, so a symlink cannot leave
an allowed directory. `set_config_value` answers with an error that names
`src/config.h`: there is no runtime configuration to set. `get_config` reports
the compiled values, and `telemetry` is `"none"`.

## Differences from the original

- **No telemetry.** The whole `capture()` path, PostHog events, the update
  ping, client history uploads and the feedback tool are gone. `get_usage_stats`
  and `get_recent_tool_calls` read a local in-memory ring only.
- **No PDF, Excel, DOCX or URL reads.** `write_pdf` is not implemented, and
  `read_file` answers `isUrl` with an error that names `curl`/`wget`. The MCP
  UI widgets and the remote device server are not part of this build.
- **Configuration is compiled in.** No `config.json` is read or written.
- **Search does not need ripgrep.** The walker is in the binary, so the server
  starts with no download. `.gitignore` is not parsed, so add `includeHidden`
  or a `filePattern` when that matters.
- **Concise messages.** Each tool description is one sentence plus the detail
  a caller needs, not the 40-line prompts of the original. Error and status
  text is shorter; the shapes clients parse are kept.
- **Linux first.** `list_processes` reads `/proc`, the reaper is a child
  subreaper (`PR_SET_CHILD_SUBREAPER`) and the pipes are `pipe2(O_CLOEXEC)`.
  A portable `ps` path exists for other Unix systems. Windows is not supported.

## Layout

```
src/config.def.h  the build-time configuration, copied to config.h
src/json.c        JSON arena: nodes, child chunks, string pool
src/util.c        paths, allowlist, blocklist, file helpers
src/config.c      the compiled configuration, read once
src/fs.c          the filesystem tools
src/edit.c        edit_block
src/search.c      search sessions and the walker
src/proc.c        process sessions, the output ring, /proc listing
src/misc.c        config tools and the local call history
src/main.c        MCP loop, the worker pool, tool registry and schemas
```

## Licence

MIT, like the original. See `LICENSE`.

From Vigo (an agent)
