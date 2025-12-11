#include "call_node.h"

// Records a SQL query execution in the current function call's context
// Tracks query text, duration, type, affected rows, database hostname, database system, and DSN for performance analysis
void record_sql_query(const char *sql, double duration, zval *params, const char *query_type, int rows_affected, const char *db_host, const char *db_system, const char *db_dsn) {
    if (!OPA_G(enabled) || !profiling_active) return;
    
    extern opa_collector_t *global_collector;
    
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Get the currently executing function call from the collector
    call_node_t *current_call = NULL;
    if (global_collector->call_stack_top) {
        current_call = global_collector->call_stack_top;
    } else {
        // No active call stack - SQL queries executed at top level
        // Create a root call node to attach SQL queries to
        debug_log("[record_sql_query] No call stack, creating root call node for SQL query");
        char *root_call_id = opa_enter_function("__root__", NULL, __FILE__, __LINE__, 0);
        if (root_call_id) {
            efree(root_call_id);
            current_call = global_collector->call_stack_top;
        }
    }
    
    // Lazily allocate sql_queries array when first SQL query is recorded
    if (current_call) {
        if (!current_call->sql_queries) {
            current_call->sql_queries = ecalloc(1, sizeof(zval));
            if (current_call->sql_queries) {
                array_init(current_call->sql_queries);
            }
        }
    }
    
    if (current_call && current_call->sql_queries) {
        zval query_data;
        array_init(&query_data);
        
        if (sql) {
            add_assoc_string(&query_data, "query", (char *)sql);
        }
        add_assoc_double(&query_data, "duration", duration);
        add_assoc_double(&query_data, "duration_ms", duration * 1000.0);
        add_assoc_double(&query_data, "timestamp", get_time_seconds() - duration);
        
        if (query_type) {
            add_assoc_string(&query_data, "type", (char *)query_type);
        }
        
        // Skip params for now to avoid refcount issues
        // if (params && Z_TYPE_P(params) == IS_ARRAY) {
        //     zval params_copy;
        //     ZVAL_COPY(&params_copy, params);
        //     add_assoc_zval(&query_data, "params", &params_copy);
        // }
        
               // Always include rows_affected (can be -1 if unknown, 0 for SELECT with no results, or positive for actual count)
               add_assoc_long(&query_data, "rows_affected", rows_affected);
               
               // Also add rows_returned for SELECT queries (same as rows_affected for SELECT)
               if (sql && strncasecmp(sql, "SELECT", 6) == 0 && rows_affected >= 0) {
                   add_assoc_long(&query_data, "rows_returned", rows_affected);
               }
        
        // Determine query type (SELECT, INSERT, etc.)
        if (sql) {
            const char *upper_sql = sql;
            while (*upper_sql && (*upper_sql == ' ' || *upper_sql == '\t' || *upper_sql == '\n')) {
                upper_sql++;
            }
            if (strncasecmp(upper_sql, "SELECT", 6) == 0) {
                add_assoc_string(&query_data, "query_type", "SELECT");
            } else if (strncasecmp(upper_sql, "INSERT", 6) == 0) {
                add_assoc_string(&query_data, "query_type", "INSERT");
            } else if (strncasecmp(upper_sql, "UPDATE", 6) == 0) {
                add_assoc_string(&query_data, "query_type", "UPDATE");
            } else if (strncasecmp(upper_sql, "DELETE", 6) == 0) {
                add_assoc_string(&query_data, "query_type", "DELETE");
            }
        }
        
        // Add database system (mysql, postgresql, etc.) - use provided value or default to "mysql"
        if (db_system && strlen(db_system) > 0) {
            add_assoc_string(&query_data, "db_system", (char *)db_system);
        } else {
            add_assoc_string(&query_data, "db_system", "mysql");
        }
        
        // Add database hostname if available
        if (db_host && strlen(db_host) > 0) {
            add_assoc_string(&query_data, "db_host", (char *)db_host);
        }
        
        // Add database DSN (without password) if available
        if (db_dsn && strlen(db_dsn) > 0) {
            add_assoc_string(&query_data, "db_dsn", (char *)db_dsn);
        }
        
        add_next_index_zval(current_call->sql_queries, &query_data);
        // query_data is copied by add_next_index_zval
        // Don't destroy or modify query_data - it's stack-allocated and will be cleaned up automatically
        // The zval contents are now owned by sql_queries array
        // Just let it go out of scope naturally
    }
}

// Begin request tracking (called from observer_begin)
// This function is currently disabled (#if 0) but kept for future use
// Function disabled - implementation moved to observer_begin (currently #if 0)
void php_opa_begin_request(zend_execute_data *execute_data) {
    // This function is kept as a placeholder for future refactoring
    (void)execute_data;
}

// End request tracking (called from observer_end)
// This function is currently disabled but kept for future use
// Function disabled - implementation moved to observer_end
void php_opa_end_request(zend_execute_data *execute_data, zval *return_value) {
    // This function is kept as a placeholder for future refactoring
    (void)execute_data;
    (void)return_value;
}

// Records an HTTP request (cURL) execution in the current function call's context
// Tracks URL, method, status code, bytes transferred, duration, and any errors
void record_http_request(const char *url, const char *method, int status_code, size_t bytes_sent, size_t bytes_received, double duration, const char *error) {
    if (!OPA_G(enabled) || !profiling_active) return;
    
    extern opa_collector_t *global_collector;
    
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Get the currently executing function call from the collector
    call_node_t *current_call = NULL;
    if (global_collector->call_stack_top) {
        current_call = global_collector->call_stack_top;
    } else {
        // No active call stack - HTTP requests executed at top level
        // Create a root call node to attach HTTP requests to
        char *root_call_id = opa_enter_function("__root__", NULL, __FILE__, __LINE__, 0);
        if (root_call_id) {
            efree(root_call_id);
            current_call = global_collector->call_stack_top;
        }
    }
    
    // Lazily allocate http_requests array when first HTTP request is recorded
    if (current_call) {
        if (!current_call->http_requests) {
            current_call->http_requests = ecalloc(1, sizeof(zval));
            if (current_call->http_requests) {
                array_init(current_call->http_requests);
            }
        }
    }
    
    if (current_call && current_call->http_requests) {
        zval request_data;
        array_init(&request_data);
        
        if (url) {
            add_assoc_string(&request_data, "url", (char *)url);
        }
        if (method) {
            add_assoc_string(&request_data, "method", (char *)method);
        } else {
            add_assoc_string(&request_data, "method", "GET");
        }
        if (status_code > 0) {
            add_assoc_long(&request_data, "status_code", status_code);
        }
        add_assoc_long(&request_data, "bytes_sent", (long)bytes_sent);
        add_assoc_long(&request_data, "bytes_received", (long)bytes_received);
        add_assoc_double(&request_data, "duration", duration);
        add_assoc_double(&request_data, "duration_ms", duration * 1000.0);
        add_assoc_double(&request_data, "timestamp", get_time_seconds() - duration);
        if (error) {
            add_assoc_string(&request_data, "error", (char *)error);
        }
        add_assoc_string(&request_data, "type", "curl");
        
        add_next_index_zval(current_call->http_requests, &request_data);
    }
}

// Enhanced HTTP request recording with additional details
void record_http_request_enhanced(const char *url, const char *method, int status_code, 
    size_t bytes_sent, size_t bytes_received, double duration, const char *error,
    const char *uri_path, const char *query_string, const char *request_headers, 
    const char *response_headers, size_t response_size, size_t request_size, double dns_time, 
    double connect_time, double total_time) {
    
    // Call the basic record_http_request first
    record_http_request(url, method, status_code, bytes_sent, bytes_received, duration, error);
    
    // Then enhance the last added request with additional fields
    if (!OPA_G(enabled) || !profiling_active) return;
    
    extern opa_collector_t *global_collector;
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    call_node_t *current_call = NULL;
    if (global_collector->call_stack_top) {
        current_call = global_collector->call_stack_top;
    }
    
    if (current_call && current_call->http_requests) {
        // Get the last added request (most recent)
        zend_ulong num_requests = zend_hash_num_elements(Z_ARRVAL_P(current_call->http_requests));
        if (num_requests > 0) {
            zval *last_request = zend_hash_index_find(Z_ARRVAL_P(current_call->http_requests), num_requests - 1);
            if (last_request && Z_TYPE_P(last_request) == IS_ARRAY) {
                // Use response_size as fallback for bytes_received if bytes_received is 0
                if (bytes_received == 0 && response_size > 0) {
                    add_assoc_long(last_request, "bytes_received", (long)response_size);
                }
                
                // Use request_size as fallback for bytes_sent if bytes_sent is 0
                if (bytes_sent == 0 && request_size > 0) {
                    add_assoc_long(last_request, "bytes_sent", (long)request_size);
                }
                
                // Add enhanced fields
                if (uri_path) {
                    add_assoc_string(last_request, "uri", (char *)uri_path);
                }
                if (query_string) {
                    add_assoc_string(last_request, "query_string", (char *)query_string);
                }
                if (request_headers && strlen(request_headers) > 0) {
                    // Parse and add request headers as JSON object
                    // For now, just store as string - agent will parse
                    add_assoc_string(last_request, "request_headers_raw", (char *)request_headers);
                }
                if (response_headers && strlen(response_headers) > 0) {
                    add_assoc_string(last_request, "response_headers_raw", (char *)response_headers);
                }
                if (response_size > 0) {
                    add_assoc_long(last_request, "response_size", (long)response_size);
                }
                if (request_size > 0) {
                    add_assoc_long(last_request, "request_size", (long)request_size);
                }
                if (dns_time > 0.0) {
                    add_assoc_double(last_request, "dns_time", dns_time);
                    add_assoc_double(last_request, "dns_time_ms", dns_time * 1000.0);
                }
                if (connect_time > 0.0) {
                    add_assoc_double(last_request, "connect_time", connect_time);
                    add_assoc_double(last_request, "connect_time_ms", connect_time * 1000.0);
                }
                if (total_time > 0.0) {
                    add_assoc_double(last_request, "network_time", total_time);
                    add_assoc_double(last_request, "network_time_ms", total_time * 1000.0);
                }
            }
        }
    }
}

// Record cache operation (APCu, Symfony Cache) in current call context
void record_cache_operation(const char *key, const char *operation, int hit, double duration, size_t data_size, const char *cache_type) {
    if (!OPA_G(enabled) || !profiling_active) return;
    
    extern opa_collector_t *global_collector;
    
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Get the currently executing function call from the collector
    call_node_t *current_call = NULL;
    if (global_collector->call_stack_top) {
        current_call = global_collector->call_stack_top;
    }
    
    // Lazily allocate cache_operations array when first cache operation is recorded
    if (current_call) {
        if (!current_call->cache_operations) {
            current_call->cache_operations = ecalloc(1, sizeof(zval));
            if (current_call->cache_operations) {
                array_init(current_call->cache_operations);
            }
        }
    }
    
    if (current_call && current_call->cache_operations) {
        zval operation_data;
        array_init(&operation_data);
        
        if (key) {
            add_assoc_string(&operation_data, "key", (char *)key);
        }
        if (operation) {
            add_assoc_string(&operation_data, "operation", (char *)operation);
        }
        add_assoc_bool(&operation_data, "hit", hit ? 1 : 0);
        add_assoc_double(&operation_data, "duration", duration);
        add_assoc_double(&operation_data, "duration_ms", duration * 1000.0);
        add_assoc_double(&operation_data, "timestamp", get_time_seconds() - duration);
        if (data_size > 0) {
            add_assoc_long(&operation_data, "data_size", (long)data_size);
        }
        if (cache_type) {
            add_assoc_string(&operation_data, "cache_type", (char *)cache_type);
        } else {
            add_assoc_string(&operation_data, "cache_type", "apcu");
        }
        
        add_next_index_zval(current_call->cache_operations, &operation_data);
    }
}

// Record Redis operation in current call context
void record_redis_operation(const char *command, const char *key, int hit, double duration, const char *error) {
    if (!OPA_G(enabled) || !profiling_active) return;
    
    extern opa_collector_t *global_collector;
    
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Get the currently executing function call from the collector
    call_node_t *current_call = NULL;
    if (global_collector->call_stack_top) {
        current_call = global_collector->call_stack_top;
    }
    
    // Lazily allocate redis_operations array when first Redis operation is recorded
    if (current_call) {
        if (!current_call->redis_operations) {
            current_call->redis_operations = ecalloc(1, sizeof(zval));
            if (current_call->redis_operations) {
                array_init(current_call->redis_operations);
            }
        }
    }
    
    if (current_call && current_call->redis_operations) {
        zval operation_data;
        array_init(&operation_data);
        
        if (command) {
            add_assoc_string(&operation_data, "command", (char *)command);
        }
        if (key) {
            add_assoc_string(&operation_data, "key", (char *)key);
        }
        add_assoc_bool(&operation_data, "hit", hit ? 1 : 0);
        add_assoc_double(&operation_data, "duration", duration);
        add_assoc_double(&operation_data, "duration_ms", duration * 1000.0);
        add_assoc_double(&operation_data, "timestamp", get_time_seconds() - duration);
        if (error) {
            add_assoc_string(&operation_data, "error", (char *)error);
        }
        add_assoc_string(&operation_data, "type", "redis");
        
        add_next_index_zval(current_call->redis_operations, &operation_data);
    }
}

