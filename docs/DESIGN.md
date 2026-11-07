# Design

## Shape

One process, a fixed pool of worker threads, no event loop. A reader takes one
JSON-RPC message per line from stdin and hands it to a free worker. The worker
parses the message into its own document, dispatches the tool, writes the
answer and clears the document for the next request. Anything a human might
read goes to stderr, so a client never parses a log line as a result.

```
stdin ──▶ slot ──▶ worker ──▶ json_parse ──▶ tool fn ──▶ stdout (one write)
```

A tool function receives the request document and the arguments object, and
returns `{content, structuredContent, isError}` as handles of the same
document. Content is an array of MCP blocks, so images and text share one
shape. No tool writes to stdout itself.

## Threads

| Thread | Count | Role |
| --- | --- | --- |
| reader | 1 (main) | reads lines, waits when 32 requests are in flight |
| worker | `DCM_WORKERS` (8) | parses and runs one request at a time |
| reader (process session) | one per session | drains the output pipe into the session ring |
| reaper | 1 | `waitpid(-1)` collects exit statuses, so no zombie survives |
| search | one per search session | walks the tree and appends rows |

A worker stack is `DCM_WORKER_STACK` (192 KB), set at creation. The JSON parser
is iterative in the sense that it recurses only on nesting, which is bounded at
256, and the walker is bounded by the tree.

Two or three agents of a client send up to 32 calls at once. The reader copies
the line into a slot and wakes a worker, so a call that waits on a prompt parks
one worker and no other. When every slot is busy the reader waits for one,
which is the backpressure: memory does not grow with the client's queue.

`start_process` forks a child into its own session (`setsid`) with one pipe for
stdout and stderr and one for stdin. That group is what `force_terminate`
signals, so a command that started a server or an SSH connection ends whole.
On Linux the pipes are `pipe2(O_CLOEXEC)` and the reaper sets
`PR_SET_CHILD_SUBREAPER`, so orphans of a killed group are reaped here too.

The reader drains the pipe until the process ends, whatever the answer keeps.
An answer holds `max_output_lines` lines at most: half from the head, half from
the tail, and one line that says how many were left out. When the output passes
the default cap the reader opens a file and writes the whole stream to it, from
the first line, so the middle is one `read_file` away. A cap that closed the
read end would kill a command that prints more with SIGPIPE, and the reader
never closes it early.

A wait is a condition variable on the monotonic clock, so a change of the wall
clock neither stretches nor cuts it. The reader broadcasts on every read, not
only on a whole line: a prompt with no newline ends the wait, or a REPL would
look dead for the whole timeout. The unterminated tail lives in the session
buffer beside the ring, and a page that reaches the end returns it. It comes
back only when it grew, so a one-line progress bar does not spin the caller.
The answer of a call moves the unread pointer only past the lines it holds, so
a capped start does not hide the middle from the next read.

`start_process` first checks the blocklist. It compares the first word of each
command in the line, and it splits at an unquoted `;`, `|`, `&`, newline,
backtick or parenthesis, with one pair of quotes removed. This is a guard
against a mistake, not a sandbox: a shell keyword or an assignment before a
command hides the name, and `start_process` runs the shell it was given.

`list_processes` reads `/proc` directly on Linux: it is the kernel's table and
needs no fork. Other Unix systems fall back to `ps`.

## Memory

There is no per-object allocation in a request. The JSON document is three
linear arrays:

- nodes of 24 bytes (20 on a 32-bit target), addressed by a 4-byte index;
- child chunks of 16 entries, linked in order, so a container that goes on hold
  while another is built keeps its children in place;
- a byte pool for strings, and one block for a string that does not fit it.

`jdoc_reset` clears the counts and hands the same buffers back, so a server of
steady traffic allocates once per worker. A string handle is 4 bytes and never
zero, so an absent key and the empty string are different.

Sessions are slots of static arrays: `g_procs[DCM_MAX_SESSIONS]` and
`g_searches[DCM_MAX_SEARCHES]`. A process slot is reused only when its reader
has left and its child was collected, because the reader can still add lines
after the child exits. The output of a slot that is not finished stays where a
client can read it, and the count of sessions is what bounds the memory.

A process session keeps its output in a byte ring of `DCM_SESSION_BYTES`
(4 MB). A new line goes into the free space and the oldest whole lines are
dropped, so a process that prints for an hour costs the same as one that
prints a page, and line numbers say how many lines were dropped. The file of
the session stops at `DCM_SPOOL_BYTES` (16 MB), so a flood costs a bounded file
as well as a bounded ring, and the answer says where it stopped.

A search session keeps its rows as NUL-terminated strings in one block of
`DCM_SEARCH_BYTES`, with an index of 4-byte offsets and a flag bit that marks a
context row. The reader and the walker share the block under the session lock.

The call history is a ring of `DCM_HISTORY_ENTRIES` inline records: a tool name,
a time, a flag and a 96-byte summary of the arguments. Nothing is stored as
JSON and nothing is freed.

## Configuration

`src/config.h` holds every setting; `make` copies it from `src/config.def.h` the
first time. `cfg()` resolves the allowlist with `realpath` once, behind
`pthread_once`, and every tool reads the same const struct. There is no file to
parse, no value to set, and no allocation on the path of a path check.

`set_config_value` is kept for clients that call it, and answers with the error
that names the header.

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
- Unknown methods get `-32601`; a malformed line gets `-32700` with no id, and
  a line over `DCM_MAX_REQUEST_BYTES` gets `-32600` without being buffered.
- Notifications never get an answer.
- Notifications from the client (`notifications/initialized`,
  `notifications/cancelled`) are accepted and ignored.
- The tool list is written from the table: the schemas are JSON text in the
  binary, so a listing parses nothing and copies nothing.

## Why C11

The original's dependency tree is 1,043 packages and a downloaded ripgrep. The
work is: read a file, write a file, match a string, wait on a pipe. The C
library does all of it. A single 118 KB binary starts in under a millisecond,
which matters when a client spawns it for every session, and it has no update
path to defend.

The code is plain ISO C11 with `_GNU_SOURCE` for the Linux calls. It builds
without `-fwrapv`: arithmetic that wraps is done in unsigned types, and a
product that can overflow gets a wider intermediate. It builds with strict
aliasing on, and `-Wstrict-aliasing=2` must stay quiet. A byte buffer is read
through `char`, an integer built from wire bytes goes through `memcpy`, and a
node is read only through its own type.
