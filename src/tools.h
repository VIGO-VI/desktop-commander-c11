/* SPDX-License-Identifier: MIT
 * Copyright 2026 the Desktop Commander C contributors
 */
#ifndef DCM_TOOLS_H
#define DCM_TOOLS_H

#include "json.h"

#include <stdbool.h>

typedef struct {
    json *content;    /* array of content blocks, owned */
    json *structured; /* object or NULL, owned */
    bool is_error;
} tool_result;

tool_result result_text(const char *text);
tool_result result_error(const char *fmt, ...);
tool_result result_text_struct(const char *text, json *structured);
void result_free(tool_result *r);

/* Each tool takes the parsed arguments object (never NULL) and returns a result. */
tool_result tool_get_config(json *a);
tool_result tool_set_config_value(json *a);
tool_result tool_read_file(json *a);
tool_result tool_read_multiple_files(json *a);
tool_result tool_write_file(json *a);
tool_result tool_create_directory(json *a);
tool_result tool_list_directory(json *a);
tool_result tool_move_file(json *a);
tool_result tool_get_file_info(json *a);
tool_result tool_edit_block(json *a);

tool_result tool_start_search(json *a);
tool_result tool_get_more_search_results(json *a);
tool_result tool_stop_search(json *a);
tool_result tool_list_searches(json *a);

tool_result tool_start_process(json *a);
tool_result tool_read_process_output(json *a);
tool_result tool_interact_with_process(json *a);
tool_result tool_force_terminate(json *a);
tool_result tool_list_sessions(json *a);
tool_result tool_list_processes(json *a);
tool_result tool_kill_process(json *a);

tool_result tool_get_usage_stats(json *a);
tool_result tool_get_recent_tool_calls(json *a);

/* Shared: record a call for get_usage_stats / get_recent_tool_calls. */
void history_record(const char *tool, const json *args, const tool_result *r);
void history_shutdown(void);

#endif
