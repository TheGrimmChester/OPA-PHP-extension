#include "opa.h"
#include "serialize.h"
#include "transport.h"

// Store original error and exception handlers
static zend_error_handling_t original_error_handling;
static zend_fcall_info_cache *original_exception_handler = NULL;

// Generate error fingerprint for grouping similar errors
static char* generate_error_fingerprint(const char *error_type, const char *error_message, const char *file, int line) {
    smart_string fingerprint = {0};
    smart_string_appends(&fingerprint, error_type);
    smart_string_appends(&fingerprint, ":");
    
    // Normalize error message (remove variable parts)
    if (error_message) {
        const char *msg = error_message;
        // Skip common variable patterns
        while (*msg) {
            if (strncmp(msg, "/var/www", 8) == 0 || strncmp(msg, "/app", 4) == 0) {
                // Skip absolute paths
                while (*msg && *msg != ' ') msg++;
            } else if (*msg >= '0' && *msg <= '9') {
                // Skip numbers (IDs, timestamps, etc.)
                while (*msg >= '0' && *msg <= '9') msg++;
            } else {
                smart_string_appendc(&fingerprint, *msg);
                msg++;
            }
        }
    }
    
    if (file) {
        // Extract just the filename, not full path
        const char *filename = strrchr(file, '/');
        if (filename) {
            filename++;
        } else {
            filename = file;
        }
        smart_string_appends(&fingerprint, "@");
        smart_string_appends(&fingerprint, filename);
    }
    
    if (line > 0) {
        char line_str[32];
        snprintf(line_str, sizeof(line_str), ":%d", line);
        smart_string_appends(&fingerprint, line_str);
    }
    
    smart_string_0(&fingerprint);
    return fingerprint.c ? fingerprint.c : NULL;
}

// Serialize stack trace to JSON string
static char* serialize_stack_trace(zval *trace) {
    if (!trace || Z_TYPE_P(trace) != IS_ARRAY) {
        return NULL;
    }
    
    smart_string json = {0};
    smart_string_appends(&json, "[");
    
    zval *frame;
    int first = 1;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(trace), frame) {
        if (Z_TYPE_P(frame) != IS_ARRAY) continue;
        
        if (!first) smart_string_appends(&json, ",");
        first = 0;
        
        smart_string_appends(&json, "{");
        
        zval *file = zend_hash_str_find(Z_ARRVAL_P(frame), "file", sizeof("file")-1);
        zval *line = zend_hash_str_find(Z_ARRVAL_P(frame), "line", sizeof("line")-1);
        zval *function = zend_hash_str_find(Z_ARRVAL_P(frame), "function", sizeof("function")-1);
        zval *class = zend_hash_str_find(Z_ARRVAL_P(frame), "class", sizeof("class")-1);
        
        if (file && Z_TYPE_P(file) == IS_STRING) {
            smart_string_appends(&json, "\"file\":\"");
            json_escape_string(&json, Z_STRVAL_P(file), Z_STRLEN_P(file));
            smart_string_appends(&json, "\"");
        }
        
        if (line && Z_TYPE_P(line) == IS_LONG) {
            char line_str[32];
            snprintf(line_str, sizeof(line_str), "%ld", Z_LVAL_P(line));
            smart_string_appends(&json, ",\"line\":");
            smart_string_appends(&json, line_str);
        }
        
        if (function && Z_TYPE_P(function) == IS_STRING) {
            smart_string_appends(&json, ",\"function\":\"");
            json_escape_string(&json, Z_STRVAL_P(function), Z_STRLEN_P(function));
            smart_string_appends(&json, "\"");
        }
        
        if (class && Z_TYPE_P(class) == IS_STRING) {
            smart_string_appends(&json, ",\"class\":\"");
            json_escape_string(&json, Z_STRVAL_P(class), Z_STRLEN_P(class));
            smart_string_appends(&json, "\"");
        }
        
        smart_string_appends(&json, "}");
    } ZEND_HASH_FOREACH_END();
    
    smart_string_appends(&json, "]");
    smart_string_0(&json);
    
    // Always return a valid JSON array (even if empty)
    if (json.c && json.len > 0) {
        char *result = emalloc(json.len + 1);
        if (result) {
            memcpy(result, json.c, json.len);
            result[json.len] = '\0';
        }
        smart_string_free(&json);
        return result;
    }
    
    // Return empty array if no frames
    smart_string_free(&json);
    char *empty_array = emalloc(3);
    if (empty_array) {
        strcpy(empty_array, "[]");
    }
    return empty_array;
}

// Send error to agent (exported for use from opa_api.c)
void send_error_to_agent(int error_type, const char *error_message, const char *file, int line, zval *stack_trace) {
    if (!OPA_G(enabled)) {
        return;
    }
    
    // Get current trace/span IDs
    char *trace_id = root_span_trace_id ? root_span_trace_id : generate_id();
    char *span_id = root_span_span_id ? root_span_span_id : generate_id();
    
    // Generate error fingerprint
    const char *error_type_str = "Error";
    if (error_type == E_ERROR) error_type_str = "Error";
    else if (error_type == E_WARNING) error_type_str = "Warning";
    else if (error_type == E_PARSE) error_type_str = "Parse";
    else if (error_type == E_NOTICE) error_type_str = "Notice";
    else if (error_type == E_CORE_ERROR) error_type_str = "CoreError";
    else if (error_type == E_CORE_WARNING) error_type_str = "CoreWarning";
    else if (error_type == E_COMPILE_ERROR) error_type_str = "CompileError";
    else if (error_type == E_COMPILE_WARNING) error_type_str = "CompileWarning";
    else if (error_type == E_USER_ERROR) error_type_str = "UserError";
    else if (error_type == E_USER_WARNING) error_type_str = "UserWarning";
    else if (error_type == E_USER_NOTICE) error_type_str = "UserNotice";
    else if (error_type == E_STRICT) error_type_str = "Strict";
    else if (error_type == E_RECOVERABLE_ERROR) error_type_str = "RecoverableError";
    else if (error_type == E_DEPRECATED) error_type_str = "Deprecated";
    else if (error_type == E_USER_DEPRECATED) error_type_str = "UserDeprecated";
    
    char *fingerprint = generate_error_fingerprint(error_type_str, error_message, file, line);
    char *group_id = generate_id(); // Use fingerprint hash as group_id
    
    // Serialize stack trace
    char *stack_trace_json = NULL;
    if (stack_trace) {
        stack_trace_json = serialize_stack_trace(stack_trace);
    }
    
    // Build error JSON
    smart_string json = {0};
    smart_string_appends(&json, "{\"type\":\"error\",");
    smart_string_appends(&json, "\"trace_id\":\"");
    smart_string_appends(&json, trace_id);
    smart_string_appends(&json, "\",\"span_id\":\"");
    smart_string_appends(&json, span_id);
    smart_string_appends(&json, "\",\"instance_id\":\"");
    char *instance_id = generate_id();
    smart_string_appends(&json, instance_id);
    efree(instance_id);
    smart_string_appends(&json, "\",\"group_id\":\"");
    smart_string_appends(&json, group_id);
    smart_string_appends(&json, "\",\"fingerprint\":\"");
    if (fingerprint) {
        json_escape_string(&json, fingerprint, strlen(fingerprint));
        efree(fingerprint);
    }
    smart_string_appends(&json, "\",\"error_type\":\"");
    smart_string_appends(&json, error_type_str);
    smart_string_appends(&json, "\",\"error_message\":\"");
    if (error_message) {
        json_escape_string(&json, error_message, strlen(error_message));
    }
    smart_string_appends(&json, "\",\"file\":\"");
    if (file) {
        json_escape_string(&json, file, strlen(file));
    }
    smart_string_appends(&json, "\",\"line\":");
    char line_str[32];
    snprintf(line_str, sizeof(line_str), "%d", line);
    smart_string_appends(&json, line_str);
    
    if (stack_trace_json) {
        smart_string_appends(&json, ",\"stack_trace\":");
        smart_string_appends(&json, stack_trace_json);
        efree(stack_trace_json);
    }
    
    // Add metadata
    smart_string_appends(&json, ",\"organization_id\":\"");
    if (OPA_G(organization_id)) {
        json_escape_string(&json, OPA_G(organization_id), strlen(OPA_G(organization_id)));
    } else {
        smart_string_appends(&json, "default-org");
    }
    smart_string_appends(&json, "\",\"project_id\":\"");
    if (OPA_G(project_id)) {
        json_escape_string(&json, OPA_G(project_id), strlen(OPA_G(project_id)));
    } else {
        smart_string_appends(&json, "default-project");
    }
    smart_string_appends(&json, "\",\"service\":\"");
    if (OPA_G(service)) {
        json_escape_string(&json, OPA_G(service), strlen(OPA_G(service)));
    } else {
        smart_string_appends(&json, "php-fpm");
    }
    smart_string_appends(&json, "\""); // Close service string
    
    // Add timestamp
    long timestamp_ms = get_timestamp_ms();
    char ts_str[64];
    snprintf(ts_str, sizeof(ts_str), "%ld", timestamp_ms);
    smart_string_appends(&json, ",\"occurred_at_ms\":");
    smart_string_appends(&json, ts_str);
    
    smart_string_appends(&json, "}");
    smart_string_0(&json);
    
    if (json.c && json.len > 0) {
        char *msg = emalloc(json.len + 1);
        if (msg) {
            memcpy(msg, json.c, json.len);
            msg[json.len] = '\0';
            send_message_direct(msg, 1); // Send with compression
        }
        smart_string_free(&json);
    } else {
        smart_string_free(&json);
    }
    
    efree(group_id);
    if (trace_id != root_span_trace_id) efree(trace_id);
    if (span_id != root_span_span_id) efree(span_id);
}

// Track error via shutdown function (more reliable than error handler)
static void opa_track_error_via_shutdown(void) {
    if (!OPA_G(enabled)) {
        return;
    }
    
    // Check if there was a fatal error
    // In PHP 8.4, EG(error_zval) is a zval, not a zval *
    if (Z_TYPE(EG(error_zval)) != IS_UNDEF) {
        zval *error = &EG(error_zval);
        
        // Extract error information
        const char *error_message = NULL;
        const char *file = NULL;
        int line = 0;
        int error_type = E_ERROR;
        
        if (Z_TYPE_P(error) == IS_STRING) {
            error_message = Z_STRVAL_P(error);
        } else if (Z_TYPE_P(error) == IS_OBJECT) {
            // It's an exception
            zval message_zv, file_zv, line_zv;
            ZVAL_UNDEF(&message_zv);
            ZVAL_UNDEF(&file_zv);
            ZVAL_UNDEF(&line_zv);
            
            zend_call_method_with_0_params(Z_OBJ_P(error), Z_OBJCE_P(error), NULL, "getmessage", &message_zv);
            zend_call_method_with_0_params(Z_OBJ_P(error), Z_OBJCE_P(error), NULL, "getfile", &file_zv);
            zend_call_method_with_0_params(Z_OBJ_P(error), Z_OBJCE_P(error), NULL, "getline", &line_zv);
            
            if (Z_TYPE(message_zv) == IS_STRING) {
                error_message = Z_STRVAL(message_zv);
            }
            if (Z_TYPE(file_zv) == IS_STRING) {
                file = Z_STRVAL(file_zv);
            }
            if (Z_TYPE(line_zv) == IS_LONG) {
                line = Z_LVAL(line_zv);
            }
            
            // Get trace
            zval trace;
            ZVAL_UNDEF(&trace);
            zend_call_method_with_0_params(Z_OBJ_P(error), Z_OBJCE_P(error), NULL, "gettrace", &trace);
            
            send_error_to_agent(error_type, error_message, file, line, &trace);
            
            if (Z_TYPE(message_zv) != IS_UNDEF) zval_ptr_dtor(&message_zv);
            if (Z_TYPE(file_zv) != IS_UNDEF) zval_ptr_dtor(&file_zv);
            if (Z_TYPE(line_zv) != IS_UNDEF) zval_ptr_dtor(&line_zv);
            if (Z_TYPE(trace) != IS_UNDEF) zval_ptr_dtor(&trace);
        }
    }
}

// Exception handler callback
static void opa_exception_handler(zval *exception) {
    if (!exception || Z_TYPE_P(exception) != IS_OBJECT) {
        return;
    }
    
    // Get exception message
    zval message_zv;
    ZVAL_UNDEF(&message_zv);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "getmessage", &message_zv);
    
    const char *error_message = NULL;
    if (Z_TYPE(message_zv) == IS_STRING) {
        error_message = Z_STRVAL(message_zv);
    }
    
    // Get exception code
    zval code_zv;
    ZVAL_UNDEF(&code_zv);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "getcode", &code_zv);
    
    // Get exception file and line
    zval file_zv, line_zv;
    ZVAL_UNDEF(&file_zv);
    ZVAL_UNDEF(&line_zv);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "getfile", &file_zv);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "getline", &line_zv);
    
    const char *file = NULL;
    int line = 0;
    if (Z_TYPE(file_zv) == IS_STRING) {
        file = Z_STRVAL(file_zv);
    }
    if (Z_TYPE(line_zv) == IS_LONG) {
        line = Z_LVAL(line_zv);
    }
    
    // Get exception trace
    zval trace_zv;
    ZVAL_UNDEF(&trace_zv);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "gettraceasstring", &trace_zv);
    
    // Get class name
    const char *error_type = "Exception";
    zend_string *class_name = Z_OBJCE_P(exception)->name;
    if (class_name) {
        error_type = ZSTR_VAL(class_name);
    }
    
    // Get trace as array
    zval trace_array;
    ZVAL_UNDEF(&trace_array);
    zend_call_method_with_0_params(Z_OBJ_P(exception), Z_OBJCE_P(exception), NULL, "gettrace", &trace_array);
    
    send_error_to_agent(E_ERROR, error_message, file, line, &trace_array);
    
    // Cleanup
    if (Z_TYPE(message_zv) != IS_UNDEF) zval_ptr_dtor(&message_zv);
    if (Z_TYPE(code_zv) != IS_UNDEF) zval_ptr_dtor(&code_zv);
    if (Z_TYPE(file_zv) != IS_UNDEF) zval_ptr_dtor(&file_zv);
    if (Z_TYPE(line_zv) != IS_UNDEF) zval_ptr_dtor(&line_zv);
    if (Z_TYPE(trace_zv) != IS_UNDEF) zval_ptr_dtor(&trace_zv);
    if (Z_TYPE(trace_array) != IS_UNDEF) zval_ptr_dtor(&trace_array);
}

// NOTE: opa_track_error() is defined in opa_api.c, not here
// This file only contains the internal error tracking functions

// Check if a log level should be tracked based on INI configuration
static int should_track_log_level(const char *level) {
    if (!OPA_G(log_levels) || strlen(OPA_G(log_levels)) == 0) {
        return 0; // No levels configured
    }
    
    // Parse comma-separated list
    char *levels_copy = estrdup(OPA_G(log_levels));
    if (!levels_copy) {
        return 0;
    }
    
    char *token = strtok(levels_copy, ",");
    int found = 0;
    
    while (token != NULL) {
        // Trim whitespace
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') {
            *end = '\0';
            end--;
        }
        
        if (strcasecmp(token, level) == 0) {
            found = 1;
            break;
        }
        token = strtok(NULL, ",");
    }
    
    efree(levels_copy);
    return found;
}

// Parse log message to extract level (e.g., [ERROR], [WARN], [CRITICAL])
static const char* parse_log_level(const char *message) {
    if (!message || strlen(message) < 3) {
        return "info"; // Default level
    }
    
    // Check for [LEVEL] prefix
    if (message[0] == '[') {
        const char *end = strchr(message, ']');
        if (end && end > message + 1) {
            size_t len = end - message - 1;
            if (len > 0 && len < 20) {
                char level[21];
                memcpy(level, message + 1, len);
                level[len] = '\0';
                
                // Normalize to lowercase
                for (int i = 0; level[i]; i++) {
                    if (level[i] >= 'A' && level[i] <= 'Z') {
                        level[i] = level[i] - 'A' + 'a';
                    }
                }
                
                // Check if it's a known level
                if (strcmp(level, "critical") == 0 || strcmp(level, "crit") == 0) {
                    return "critical";
                } else if (strcmp(level, "error") == 0 || strcmp(level, "err") == 0) {
                    return "error";
                } else if (strcmp(level, "warning") == 0 || strcmp(level, "warn") == 0) {
                    return "warn";
                }
            }
        }
    }
    
    // Check for common patterns
    if (strncasecmp(message, "error:", 6) == 0 || strncasecmp(message, "error ", 6) == 0) {
        return "error";
    }
    if (strncasecmp(message, "warning:", 8) == 0 || strncasecmp(message, "warning ", 8) == 0) {
        return "warn";
    }
    if (strncasecmp(message, "critical:", 9) == 0 || strncasecmp(message, "critical ", 9) == 0) {
        return "critical";
    }
    
    return "info"; // Default
}

// Send log message to agent
void send_log_to_agent(const char *level, const char *message, const char *file, int line) {
    if (!OPA_G(enabled) || !OPA_G(track_logs)) {
        return;
    }
    
    // Check if this log level should be tracked
    if (!should_track_log_level(level)) {
        return;
    }
    
    // Get current trace/span IDs
    char *trace_id = root_span_trace_id ? root_span_trace_id : generate_id();
    char *span_id = root_span_span_id ? root_span_span_id : generate_id();
    
    // Build log JSON
    smart_string json = {0};
    smart_string_appends(&json, "{\"type\":\"log\",");
    smart_string_appends(&json, "\"trace_id\":\"");
    smart_string_appends(&json, trace_id);
    smart_string_appends(&json, "\",\"span_id\":");
    if (span_id) {
        smart_string_appends(&json, "\"");
        smart_string_appends(&json, span_id);
        smart_string_appends(&json, "\"");
    } else {
        smart_string_appends(&json, "null");
    }
    smart_string_appends(&json, ",\"id\":\"");
    char *log_id = generate_id();
    smart_string_appends(&json, log_id);
    efree(log_id);
    smart_string_appends(&json, "\",\"level\":\"");
    smart_string_appends(&json, level);
    smart_string_appends(&json, "\",\"message\":\"");
    if (message) {
        json_escape_string(&json, message, strlen(message));
    }
    smart_string_appends(&json, "\"");
    
    // Add metadata
    smart_string_appends(&json, ",\"service\":\"");
    if (OPA_G(service)) {
        json_escape_string(&json, OPA_G(service), strlen(OPA_G(service)));
    } else {
        smart_string_appends(&json, "php-fpm");
    }
    smart_string_appends(&json, "\""); // Close service string
    
    // Add timestamp
    long timestamp_ms = get_timestamp_ms();
    char ts_str[64];
    snprintf(ts_str, sizeof(ts_str), "%ld", timestamp_ms);
    smart_string_appends(&json, ",\"timestamp_ms\":");
    smart_string_appends(&json, ts_str);
    
    // Add fields (file and line if available)
    smart_string_appends(&json, ",\"fields\":{");
    int fields_first = 1; // Track first field in fields object
    if (file) {
        smart_string_appends(&json, "\"file\":\"");
        json_escape_string(&json, file, strlen(file));
        smart_string_appends(&json, "\"");
        fields_first = 0;
    }
    if (line > 0) {
        if (!fields_first) {
            smart_string_appends(&json, ",");
        }
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "\"line\":%d", line);
        smart_string_appends(&json, line_str);
        fields_first = 0;
    }
    smart_string_appends(&json, "}");
    
    smart_string_appends(&json, "}");
    smart_string_0(&json);
    
    if (json.c && json.len > 0) {
        char *msg = emalloc(json.len + 1);
        if (msg) {
            memcpy(msg, json.c, json.len);
            msg[json.len] = '\0';
            send_message_direct(msg, 1); // Send with compression
        }
        smart_string_free(&json);
    } else {
        smart_string_free(&json);
    }
    
    if (trace_id != root_span_trace_id) efree(trace_id);
    if (span_id != root_span_span_id) efree(span_id);
}

// Note: Error handler registration in PHP 8.4 is complex
// We rely on PHP applications to call opa_track_error() from their error handlers
// or use shutdown functions for fatal errors

// Store original error_log function
static zend_function *orig_error_log_func = NULL;
static zif_handler orig_error_log_handler = NULL;

// Wrapper for error_log() function
static void zif_opa_error_log(zend_execute_data *execute_data, zval *return_value) {
    // Track log if enabled - get message from first parameter
    if (OPA_G(enabled) && OPA_G(track_logs) && ZEND_CALL_NUM_ARGS(execute_data) > 0) {
        zval *message_zv = ZEND_CALL_ARG(execute_data, 1);
        if (message_zv && Z_TYPE_P(message_zv) == IS_STRING) {
            const char *level = parse_log_level(Z_STRVAL_P(message_zv));
            const char *file = zend_get_executed_filename();
            uint32_t line = zend_get_executed_lineno();
            send_log_to_agent(level, Z_STRVAL_P(message_zv), file, (int)line);
        }
    }
    
    // Call original error_log function
    if (orig_error_log_handler) {
        orig_error_log_handler(execute_data, return_value);
    }
}

// Initialize error tracking
void opa_init_error_tracking(void) {
    if (!OPA_G(enabled)) {
        return;
    }
    
    // Hook error_log() function if log tracking is enabled
    if (OPA_G(track_logs)) {
        orig_error_log_func = zend_hash_str_find_ptr(CG(function_table), "error_log", sizeof("error_log")-1);
        if (orig_error_log_func && orig_error_log_func->type == ZEND_INTERNAL_FUNCTION) {
            orig_error_log_handler = orig_error_log_func->internal_function.handler;
            orig_error_log_func->internal_function.handler = zif_opa_error_log;
            if (OPA_G(debug_log_enabled)) {
                debug_log("[OPA] error_log() hook registered");
            }
        }
    }
    
    // Note: Error handler registration is complex in PHP 8.4
    // We rely on the PHP application to call opa_track_error() from their error handlers
    // or use the shutdown function approach for fatal errors
}

// Cleanup error tracking
void opa_cleanup_error_tracking(void) {
    // Restore original error_log handler
    if (orig_error_log_func && orig_error_log_handler) {
        orig_error_log_func->internal_function.handler = orig_error_log_handler;
        orig_error_log_func = NULL;
        orig_error_log_handler = NULL;
    }
}

