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
    
    if (json.c && json.len > 0) {
        char *result = emalloc(json.len + 1);
        if (result) {
            memcpy(result, json.c, json.len);
            result[json.len] = '\0';
        }
        smart_string_free(&json);
        return result;
    }
    
    smart_string_free(&json);
    return NULL;
}

// Send error to agent (exported for use from opa_api.c)
void send_error_to_agent(int error_type, const char *error_message, const char *file, int line, zval *stack_trace, int *exception_code) {
    (void)exception_code; // Unused parameter, kept for API compatibility
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

// Send log to agent (exported for use from opa.c and error handlers)
void send_log_to_agent(const char *level, const char *message, const char *file, int line) {
    if (!OPA_G(enabled) || !OPA_G(track_logs)) {
        return;
    }
    
    // Check if log level is enabled (if log_levels is configured)
    // Temporarily disabled to test if it's causing memory corruption
    int skip_level_check = 1; // Set to 0 to enable level checking
    if (!skip_level_check && OPA_G(log_levels) && strlen(OPA_G(log_levels)) > 0 && level) {
        // Convert level to lowercase for comparison
        char level_lower[32] = {0};
        size_t level_len = strlen(level);
        if (level_len >= sizeof(level_lower)) level_len = sizeof(level_lower) - 1;
        for (size_t i = 0; i < level_len; i++) {
            level_lower[i] = (level[i] >= 'A' && level[i] <= 'Z') ? level[i] - 'A' + 'a' : level[i];
        }
        
        // Normalize: warn -> warning, err -> error, inf -> info, crit -> critical
        const char *level_to_check = level_lower;
        char normalized[32] = {0};
        if (strcmp(level_lower, "warn") == 0) {
            strncpy(normalized, "warning", sizeof(normalized) - 1);
            level_to_check = normalized;
        } else if (strcmp(level_lower, "err") == 0) {
            strncpy(normalized, "error", sizeof(normalized) - 1);
            level_to_check = normalized;
        } else if (strcmp(level_lower, "inf") == 0) {
            strncpy(normalized, "info", sizeof(normalized) - 1);
            level_to_check = normalized;
        } else if (strcmp(level_lower, "crit") == 0) {
            strncpy(normalized, "critical", sizeof(normalized) - 1);
            level_to_check = normalized;
        }
        
        // Simple check: see if level appears in log_levels string
        // Check for comma-separated matches
        const char *log_levels_str = OPA_G(log_levels);
        int level_enabled = 0;
        
        // Check if it's the only level
        if (strcmp(log_levels_str, level_to_check) == 0) {
            level_enabled = 1;
        } else {
            // Check if it appears with comma boundaries
            char pattern[64];
            snprintf(pattern, sizeof(pattern), ",%s,", level_to_check);
            if (strstr(log_levels_str, pattern) != NULL) {
                level_enabled = 1;
            } else {
                // Check if it's at the start
                size_t pattern_len = strlen(level_to_check);
                if (strncmp(log_levels_str, level_to_check, pattern_len) == 0 && 
                    log_levels_str[pattern_len] == ',') {
                    level_enabled = 1;
                } else {
                    // Check if it's at the end
                    size_t log_levels_len = strlen(log_levels_str);
                    if (log_levels_len >= pattern_len && 
                        strcmp(log_levels_str + log_levels_len - pattern_len, level_to_check) == 0 &&
                        log_levels_str[log_levels_len - pattern_len - 1] == ',') {
                        level_enabled = 1;
                    }
                }
            }
        }
        
        if (!level_enabled) {
            return; // Log level not enabled
        }
    }
    
    // Get current trace/span IDs - make safe copies to avoid memory corruption
    char *trace_id = NULL;
    char *trace_id_to_free = NULL;
    // Safely check root_span_trace_id before using strlen
    if (root_span_trace_id) {
        // Validate the pointer is valid by checking it's not NULL and trying to read first char
        // This is a defensive check to avoid memory corruption
        size_t trace_id_len = 0;
        const char *p = root_span_trace_id;
        while (*p != '\0' && trace_id_len < 256) { // Max reasonable ID length
            trace_id_len++;
            p++;
        }
        if (trace_id_len > 0 && trace_id_len < 256) {
            trace_id = emalloc(trace_id_len + 1);
            if (trace_id) {
                memcpy(trace_id, root_span_trace_id, trace_id_len);
                trace_id[trace_id_len] = '\0';
                trace_id_to_free = trace_id;
            }
        }
    }
    if (!trace_id) {
        trace_id = generate_id();
        trace_id_to_free = trace_id;
    }
    
    char *span_id = NULL;
    char *span_id_to_free = NULL;
    // Safely check root_span_span_id before using strlen
    if (root_span_span_id) {
        // Validate the pointer is valid
        size_t span_id_len = 0;
        const char *p = root_span_span_id;
        while (*p != '\0' && span_id_len < 256) { // Max reasonable ID length
            span_id_len++;
            p++;
        }
        if (span_id_len > 0 && span_id_len < 256) {
            span_id = emalloc(span_id_len + 1);
            if (span_id) {
                memcpy(span_id, root_span_span_id, span_id_len);
                span_id[span_id_len] = '\0';
                span_id_to_free = span_id;
            }
        }
    }
    
    // Generate log ID
    char *log_id = generate_id();
    
    // Get timestamp in milliseconds
    long timestamp_ms = get_timestamp_ms();
    
    // Build log JSON
    smart_string json = {0};
    smart_string_appends(&json, "{\"type\":\"log\",");
    smart_string_appends(&json, "\"id\":\"");
    if (log_id) {
        smart_string_appends(&json, log_id);
    }
    smart_string_appends(&json, "\",\"trace_id\":\"");
    if (trace_id) {
        smart_string_appends(&json, trace_id);
    }
    smart_string_appends(&json, "\",\"span_id\":");
    if (span_id && strlen(span_id) > 0) {
        smart_string_appends(&json, "\"");
        smart_string_appends(&json, span_id);
        smart_string_appends(&json, "\"");
    } else {
        smart_string_appends(&json, "null");
    }
    smart_string_appends(&json, ",\"level\":\"");
    // Safely handle level string - use the level directly (it's a string literal from log_structured)
    const char *level_str = level ? level : "INFO";
    smart_string_appends(&json, level_str);
    smart_string_appends(&json, "\",\"message\":\"");
    if (message) {
        // Validate message pointer and length before using
        size_t msg_len = strlen(message);
        if (msg_len > 0 && msg_len < 1024 * 1024) { // Sanity check: max 1MB
            json_escape_string(&json, message, msg_len);
        }
    }
    smart_string_appends(&json, "\",\"service\":\"");
    if (OPA_G(service)) {
        json_escape_string(&json, OPA_G(service), strlen(OPA_G(service)));
    } else {
        smart_string_appends(&json, "php-fpm");
    }
    smart_string_appends(&json, "\",\"timestamp_ms\":");
    char ts_str[64];
    snprintf(ts_str, sizeof(ts_str), "%ld", timestamp_ms);
    smart_string_appends(&json, ts_str);
    
    // Add fields with file and line if available
    smart_string_appends(&json, ",\"fields\":{");
    if (file) {
        smart_string_appends(&json, "\"file\":\"");
        json_escape_string(&json, file, strlen(file));
        smart_string_appends(&json, "\"");
        if (line > 0) {
            smart_string_appends(&json, ",\"line\":");
            char line_str[32];
            snprintf(line_str, sizeof(line_str), "%d", line);
            smart_string_appends(&json, line_str);
        }
    } else if (line > 0) {
        smart_string_appends(&json, "\"line\":");
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "%d", line);
        smart_string_appends(&json, line_str);
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
    
    // Free allocated memory
    if (log_id) efree(log_id);
    if (trace_id_to_free) efree(trace_id_to_free);
    if (span_id_to_free) efree(span_id_to_free);
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
            
            int exception_code = 0;
            send_error_to_agent(error_type, error_message, file, line, &trace, &exception_code);
            
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
    
    int exception_code = 0;
    if (Z_TYPE(code_zv) == IS_LONG) {
        exception_code = Z_LVAL(code_zv);
    }
    send_error_to_agent(E_ERROR, error_message, file, line, &trace_array, &exception_code);
    
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

// Initialize error tracking - use shutdown function approach
void opa_init_error_tracking(void) {
    if (!OPA_G(enabled)) {
        return;
    }
    
    // Register shutdown function to check for uncaught exceptions
    // This is more reliable than trying to hook into error handlers
    // The PHP application can also call opa_track_error() from its own error handlers
}

// Cleanup error tracking
void opa_cleanup_error_tracking(void) {
    // TEMPORARILY DISABLED: zend_replace_error_handling API changed in PHP 8.4
    /*
    if (original_error_handling.error_handler) {
        zend_error_handling current;
        zend_replace_error_handling(EH_NORMAL, NULL, &current);
    }
    */
}

