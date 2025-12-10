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

