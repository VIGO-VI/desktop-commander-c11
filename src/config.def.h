/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * Build-time configuration. `make` copies this file to src/config.h the first
 * time; edit src/config.h (git ignores it) and rebuild. The server reads no
 * configuration file and has no settable key at run time.
 *
 * Every number below is a bound, not a hint: the arrays and buffers are
 * static, so the server's memory is known at build time.
 */
#ifndef DCM_CONFIG_H
#define DCM_CONFIG_H

/* Roots a tool may touch, as a comma-separated list of absolute paths.
 * Paths are resolved with realpath, so a symlink cannot leave a root.
 * Leave it empty to allow the whole file system. */
#define DCM_ALLOWED_DIRECTORIES

/* A command whose first word is one of these is refused by start_process.
 * This is the list of the original server. */
#define DCM_BLOCKED_COMMANDS                                                  \
    "mkfs", "format", "mount", "umount", "fdisk", "dd", "parted", "diskpart", \
    "sudo", "su", "passwd", "adduser", "useradd", "usermod", "groupadd",      \
    "chsh", "visudo", "shutdown", "reboot", "halt", "poweroff", "init",       \
    "iptables", "firewall", "netsh", "sfc", "bcdedit", "reg", "net", "sc",    \
    "runas", "cipher", "takeown",

/* Shell for start_process. "" takes $SHELL, then /bin/bash. */
#define DCM_DEFAULT_SHELL ""

/* Default length of read_file, and the line count edit_block warns above. */
#define DCM_READ_LINE_LIMIT 1000
#define DCM_WRITE_LINE_LIMIT 50

/* Requests accepted before the reader waits: three agents of ten calls each,
 * plus a little. A request beyond this waits for a slot. */
#define DCM_MAX_INFLIGHT 32
/* Threads that run them. A blocked call parks its own thread only. */
#define DCM_WORKERS 8
/* Stack of one worker. The parser is iterative and the walker is shallow. */
#define DCM_WORKER_STACK (192u * 1024)
/* Largest request line. Anything longer is refused, not buffered. */
#define DCM_MAX_REQUEST_BYTES (8u * 1024 * 1024)

/* Nodes and string bytes one request may use. Both are reused across calls. */
#define DCM_REQUEST_NODES 65536
#define DCM_REQUEST_STRINGS (128u * 1024)

/* Process sessions, and the output each keeps. The oldest lines go first. */
#define DCM_MAX_SESSIONS 32
#define DCM_SESSION_BYTES (4u * 1024 * 1024)

/* Search sessions, and the rows each keeps. */
#define DCM_MAX_SEARCHES 16
#define DCM_SEARCH_ROWS 8192
#define DCM_SEARCH_BYTES (2u * 1024 * 1024)

/* Calls kept for get_recent_tool_calls. */
#define DCM_HISTORY_ENTRIES 200

#endif
