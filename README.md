# Desktop Commander, rewritten in C11

An MCP server for files, searches and long-running terminal sessions. It is a
rewrite of [DesktopCommanderMCP](https://github.com/wonderwhy-er/DesktopCommanderMCP)
in C11 for Linux, with no Node.js, no runtime dependencies and no telemetry.

- One static binary, about 65 KB. Only the C library and pthreads.
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
group, not just the shell.

A search is a session too. `start_search` returns at once, a thread walks the
tree, and `get_more_search_results` pages the rows. Patterns are POSIX extended
regular expressions, or literal strings with `literalSearch`.

## Build and test

```sh
make                # builds ./dcm
make check          # unit tests and the conformance run
```

`make check` runs `tests/test_json`, `tests/test_util` and
`tests/conformance.py --binary ./dcm`. The conformance test creates a fixture
tree, calls every covered tool and checks the answers.

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

On this machine the comparison covers 14 of 16 cases. The two search cases are
skipped because the original refuses to search without a ripgrep binary, which
this machine does not have. `docs/PARITY.md` lists the cases and the results.

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
no uninstaller and nothing to update.

## Configuration

`~/.config/desktop-commander/config.json` (or `$XDG_CONFIG_HOME`), read at
start and written by `set_config_value`:

| Key | Default | Meaning |
| --- | --- | --- |
| `allowedDirectories` | `[]` | Roots a path must be under. `[]` allows the whole file system. |
| `blockedCommands` | the original's list | Commands that `start_process` refuses. |
| `defaultShell` | `$SHELL`, else `/bin/bash` | Shell for `start_process`. |
| `fileReadLineLimit` | `1000` | Default `length` for `read_file`. |
| `fileWriteLineLimit` | `50` | Warning threshold for `edit_block`. |

Paths are resolved with `realpath` before the check, so a symlink cannot leave
an allowed directory. `telemetryEnabled` is rejected: there is no telemetry to
enable.

## Differences from the original

- **No telemetry.** The whole `capture()` path, PostHog events, the update
  ping, client history uploads and the feedback tool are gone. `get_usage_stats`
  and `get_recent_tool_calls` read a local in-memory ring only.
- **No PDF, Excel, DOCX or URL reads.** `write_pdf` is not implemented, and
  `read_file` answers `isUrl` with an error that names `curl`/`wget`. The MCP
  UI widgets and the remote device server are not part of this build.
- **Search does not need ripgrep.** The walker is in the binary, so the server
  starts with no download. `.gitignore` is not parsed, so add `includeHidden`
  or a `filePattern` when that matters.
- **Concise messages.** Each tool description is one sentence plus the detail
  a caller needs, not the 40-line prompts of the original. Error and status
  text is shorter; the shapes clients parse are kept.
- **Linux first.** `list_processes` reads `/proc`. A portable `ps` path exists
  for other Unix systems. Windows is not supported.

## Layout

```
src/json.c      JSON reader/writer and string buffer
src/util.c      paths, allowlist, blocklist, file helpers
src/config.c    config.json, defaults, set_config_value keys
src/fs.c        the filesystem tools
src/edit.c      edit_block
src/search.c    search sessions and the walker
src/proc.c      process sessions, /proc listing
src/misc.c      config tools and the local call history
src/main.c      MCP loop, tool registry and schemas
```

## Licence

MIT, like the original. See `LICENSE`.
