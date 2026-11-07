# Design

## Shape

One process, one thread per concern, no event loop. `src/main.c` reads one
JSON-RPC message per line from stdin, dispatches a tool, writes one line to
stdout, and flushes. Anything a human might read goes to stderr, so a client
never parses a log line as a result.

```
stdin ──▶ json_parse ──▶ tools/call ──▶ tool fn ──▶ tool_result ──▶ stdout
```

A tool function receives the parsed `arguments` object and returns
`{content, structuredContent, isError}`. Content is an array of MCP blocks, so
images and text share one shape. No tool writes to stdout itself.

## Threads

| Thread | Role |
| --- | --- |
| main | protocol loop; runs each tool call to completion |
| reader (one per process session) | drains the output pipe into the session's line buffer |
| reaper (one) | `waitpid(-1)` collects exit statuses, so no zombie survives |
| search (one per search session, detached) | walks the tree and appends rows |

Process sessions and search sessions are the only long-lived state. Both take a
mutex and a condition variable; a read waits on the condition, so a REPL that
is quiet costs nothing and answers the moment it prints.

`start_process` forks a child into its own session (`setsid`) with one pipe for
stdout and stderr and one for stdin. That group is what `force_terminate`
signals, so a command that started a server or an SSH connection ends whole.
On Linux the pipes are `pipe2(O_CLOEXEC)` and the reaper sets
`PR_SET_CHILD_SUBREAPER`, so orphans of a killed group are reaped here too.

`list_processes` reads `/proc` directly on Linux: it is the kernel's table and
needs no fork. Other Unix systems fall back to `ps`.

## Memory

Every value is either owned by its container or explicitly taken with
`jobj_take`. `json_free` walks the tree. There is no arena and no garbage
collector; the server's steady state is the session buffers plus the call
history ring (200 entries).

The output buffer of a session is capped at 50 MB. The oldest lines are evicted
first and the answer says how many, so line numbers are never silently wrong.

## Search

A search thread walks the tree with `opendir`/`lstat`, matching:

- files: `fnmatch` for a glob pattern, case-insensitive substring otherwise;
- content: POSIX extended `regcomp`/`regexec`, or a substring for
  `literalSearch`.

Files over 32 MB and files with a NUL byte in the first 8 KB are skipped, the
same kinds of file ripgrep would not print as text. Rows are `path:line - text`
for a match and `path:line · text` for a context line, so a reader can tell
them apart without a second field.

The search has its own deadline and `maxResults`; both are checked between
lines, so `stop_search` answers within one file, not one tree.

## Protocol notes

- `initialize` echoes the requested `protocolVersion`, and advertises tools,
  resources, prompts and logging, all empty but present.
- Unknown methods get `-32601`; a malformed line gets `-32700` with no id.
- Notifications never get an answer.
- Notifications from the client (`notifications/initialized`,
  `notifications/cancelled`) are accepted and ignored.

## Why C11

The original's dependency tree is 1,043 packages and a downloaded ripgrep. The
work is: read a file, write a file, match a string, wait on a pipe. The C
library does all of it. A single 65 KB binary starts in under a millisecond,
which matters when a client spawns it for every session, and it has no update
path to defend.

Strict aliasing and `-fwrapv` rules of the larger project this came from do not
apply here; the code uses plain C11 with `_GNU_SOURCE` for the Linux calls.
