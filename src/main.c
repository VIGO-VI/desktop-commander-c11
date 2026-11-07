/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 *
 * MCP server over stdio. One JSON-RPC message per line, as the protocol says.
 */
#include "json.h"
#include "tools.h"
#include "util.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef tool_result (*tool_fn)(json *args);

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *schema;
    tool_fn fn;
    bool read_only;
    bool destructive;
    bool open_world;
} tool_def;

/* Short descriptions: each one says what the tool does and the one detail a
 * caller needs. The schema carries the rest. */
static const tool_def TOOLS[] = {
    { "get_config", "Get Configuration",
      "Get the server configuration: blockedCommands, defaultShell, allowedDirectories, "
      "fileReadLineLimit, fileWriteLineLimit, version, systemInfo.",
      "{\"type\":\"object\",\"properties\":{}}", tool_get_config, true, false, false },

    { "set_config_value", "Set Configuration Value",
      "Set one configuration key. allowedDirectories [] means the whole file system.",
      "{\"type\":\"object\",\"properties\":{"
      "\"key\":{\"type\":\"string\"},"
      "\"value\":{\"type\":[\"string\",\"number\",\"boolean\",\"array\",\"null\"],"
      "\"items\":{\"type\":\"string\"}}},"
      "\"required\":[\"key\",\"value\"]}",
      tool_set_config_value, false, true, false },

    { "read_file", "Read File",
      "Read a file as text, or as an image block for PNG/JPEG/GIF/WebP. offset < 0 reads "
      "the last lines; length limits the lines returned.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"isUrl\":{\"type\":\"boolean\",\"default\":false},"
      "\"offset\":{\"type\":\"number\",\"default\":0},"
      "\"length\":{\"type\":\"number\",\"default\":1000},"
      "\"sheet\":{\"type\":\"string\"},\"range\":{\"type\":\"string\"},"
      "\"options\":{\"type\":\"object\"}},"
      "\"required\":[\"path\"]}",
      tool_read_file, true, false, false },

    { "read_multiple_files", "Read Multiple Files",
      "Read several files at once. A failed file is reported and does not stop the rest.",
      "{\"type\":\"object\",\"properties\":{"
      "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
      "\"required\":[\"paths\"]}",
      tool_read_multiple_files, true, false, false },

    { "write_file", "Write File",
      "Write or append to a text file. mode must be given for an existing non-empty file.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"content\":{\"type\":\"string\"},"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"rewrite\",\"append\"],\"default\":\"rewrite\"}},"
      "\"required\":[\"path\",\"content\"]}",
      tool_write_file, false, true, false },

    { "create_directory", "Create Directory",
      "Create a directory and its parents.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}",
      tool_create_directory, false, false, false },

    { "list_directory", "List Directory",
      "List a directory: [DIR] and [FILE] lines, recursive to depth (default 2). "
      "A nested directory shows at most 100 entries and then a warning.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"depth\":{\"type\":\"number\",\"default\":2}},"
      "\"required\":[\"path\"]}",
      tool_list_directory, true, false, false },

    { "move_file", "Move File",
      "Move or rename a file or directory.",
      "{\"type\":\"object\",\"properties\":{"
      "\"source\":{\"type\":\"string\"},\"destination\":{\"type\":\"string\"}},"
      "\"required\":[\"source\",\"destination\"]}",
      tool_move_file, false, true, false },

    { "get_file_info", "Get File Info",
      "Get size, times, permissions and type of a path.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"]}",
      tool_get_file_info, true, false, false },

    { "edit_block", "Edit Block",
      "Replace exact text in a file, expected_replacements times. When the text is not "
      "found, the closest match and its similarity are reported and nothing is written.",
      "{\"type\":\"object\",\"properties\":{"
      "\"file_path\":{\"type\":\"string\"},"
      "\"old_string\":{\"type\":\"string\"},"
      "\"new_string\":{\"type\":\"string\"},"
      "\"expected_replacements\":{\"type\":\"number\",\"default\":1},"
      "\"range\":{\"type\":\"string\"},"
      "\"content\":{},\"options\":{\"type\":\"object\"}},"
      "\"required\":[\"file_path\"]}",
      tool_edit_block, false, true, false },

    { "start_search", "Start Search",
      "Start a background search and return a session id. searchType files matches names, "
      "content matches text inside files. Read results with get_more_search_results.",
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"pattern\":{\"type\":\"string\"},"
      "\"searchType\":{\"type\":\"string\",\"enum\":[\"files\",\"content\"],\"default\":\"files\"},"
      "\"filePattern\":{\"type\":\"string\"},"
      "\"ignoreCase\":{\"type\":\"boolean\",\"default\":true},"
      "\"maxResults\":{\"type\":\"number\"},"
      "\"includeHidden\":{\"type\":\"boolean\",\"default\":false},"
      "\"contextLines\":{\"type\":\"number\",\"default\":5},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"earlyTermination\":{\"type\":\"boolean\"},"
      "\"literalSearch\":{\"type\":\"boolean\",\"default\":false}},"
      "\"required\":[\"path\",\"pattern\"]}",
      tool_start_search, true, false, false },

    { "get_more_search_results", "Get More Search Results",
      "Read a page of a search by session id. offset is the row index, length the page size.",
      "{\"type\":\"object\",\"properties\":{"
      "\"sessionId\":{\"type\":\"string\"},"
      "\"offset\":{\"type\":\"number\",\"default\":0},"
      "\"length\":{\"type\":\"number\",\"default\":100}},"
      "\"required\":[\"sessionId\"]}",
      tool_get_more_search_results, true, false, false },

    { "stop_search", "Stop Search",
      "Stop a running search session.",
      "{\"type\":\"object\",\"properties\":{\"sessionId\":{\"type\":\"string\"}},"
      "\"required\":[\"sessionId\"]}",
      tool_stop_search, false, false, false },

    { "list_searches", "List Searches",
      "List search sessions with their state.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_searches, true, false, false },

    { "start_process", "Start Process",
      "Run a shell command and wait up to timeout_ms for output. A command still running "
      "returns a PID for read_process_output and interact_with_process.",
      "{\"type\":\"object\",\"properties\":{"
      "\"command\":{\"type\":\"string\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"shell\":{\"type\":\"string\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"command\",\"timeout_ms\"]}",
      tool_start_process, false, true, true },

    { "read_process_output", "Read Process Output",
      "Read a session's output. offset 0 reads new lines, a positive offset reads from that "
      "line, a negative offset reads the last lines. Waits timeout_ms for new output.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pid\":{\"type\":\"number\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"offset\":{\"type\":\"number\"},"
      "\"length\":{\"type\":\"number\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"pid\"]}",
      tool_read_process_output, true, false, false },

    { "interact_with_process", "Interact With Process",
      "Send input to a running session and return its new output.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pid\":{\"type\":\"number\"},"
      "\"input\":{\"type\":\"string\"},"
      "\"timeout_ms\":{\"type\":\"number\"},"
      "\"wait_for_prompt\":{\"type\":\"boolean\"},"
      "\"verbose_timing\":{\"type\":\"boolean\"}},"
      "\"required\":[\"pid\",\"input\"]}",
      tool_interact_with_process, false, false, false },

    { "force_terminate", "Force Terminate",
      "End a session and every process its command started.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"number\"}},"
      "\"required\":[\"pid\"]}",
      tool_force_terminate, false, true, false },

    { "list_sessions", "List Sessions",
      "List the running sessions with their PIDs and run time.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_sessions, true, false, false },

    { "list_processes", "List Processes",
      "List the processes of the machine.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_list_processes, true, false, false },

    { "kill_process", "Kill Process",
      "Terminate one process by PID.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"number\"}},"
      "\"required\":[\"pid\"]}",
      tool_kill_process, false, true, false },

    { "get_usage_stats", "Get Usage Stats",
      "Call counts for this session.",
      "{\"type\":\"object\",\"properties\":{}}",
      tool_get_usage_stats, true, false, false },

    { "get_recent_tool_calls", "Get Recent Tool Calls",
      "List recent tool calls from the local history.",
      "{\"type\":\"object\",\"properties\":{"
      "\"maxResults\":{\"type\":\"number\",\"default\":50},"
      "\"toolName\":{\"type\":\"string\"},"
      "\"since\":{\"type\":\"string\"}}}",
      tool_get_recent_tool_calls, true, false, false },
};

static const size_t TOOL_COUNT = sizeof TOOLS / sizeof TOOLS[0];

#define INSTRUCTIONS \
    "Desktop Commander: files, search and long-running terminal sessions on this machine. " \
    "This build has no telemetry."

/* ---------------------------------------------------------------- output */

static void send_line(const char *text)
{
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_result(const json *id, json *result)
{
    if (!id)
        return;
    json *msg = jobj();
    jobj_set(msg, "jsonrpc", jstr("2.0"));
    jobj_set(msg, "id", id->t == J_INT ? jint(id->u.i) : jstr(id->t == J_STR ? id->u.s : ""));
    jobj_set(msg, "result", result);
    char *text = json_dump(msg, false);
    send_line(text);
    free(text);
    jobj_take(msg, "result"); /* the caller still owns it */
    json_free(msg);
}

static void send_error(const json *id, int code, const char *message)
{
    json *err = jobj();
    jobj_set(err, "code", jint(code));
    jobj_set(err, "message", jstr(message));
    json *msg = jobj();
    jobj_set(msg, "jsonrpc", jstr("2.0"));
    json *idv = jnull();
    if (id) {
        json_free(idv);
        idv = id->t == J_INT ? jint(id->u.i) : jstr(id->t == J_STR ? id->u.s : "");
    }
    jobj_set(msg, "id", idv);
    jobj_set(msg, "error", err);
    char *text = json_dump(msg, false);
    send_line(text);
    free(text);
    json_free(msg);
}

/* ---------------------------------------------------------------- handlers */

static json *tools_list(void)
{
    json *arr = jarr();
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        const tool_def *t = &TOOLS[i];
        json *o = jobj();
        jobj_set(o, "name", jstr(t->name));
        jobj_set(o, "title", jstr(t->title));
        jobj_set(o, "description", jstr(t->description));
        char *err = NULL;
        json *schema = json_parse(t->schema, strlen(t->schema), &err);
        free(err);
        jobj_set(o, "inputSchema", schema ? schema : jobj());
        json *annot = jobj();
        jobj_set(annot, "title", jstr(t->title));
        jobj_set(annot, "readOnlyHint", jbool(t->read_only));
        jobj_set(annot, "destructiveHint", jbool(t->destructive));
        jobj_set(annot, "openWorldHint", jbool(t->open_world));
        jobj_set(o, "annotations", annot);
        jarr_push(arr, o);
    }
    json *root = jobj();
    jobj_set(root, "tools", arr);
    return root;
}

static const tool_def *find_tool(const char *name)
{
    for (size_t i = 0; i < TOOL_COUNT; i++)
        if (strcmp(TOOLS[i].name, name) == 0)
            return &TOOLS[i];
    return NULL;
}

static void handle_call(const json *id, json *params)
{
    const char *name = jstr_get(params, "name", NULL);
    const tool_def *t = name ? find_tool(name) : NULL;
    if (!t) {
        send_error(id, -32602, "Unknown tool");
        return;
    }
    json *args = jget(params, "arguments");
    bool owned = false;
    if (!args || args->t != J_OBJ) {
        args = jobj();
        owned = true;
    }

    tool_result r = t->fn(args);
    history_record(t->name, args, &r);
    if (owned)
        json_free(args);

    json *result = jobj();
    jobj_set(result, "content", r.content);
    if (r.is_error)
        jobj_set(result, "isError", jbool(true));
    if (r.structured)
        jobj_set(result, "structuredContent", r.structured);
    r.content = NULL;
    r.structured = NULL;
    send_result(id, result);
    json_free(result);
}

static void handle_initialize(const json *id, json *params)
{
    const char *asked = jstr_get(params, "protocolVersion", "2024-11-05");
    json *caps = jobj();
    jobj_set(caps, "tools", jobj());
    jobj_set(caps, "resources", jobj());
    jobj_set(caps, "prompts", jobj());
    jobj_set(caps, "logging", jobj());

    json *server = jobj();
    jobj_set(server, "name", jstr("desktop-commander"));
    jobj_set(server, "version", jstr(DCM_VERSION));

    json *result = jobj();
    jobj_set(result, "protocolVersion", jstr(asked && *asked ? asked : "2024-11-05"));
    jobj_set(result, "capabilities", caps);
    jobj_set(result, "serverInfo", server);
    jobj_set(result, "instructions", jstr(INSTRUCTIONS));
    send_result(id, result);
    json_free(result);
}

/* ---------------------------------------------------------------- loop */

static void run(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;

        char *err = NULL;
        json *msg = json_parse(line, (size_t)n, &err);
        if (!msg) {
            send_error(NULL, -32700, err ? err : "Parse error");
            free(err);
            continue;
        }

        json *id = jget(msg, "id");
        const char *method = jstr_get(msg, "method", NULL);
        json *params = jget(msg, "params");
        if (!params || params->t != J_OBJ)
            params = NULL;

        if (!method) {
            /* A response to a request we never send: ignore it. */
            json_free(msg);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(id, params);
        } else if (strcmp(method, "notifications/initialized") == 0 ||
                   strcmp(method, "notifications/cancelled") == 0) {
            /* no answer */
        } else if (strcmp(method, "ping") == 0) {
            json *r = jobj();
            send_result(id, r);
        } else if (strcmp(method, "tools/list") == 0) {
            json *r = tools_list();
            send_result(id, r);
            json_free(r);
        } else if (strcmp(method, "tools/call") == 0) {
            if (!params)
                send_error(id, -32602, "Missing params");
            else
                handle_call(id, params);
        } else if (strcmp(method, "resources/list") == 0) {
            json *r = jobj();
            jobj_set(r, "resources", jarr());
            send_result(id, r);
            json_free(r);
        } else if (strcmp(method, "prompts/list") == 0) {
            json *r = jobj();
            jobj_set(r, "prompts", jarr());
            send_result(id, r);
            json_free(r);
        } else if (strcmp(method, "logging/setLevel") == 0) {
            json *r = jobj();
            send_result(id, r);
            json_free(r);
        } else if (strcmp(method, "completion/complete") == 0) {
            json *r = jobj();
            json *c = jobj();
            jobj_set(c, "values", jarr());
            jobj_set(r, "completion", c);
            send_result(id, r);
            json_free(r);
        } else {
            send_error(id, -32601, "Method not found");
        }
        json_free(msg);
    }
    free(line);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("desktop-commander %s (C11)\n", DCM_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--version] [--help]\n\n"
                   "An MCP server over stdio: files, search and terminal sessions.\n"
                   "Settings: %s/config.json\n",
                   argv[0], dcm_config_dir());
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    run();
    history_shutdown();
    return 0;
}
