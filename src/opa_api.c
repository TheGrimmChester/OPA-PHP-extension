#include "opa.h"
#include "span.h"
#include "transport.h"
#include "serialize.h"

// Creates a new manual span and returns its span_id
// Manual spans allow programmatic tracing of specific operations
// Tags are optional key-value pairs for additional context
PHP_FUNCTION(opa_start_span) {
    char *name;
    size_t name_len;
    zval *tags = NULL;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|a", &name, &name_len, &tags) == FAILURE) {
        return;
    }
    
    HashTable *spans = get_active_spans();
    
    char *span_id = generate_id();
    char *trace_id = generate_id();
    span_context_t *span = create_span_context(span_id, trace_id, NULL);
    // create_span_context does estrdup, so free original IDs
    efree(span_id);
    efree(trace_id);
    span->start_ts = get_timestamp_ms();
    if (span->name) efree(span->name); // Free the NULL from create_span_context
    span->name = estrndup(name, name_len);
    span->is_manual = 1;
    span->status = -1;
    
    // Convert zval array to span_tag_t linked list
    if (tags && Z_TYPE_P(tags) == IS_ARRAY) {
        HashTable *ht = Z_ARRVAL_P(tags);
        zend_string *key;
        zval *val;
        
        ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
            // Convert key to string
            char *key_str = NULL;
            char key_buf[64] = {0};
            if (key) {
                key_str = ZSTR_VAL(key);
            } else {
                // Numeric key - convert to string
                zend_ulong num_key;
                zend_hash_get_current_key(ht, NULL, &num_key);
                snprintf(key_buf, sizeof(key_buf), "%lu", num_key);
                key_str = key_buf;
            }
            
            if (!key_str) continue;
            
            // Convert value to string
            char *value_str = NULL;
            size_t value_len = 0;
            char value_buf[128] = {0};
            smart_string json_buf = {0};
            int needs_json_free = 0;
            
            if (Z_TYPE_P(val) == IS_STRING) {
                value_str = Z_STRVAL_P(val);
                value_len = Z_STRLEN_P(val);
            } else if (Z_TYPE_P(val) == IS_LONG) {
                snprintf(value_buf, sizeof(value_buf), "%ld", Z_LVAL_P(val));
                value_str = value_buf;
                value_len = strlen(value_buf);
            } else if (Z_TYPE_P(val) == IS_DOUBLE) {
                snprintf(value_buf, sizeof(value_buf), "%.6f", Z_DVAL_P(val));
                value_str = value_buf;
                value_len = strlen(value_buf);
            } else if (Z_TYPE_P(val) == IS_TRUE) {
                value_str = "true";
                value_len = 4;
            } else if (Z_TYPE_P(val) == IS_FALSE) {
                value_str = "false";
                value_len = 5;
            } else if (Z_TYPE_P(val) == IS_NULL) {
                value_str = "null";
                value_len = 4;
            } else {
                // For other types, serialize to JSON
                serialize_zval_json(&json_buf, val);
                smart_string_0(&json_buf);
                if (json_buf.c && json_buf.len > 0) {
                    value_str = json_buf.c;
                    value_len = json_buf.len;
                    needs_json_free = 1;
                } else {
                    smart_string_free(&json_buf);
                    continue;
                }
            }
            
            if (value_str && value_len > 0) {
                // Create tag with malloc'd strings
                span_tag_t *tag = malloc(sizeof(span_tag_t));
                if (tag) {
                    // Copy key
                    if (key && ZSTR_LEN(key) > 0) {
                        tag->key = strdup(ZSTR_VAL(key));
                    } else {
                        tag->key = strdup(key_str);
                    }
                    
                    // Copy value (always malloc, even for string types)
                    tag->value = malloc(value_len + 1);
                    if (tag->key && tag->value) {
                        memcpy(tag->value, value_str, value_len);
                        tag->value[value_len] = '\0';
                        
                        tag->next = span->tags;
                        span->tags = tag;
                    } else {
                        // Allocation failed, clean up
                        if (tag->key) free(tag->key);
                        if (tag->value) free(tag->value);
                        free(tag);
                    }
                }
            }
            
            // Free smart_string if used
            if (needs_json_free && json_buf.c) {
                smart_string_free(&json_buf);
            }
        } ZEND_HASH_FOREACH_END();
    }
    
    zend_string *key = zend_string_init(span->span_id, strlen(span->span_id), 0);
    zend_hash_add_ptr(spans, key, span);
    zend_string_release(key);
    
    RETURN_STRING(span->span_id);
}

// Finalizes a manual span by setting end timestamp and sending it to the agent
// The span must have been created with opa_start_span()
PHP_FUNCTION(opa_end_span) {
    char *span_id;
    size_t span_id_len;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &span_id, &span_id_len) == FAILURE) {
        return;
    }
    
    HashTable *spans = get_active_spans();
    if (!spans) {
        RETURN_FALSE;
    }
    
    zend_string *key = zend_string_init(span_id, span_id_len, 0);
    span_context_t *span = zend_hash_find_ptr(spans, key);
    
    if (span) {
        span->end_ts = get_timestamp_ms();
        span->status = 1;
        
        // Produce JSON and send - use malloc'd string (safe after fastcgi_finish_request)
        char *msg = produce_span_json(span);
        if (msg) {
            // Convert malloc'd string to emalloc'd for send_message_direct
            // (send_message_direct expects emalloc'd and will efree it)
            char *msg_copy = estrdup(msg);
            free(msg); // Free malloc'd version
            send_message_direct(msg_copy, 1); // Send directly
        }
        
        zend_hash_del(spans, key);
        free_span_context(span);
        zend_string_release(key);
        
        RETURN_TRUE;
    }
    
    zend_string_release(key);
    RETURN_FALSE;
}

// Adds a key-value tag to an existing span for additional metadata
// Useful for adding context like user_id, request_id, etc.
PHP_FUNCTION(opa_add_tag) {
    char *span_id, *key, *value;
    size_t span_id_len, key_len, value_len;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss", &span_id, &span_id_len, &key, &key_len, &value, &value_len) == FAILURE) {
        return;
    }
    
    // CRITICAL: Check profiling_active to avoid accessing memory during/after RSHUTDOWN
    if (!profiling_active) {
        RETURN_FALSE;
    }
    
    // CRITICAL: Use mutex to protect span access and prevent race conditions with RSHUTDOWN
    pthread_mutex_lock(&active_spans_mutex);
    
    // Re-check profiling_active after acquiring mutex (RSHUTDOWN might have disabled it)
    if (!profiling_active) {
        pthread_mutex_unlock(&active_spans_mutex);
        RETURN_FALSE;
    }
    
    HashTable *spans = get_active_spans();
    if (!spans) {
        pthread_mutex_unlock(&active_spans_mutex);
        RETURN_FALSE;
    }
    
    zend_string *skey = zend_string_init(span_id, span_id_len, 0);
    span_context_t *span = zend_hash_find_ptr(spans, skey);
    
    // CRITICAL: Verify span is still in hash table (might have been removed by RSHUTDOWN)
    // Re-check to ensure span pointer is still valid
    if (!span || !profiling_active) {
        zend_string_release(skey);
        pthread_mutex_unlock(&active_spans_mutex);
        RETURN_FALSE;
    }
    
    // Verify span is still in the hash table by checking again
    span_context_t *span_verify = zend_hash_find_ptr(spans, skey);
    if (span != span_verify || !profiling_active) {
        zend_string_release(skey);
        pthread_mutex_unlock(&active_spans_mutex);
        RETURN_FALSE;
    }
    
    zend_string_release(skey);
    
    // Add tag using persistent tag storage (malloc'd, safe after request cleanup)
    span_add_tag(span, key, value);
    
    pthread_mutex_unlock(&active_spans_mutex);
    RETURN_TRUE;
}

// Sets the parent span for a manual span to establish trace hierarchy
// Allows creating custom parent-child relationships in traces
PHP_FUNCTION(opa_set_parent) {
    char *span_id, *parent_id;
    size_t span_id_len, parent_id_len;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &span_id, &span_id_len, &parent_id, &parent_id_len) == FAILURE) {
        return;
    }
    
    HashTable *spans = get_active_spans();
    if (!spans) {
        RETURN_FALSE;
    }
    
    zend_string *skey = zend_string_init(span_id, span_id_len, 0);
    span_context_t *span = zend_hash_find_ptr(spans, skey);
    zend_string_release(skey);
    
    if (span) {
        if (span->parent_id) efree(span->parent_id);
        span->parent_id = estrndup(parent_id, parent_id_len);
        RETURN_TRUE;
    }
    
    RETURN_FALSE;
}

// Captures variable dumps and attaches them to the current active span
// Accepts any number of variables and serializes them for debugging
// Dumps are stored in the span's dumps array and sent to the agent
PHP_FUNCTION(dump) {
    if (ZEND_NUM_ARGS() == 0) {
        return; // No arguments, nothing to dump
    }
    
    // Get current active span
    span_context_t *span = NULL;
    HashTable *spans = get_active_spans();
    
    // Try to get the first active span (most recent)
    if (spans && zend_hash_num_elements(spans) > 0) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(spans, key, val) {
            span = (span_context_t *)val;
            break; // Get first span
        } ZEND_HASH_FOREACH_END();
    }
    
    // If no active span, use root span dumps (stored separately)
    zval *target_dumps = NULL;
    
    if (span) {
        // Initialize dumps array if needed
        if (!span->dumps) {
            span->dumps = emalloc(sizeof(zval));
            array_init(span->dumps);
        }
        target_dumps = span->dumps;
        debug_log("[dump] Using active span dumps, span_id=%s", span->span_id ? span->span_id : "NULL");
    } else {
        // Use root span dumps
        // Ensure root span exists - if not, it will be created on first function call
        // But we can still initialize dumps array now
        pthread_mutex_lock(&root_span_data_mutex);
        
        // Initialize root span if it doesn't exist yet (will be finalized in opa_execute_ex)
        if (!root_span_span_id) {
            // Create minimal root span data - will be updated when first function is called
            root_span_span_id = strdup(generate_id());
            root_span_trace_id = strdup(generate_id());
            root_span_start_ts = get_timestamp_ms();
            
            // Try to extract real URI from $_SERVER instead of using generic name
            zval *server = NULL;
            char *span_name = NULL;
            if ((server = zend_hash_str_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER")-1)) != NULL) {
                if (Z_TYPE_P(server) == IS_ARRAY) {
                    zval *request_uri = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
                    if (request_uri && Z_TYPE_P(request_uri) == IS_STRING && Z_STRLEN_P(request_uri) > 0) {
                        zval *request_method = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
                        if (request_method && Z_TYPE_P(request_method) == IS_STRING && Z_STRLEN_P(request_method) > 0) {
                            size_t name_len = Z_STRLEN_P(request_method) + 1 + Z_STRLEN_P(request_uri) + 1;
                            span_name = malloc(name_len);
                            if (span_name) {
                                snprintf(span_name, name_len, "%s %s", Z_STRVAL_P(request_method), Z_STRVAL_P(request_uri));
                            }
                        } else {
                            span_name = strdup(Z_STRVAL_P(request_uri));
                        }
                    } else {
                        zval *script_name = zend_hash_str_find(Z_ARRVAL_P(server), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
                        if (script_name && Z_TYPE_P(script_name) == IS_STRING && Z_STRLEN_P(script_name) > 0) {
                            span_name = strdup(Z_STRVAL_P(script_name));
                        }
                    }
                }
            }
            root_span_name = span_name ? span_name : strdup("PHP Request");
            debug_log("[dump] Created root span early, span_id=%s, name=%s", root_span_span_id, root_span_name);
        }
        
        // Initialize dumps array if needed (should already be initialized in RINIT, but check anyway)
        if (!root_span_dumps) {
            root_span_dumps = emalloc(sizeof(zval));
            array_init(root_span_dumps);
            debug_log("[dump] Initialized root_span_dumps (late initialization)");
        }
        target_dumps = root_span_dumps;
        
        int dumps_count = target_dumps && Z_TYPE_P(target_dumps) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(target_dumps)) : 0;
        debug_log("[dump] Using root span dumps, span_id=%s, dumps_count=%d", root_span_span_id, dumps_count);
    }
    
    // Get current file and line
    const char *file_str = zend_get_executed_filename();
    if (!file_str) {
        file_str = "unknown";
    }
    uint32_t lineno = zend_get_executed_lineno();
    long timestamp = get_timestamp_ms();
    
    // Get current execution context
    zend_execute_data *ex_data = EG(current_execute_data);
    if (!ex_data) {
        if (span == NULL) {
            pthread_mutex_unlock(&root_span_data_mutex);
        }
        RETURN_NULL();
    }
    
    // Process each argument
    int arg_count = ZEND_NUM_ARGS();
    
    for (int i = 0; i < arg_count; i++) {
        zval *arg = ZEND_CALL_VAR_NUM(ex_data, i);
        if (!arg) {
            continue; // Skip invalid arguments
        }
        
        // Create dump entry
        zval dump_entry;
        array_init(&dump_entry);
        
        // Add timestamp
        add_assoc_long(&dump_entry, "timestamp", timestamp);
        
        // Add file
        add_assoc_string(&dump_entry, "file", (char *)file_str);
        
        // Add line
        add_assoc_long(&dump_entry, "line", (zend_long)lineno);
        
        // Serialize to JSON
        smart_string json_buf = {0};
        serialize_zval_json(&json_buf, arg);
        smart_string_0(&json_buf);
        
        // Store JSON as string in dump entry (data field contains the JSON string)
        // In PHP 8.4, smart_string uses .c for data and .len for length
        if (json_buf.c && json_buf.len > 0) {
            add_assoc_stringl(&dump_entry, "data", json_buf.c, json_buf.len);
        } else {
            add_assoc_string(&dump_entry, "data", "null");
        }
        
        // Serialize to text
        smart_string text_buf = {0};
        serialize_zval_text(&text_buf, arg);
        smart_string_0(&text_buf);
        
        if (text_buf.c && text_buf.len > 0) {
            add_assoc_stringl(&dump_entry, "text", text_buf.c, text_buf.len);
        } else {
            add_assoc_string(&dump_entry, "text", "");
        }
        
        // Add dump entry to dumps array
        // If using root span dumps, we need to keep the mutex locked
        add_next_index_zval(target_dumps, &dump_entry);
        int total_dumps = target_dumps && Z_TYPE_P(target_dumps) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(target_dumps)) : 0;
        debug_log("[dump] Added dump entry %d, total dumps=%d, span_id=%s", i, total_dumps, root_span_span_id ? root_span_span_id : "NULL");
        
        // Free smart strings
        if (json_buf.c) {
            smart_string_free(&json_buf);
        }
        if (text_buf.c) {
            smart_string_free(&text_buf);
        }
    }
    
    // Unlock mutex if we were using root span dumps
    if (span == NULL) {
        pthread_mutex_unlock(&root_span_data_mutex);
    }
    
    // Return NULL silently - no output to user, data is sent to agent via span
    RETURN_NULL();
}

// Alias for dump() - same functionality, different name
PHP_FUNCTION(opa_dump) {
    if (ZEND_NUM_ARGS() == 0) {
        return; // No arguments, nothing to dump
    }
    
    // Get current active span
    span_context_t *span = NULL;
    HashTable *spans = get_active_spans();
    
    // Try to get the first active span (most recent)
    if (spans && zend_hash_num_elements(spans) > 0) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(spans, key, val) {
            span = (span_context_t *)val;
            break; // Get first span
        } ZEND_HASH_FOREACH_END();
    }
    
    // If no active span, use root span dumps (stored separately)
    zval *target_dumps = NULL;
    
    if (span) {
        // Initialize dumps array if needed
        if (!span->dumps) {
            span->dumps = emalloc(sizeof(zval));
            array_init(span->dumps);
        }
        target_dumps = span->dumps;
        debug_log("[opa_dump] Using active span dumps, span_id=%s", span->span_id ? span->span_id : "NULL");
    } else {
        // Use root span dumps
        // Ensure root span exists - if not, it will be created on first function call
        // But we can still initialize dumps array now
        pthread_mutex_lock(&root_span_data_mutex);
        
        // Initialize root span if it doesn't exist yet (will be finalized in opa_execute_ex)
        if (!root_span_span_id) {
            // Create minimal root span data - will be updated when first function is called
            root_span_span_id = strdup(generate_id());
            root_span_trace_id = strdup(generate_id());
            root_span_start_ts = get_timestamp_ms();
            
            // Try to extract real URI from $_SERVER instead of using generic name
            zval *server = NULL;
            char *span_name = NULL;
            if ((server = zend_hash_str_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER")-1)) != NULL) {
                if (Z_TYPE_P(server) == IS_ARRAY) {
                    zval *request_uri = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
                    if (request_uri && Z_TYPE_P(request_uri) == IS_STRING && Z_STRLEN_P(request_uri) > 0) {
                        zval *request_method = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
                        if (request_method && Z_TYPE_P(request_method) == IS_STRING && Z_STRLEN_P(request_method) > 0) {
                            size_t name_len = Z_STRLEN_P(request_method) + 1 + Z_STRLEN_P(request_uri) + 1;
                            span_name = malloc(name_len);
                            if (span_name) {
                                snprintf(span_name, name_len, "%s %s", Z_STRVAL_P(request_method), Z_STRVAL_P(request_uri));
                            }
                        } else {
                            span_name = strdup(Z_STRVAL_P(request_uri));
                        }
                    } else {
                        zval *script_name = zend_hash_str_find(Z_ARRVAL_P(server), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
                        if (script_name && Z_TYPE_P(script_name) == IS_STRING && Z_STRLEN_P(script_name) > 0) {
                            span_name = strdup(Z_STRVAL_P(script_name));
                        }
                    }
                }
            }
            root_span_name = span_name ? span_name : strdup("PHP Request");
            debug_log("[opa_dump] Created root span early, span_id=%s, name=%s", root_span_span_id, root_span_name);
        }
        
        // Initialize dumps array if needed (should already be initialized in RINIT, but check anyway)
        if (!root_span_dumps) {
            root_span_dumps = emalloc(sizeof(zval));
            array_init(root_span_dumps);
            debug_log("[opa_dump] Initialized root_span_dumps (late initialization)");
        }
        target_dumps = root_span_dumps;
        
        int dumps_count = target_dumps && Z_TYPE_P(target_dumps) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(target_dumps)) : 0;
        debug_log("[opa_dump] Using root span dumps, span_id=%s, dumps_count=%d", root_span_span_id, dumps_count);
    }
    
    // Get current file and line
    const char *file_str = zend_get_executed_filename();
    if (!file_str) {
        file_str = "unknown";
    }
    uint32_t lineno = zend_get_executed_lineno();
    long timestamp = get_timestamp_ms();
    
    // Get current execution context
    zend_execute_data *ex_data = EG(current_execute_data);
    if (!ex_data) {
        if (span == NULL) {
            pthread_mutex_unlock(&root_span_data_mutex);
        }
        RETURN_NULL();
    }
    
    // Process each argument
    int arg_count = ZEND_NUM_ARGS();
    
    for (int i = 0; i < arg_count; i++) {
        zval *arg = ZEND_CALL_VAR_NUM(ex_data, i);
        if (!arg) {
            continue; // Skip invalid arguments
        }
        
        // Create dump entry
        zval dump_entry;
        array_init(&dump_entry);
        
        // Add timestamp
        add_assoc_long(&dump_entry, "timestamp", timestamp);
        
        // Add file
        add_assoc_string(&dump_entry, "file", (char *)file_str);
        
        // Add line
        add_assoc_long(&dump_entry, "line", (zend_long)lineno);
        
        // Serialize to JSON
        smart_string json_buf = {0};
        serialize_zval_json(&json_buf, arg);
        smart_string_0(&json_buf);
        
        // Store JSON as string in dump entry (data field contains the JSON string)
        // In PHP 8.4, smart_string uses .c for data and .len for length
        if (json_buf.c && json_buf.len > 0) {
            add_assoc_stringl(&dump_entry, "data", json_buf.c, json_buf.len);
        } else {
            add_assoc_string(&dump_entry, "data", "null");
        }
        
        // Serialize to text
        smart_string text_buf = {0};
        serialize_zval_text(&text_buf, arg);
        smart_string_0(&text_buf);
        
        if (text_buf.c && text_buf.len > 0) {
            add_assoc_stringl(&dump_entry, "text", text_buf.c, text_buf.len);
        } else {
            add_assoc_string(&dump_entry, "text", "");
        }
        
        // Add dump entry to dumps array
        // If using root span dumps, we need to keep the mutex locked
        add_next_index_zval(target_dumps, &dump_entry);
        int total_dumps = target_dumps && Z_TYPE_P(target_dumps) == IS_ARRAY ? zend_hash_num_elements(Z_ARRVAL_P(target_dumps)) : 0;
        debug_log("[opa_dump] Added dump entry %d, total dumps=%d, span_id=%s", i, total_dumps, root_span_span_id ? root_span_span_id : "NULL");
        
        // Free smart strings
        if (json_buf.c) {
            smart_string_free(&json_buf);
        }
        if (text_buf.c) {
            smart_string_free(&text_buf);
        }
    }
    
    // Unlock mutex if we were using root span dumps
    if (span == NULL) {
        pthread_mutex_unlock(&root_span_data_mutex);
    }
    
    // Return NULL silently - no output to user, data is sent to agent via span
    RETURN_NULL();
}

// Enables profiling for the current request
// Can be called at runtime to enable profiling even if it was disabled at request start
PHP_FUNCTION(opa_enable) {
    
    // Set profiling active flag
    profiling_active = 1;
    
    // Initialize collector if not already initialized
    if (!global_collector) {
        global_collector = opa_collector_init();
    }
    
    // Start collector if it exists
    if (global_collector) {
        opa_collector_start(global_collector);
    }
    
    RETURN_TRUE;
}

// Disables profiling for the current request
// Can be called at runtime to disable profiling even if it was enabled at request start
PHP_FUNCTION(opa_disable) {
    
    // Set profiling inactive flag
    profiling_active = 0;
    
    // Stop collector but don't free it (in case re-enabled later in same request)
    if (global_collector) {
        opa_collector_stop(global_collector);
    }
    
    RETURN_TRUE;
}

// Returns whether profiling is currently enabled for the current request
PHP_FUNCTION(opa_is_enabled) {
    
    RETURN_BOOL(profiling_active != 0);
}

// Error tracking function - called from PHP userland error handlers
PHP_FUNCTION(opa_track_error) {
    zend_string *error_type = NULL;
    zend_string *error_message = NULL;
    zend_string *file = NULL;
    zend_long line = 0;
    zval *stack_trace = NULL;
    
    ZEND_PARSE_PARAMETERS_START(2, 5)
        Z_PARAM_STR(error_type)
        Z_PARAM_STR(error_message)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(file)
        Z_PARAM_LONG(line)
        Z_PARAM_ARRAY_OR_NULL(stack_trace)
    ZEND_PARSE_PARAMETERS_END();
    
    // Call the error tracking function from error_tracking.c
    extern void send_error_to_agent(int error_type, const char *error_message, const char *file, int line, zval *stack_trace, int *exception_code);
    
    int exception_code = 0; // Regular errors don't have exception codes
    send_error_to_agent(
        E_ERROR, // Default to E_ERROR for userland errors
        error_message ? ZSTR_VAL(error_message) : NULL,
        file ? ZSTR_VAL(file) : NULL,
        (int)line,
        stack_trace,
        &exception_code
    );
}
