#include "span.h"
#include "serialize.h"
#include "opa.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// Simple dynamic buffer using malloc/realloc (not emalloc)
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} json_buffer_t;

static void json_buffer_init(json_buffer_t *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

static void json_buffer_append(json_buffer_t *buf, const char *str, size_t len) {
    if (!str || len == 0) return;
    
    size_t new_len = buf->len + len;
    if (new_len >= buf->capacity) {
        size_t new_capacity = buf->capacity ? buf->capacity * 2 : 256;
        if (new_capacity < new_len + 1) {
            new_capacity = new_len + 1;
        }
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) return; // Out of memory
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    
    memcpy(buf->data + buf->len, str, len);
    buf->len = new_len;
    buf->data[buf->len] = '\0';
}

static void json_buffer_append_str(json_buffer_t *buf, const char *str) {
    if (str) {
        json_buffer_append(buf, str, strlen(str));
    }
}

static void json_buffer_append_char(json_buffer_t *buf, char c) {
    json_buffer_append(buf, &c, 1);
}

static void json_buffer_free(json_buffer_t *buf) {
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->len = 0;
    buf->capacity = 0;
}

// JSON escape string - writes to malloc'd buffer
static void json_escape_string_malloc(json_buffer_t *buf, const char *str, size_t len) {
    const char *p = str;
    const char *end = str + len;
    while (p < end) {
        unsigned char c = *p++;
        switch (c) {
            case '"': json_buffer_append_str(buf, "\\\""); break;
            case '\\': json_buffer_append_str(buf, "\\\\"); break;
            case '\b': json_buffer_append_str(buf, "\\b"); break;
            case '\f': json_buffer_append_str(buf, "\\f"); break;
            case '\n': json_buffer_append_str(buf, "\\n"); break;
            case '\r': json_buffer_append_str(buf, "\\r"); break;
            case '\t': json_buffer_append_str(buf, "\\t"); break;
            default:
                if (c < 0x20) {
                    char hex[7];
                    snprintf(hex, sizeof(hex), "\\u%04x", c);
                    json_buffer_append_str(buf, hex);
                } else {
                    json_buffer_append_char(buf, c);
                }
                break;
        }
    }
}

// Forward declarations
static void serialize_call_node_json_malloc(json_buffer_t *buf, call_node_t *call);
static void serialize_call_stack_from_root_malloc(json_buffer_t *buf);

// Aggregate network bytes from all call nodes in the collector
static void aggregate_network_bytes_from_calls(size_t *total_sent, size_t *total_received) {
    *total_sent = 0;
    *total_received = 0;
    
    if (!global_collector || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    call_node_t *call = global_collector->calls;
    while (call) {
        if (call->magic == OPA_CALL_NODE_MAGIC && call->start_time > 0.0) {
            long net_sent = (long)call->end_bytes_sent - (long)call->start_bytes_sent;
            long net_received = (long)call->end_bytes_received - (long)call->start_bytes_received;
            if (net_sent > 0) {
                *total_sent += (size_t)net_sent;
            }
            if (net_received > 0) {
                *total_received += (size_t)net_received;
            }
        }
        call = call->next;
    }
}

// Aggregate SQL queries from all call nodes in the collector AND global SQL queries array
// Returns the number of SQL queries found
static int aggregate_sql_queries_from_calls(json_buffer_t *buf) {
    int query_count = 0;
    
    if (!global_collector || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        json_buffer_append_str(buf, "[]");
        return 0;
    }
    
    json_buffer_append_str(buf, "[");
    int first = 1;
    
    // First, add SQL queries from call nodes
    call_node_t *call = global_collector->calls;
    while (call) {
        if (call->magic == OPA_CALL_NODE_MAGIC && 
            call->sql_queries && 
            Z_TYPE_P(call->sql_queries) == IS_ARRAY &&
            zend_hash_num_elements(Z_ARRVAL_P(call->sql_queries)) > 0) {
            
            HashTable *ht = Z_ARRVAL_P(call->sql_queries);
            zval *val;
            ZEND_HASH_FOREACH_VAL(ht, val) {
                if (!first) {
                    json_buffer_append_str(buf, ",");
                }
                // Serialize SQL query zval to JSON
                // We need to use a temporary smart_string for this, then copy to json_buffer
                smart_string temp_buf = {0};
                serialize_zval_json(&temp_buf, val);
                smart_string_0(&temp_buf);
                if (temp_buf.c && temp_buf.len > 0) {
                    json_buffer_append(buf, temp_buf.c, temp_buf.len);
                    query_count++;
                }
                smart_string_free(&temp_buf);
                first = 0;
            } ZEND_HASH_FOREACH_END();
        }
        call = call->next;
    }
    
    // Then, add SQL queries from global array (captured outside of call stack)
    pthread_mutex_lock(&global_collector->global_sql_mutex);
    if (global_collector->global_sql_queries && 
        Z_TYPE_P(global_collector->global_sql_queries) == IS_ARRAY &&
        zend_hash_num_elements(Z_ARRVAL_P(global_collector->global_sql_queries)) > 0) {
        
        HashTable *ht = Z_ARRVAL_P(global_collector->global_sql_queries);
        zval *val;
        ZEND_HASH_FOREACH_VAL(ht, val) {
            if (!first) {
                json_buffer_append_str(buf, ",");
            }
            // Serialize SQL query zval to JSON
            smart_string temp_buf = {0};
            serialize_zval_json(&temp_buf, val);
            smart_string_0(&temp_buf);
            if (temp_buf.c && temp_buf.len > 0) {
                json_buffer_append(buf, temp_buf.c, temp_buf.len);
                query_count++;
            }
            smart_string_free(&temp_buf);
            first = 0;
        } ZEND_HASH_FOREACH_END();
        debug_log("[aggregate_sql_queries_from_calls] Added %d SQL queries from global array", 
            zend_hash_num_elements(Z_ARRVAL_P(global_collector->global_sql_queries)));
    }
    pthread_mutex_unlock(&global_collector->global_sql_mutex);
    
    json_buffer_append_str(buf, "]");
    debug_log("[aggregate_sql_queries_from_calls] Total SQL queries aggregated: %d", query_count);
    return query_count;
}

// External reference to global collector ()
extern opa_collector_t *global_collector;

// Serialize call stack from root calls using malloc'd buffer (safe after fastcgi_finish_request)
// This is a malloc-based version of serialize_call_stack_from_root
static void serialize_call_stack_from_root_malloc(json_buffer_t *buf) {
    json_buffer_append_str(buf, "[");
    
    // Access global collector for call tracking
    extern opa_collector_t *global_collector;
    
    debug_log("[SERIALIZE] Starting serialization, global_collector=%p", global_collector);
    
    if (!global_collector) {
        debug_log("[SERIALIZE] ERROR: global_collector is NULL");
        json_buffer_append_str(buf, "]");
        return;
    }
    
    if (global_collector->magic != OPA_COLLECTOR_MAGIC) {
        debug_log("[SERIALIZE] ERROR: global_collector has invalid magic: 0x%08X (expected 0x%08X)", 
            global_collector->magic, OPA_COLLECTOR_MAGIC);
        json_buffer_append_str(buf, "]");
        return;
    }
    
    debug_log("[SERIALIZE] Collector is valid: active=%d, calls=%p, call_stack_depth=%d", 
        global_collector->active, global_collector->calls, global_collector->call_stack_depth);
    
    call_node_t *call = global_collector->calls;
    int first = 1;
    int found_any = 0;
    int total_calls = 0;
    int valid_calls = 0;
    
    // Count total calls for debugging
    call_node_t *debug_call = global_collector->calls;
    while (debug_call) {
        total_calls++;
        if (debug_call->magic == OPA_CALL_NODE_MAGIC && debug_call->start_time > 0.0) {
            valid_calls++;
        }
        debug_call = debug_call->next;
    }
    debug_log("[SERIALIZE] Total calls in collector: %d (valid: %d), call_stack_depth: %d", 
        total_calls, valid_calls, global_collector->call_stack_depth);
    
    // Serialize ALL calls as a flat list to ensure nothing is lost
    // The recursive structure will be rebuilt by the agent using parent_id relationships
    debug_log("[SERIALIZE] Serializing all %d valid calls as flat list (will be rebuilt by agent)", valid_calls);
    call = global_collector->calls;
    int serialized_count = 0;
    while (call) {
        if (call->magic == OPA_CALL_NODE_MAGIC && call->start_time > 0.0) {
            if (!first) {
                json_buffer_append_str(buf, ",");
            }
            serialize_call_node_json_malloc(buf, call);
            first = 0;
            serialized_count++;
        }
        call = call->next;
    }
    debug_log("[SERIALIZE] Serialized %d calls as flat list", serialized_count);
    
    json_buffer_append_str(buf, "]");
}

// Serialize call node to JSON using malloc'd buffer
static void serialize_call_node_json_malloc(json_buffer_t *buf, call_node_t *call) {
    if (!call || call->magic != OPA_CALL_NODE_MAGIC) return;
    
    // Debug: log parent_id at serialization time
    debug_log("[SERIALIZE] serialize_call_node_json_malloc: call_id=%s, parent_id=%s", 
        call->call_id ? call->call_id : "NULL",
        call->parent_id ? call->parent_id : "NULL");
    
    json_buffer_append_str(buf, "{");
    
    if (call->call_id) {
        json_buffer_append_str(buf, "\"call_id\":\"");
        json_buffer_append_str(buf, call->call_id);
        json_buffer_append_str(buf, "\"");
    }
    
    // Always include function field, even if empty (for debugging)
    json_buffer_append_str(buf, ",\"function\":\"");
    if (call->function_name && strlen(call->function_name) > 0) {
        json_escape_string_malloc(buf, call->function_name, strlen(call->function_name));
    } else {
        // Fallback: use function_type to generate a name
        if (call->function_type == 2 && call->class_name) {
            // Method call - use class name
            json_escape_string_malloc(buf, call->class_name, strlen(call->class_name));
            json_buffer_append_str(buf, "::");
        }
        json_buffer_append_str(buf, "<unknown>");
    }
    json_buffer_append_str(buf, "\"");
    
    if (call->class_name) {
        json_buffer_append_str(buf, ",\"class\":\"");
        json_escape_string_malloc(buf, call->class_name, strlen(call->class_name));
        json_buffer_append_str(buf, "\"");
    }
    
    if (call->file) {
        json_buffer_append_str(buf, ",\"file\":\"");
        json_escape_string_malloc(buf, call->file, strlen(call->file));
        json_buffer_append_str(buf, "\"");
    }
    
    if (call->line > 0) {
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "%d", call->line);
        json_buffer_append_str(buf, ",\"line\":");
        json_buffer_append_str(buf, line_str);
    }
    
    // Duration in milliseconds
    // TEMPORARY TEST: If end_time is 0, use a default duration of 1ms for testing
    double end_time = call->end_time > 0.0 ? call->end_time : call->start_time + 0.001; // Default 1ms if not set
    double duration_ms = (end_time - call->start_time) * 1000.0;
    if (duration_ms < 0.0) duration_ms = 0.0; // Safety check
    char duration_str[64];
    snprintf(duration_str, sizeof(duration_str), "%.3f", duration_ms);
    json_buffer_append_str(buf, ",\"duration_ms\":");
    json_buffer_append_str(buf, duration_str);
    
    // CPU time in milliseconds
    // TEMPORARY TEST: If end_cpu_time is 0, use a default value for testing
    double end_cpu_time = call->end_cpu_time > 0.0 ? call->end_cpu_time : call->start_cpu_time + 0.0005; // Default 0.5ms if not set
    double cpu_ms = (end_cpu_time - call->start_cpu_time) * 1000.0;
    if (cpu_ms < 0.0) cpu_ms = 0.0; // Safety check
    char cpu_str[64];
    snprintf(cpu_str, sizeof(cpu_str), "%.3f", cpu_ms);
    json_buffer_append_str(buf, ",\"cpu_ms\":");
    json_buffer_append_str(buf, cpu_str);
    
    // Memory delta
    long memory_delta = (long)call->end_memory - (long)call->start_memory;
    char mem_str[64];
    snprintf(mem_str, sizeof(mem_str), "%ld", memory_delta);
    json_buffer_append_str(buf, ",\"memory_delta\":");
    json_buffer_append_str(buf, mem_str);
    
    // Network bytes
    long net_sent = (long)call->end_bytes_sent - (long)call->start_bytes_sent;
    long net_received = (long)call->end_bytes_received - (long)call->start_bytes_received;
    char net_sent_str[64], net_recv_str[64];
    snprintf(net_sent_str, sizeof(net_sent_str), "%ld", net_sent);
    snprintf(net_recv_str, sizeof(net_recv_str), "%ld", net_received);
    json_buffer_append_str(buf, ",\"network_bytes_sent\":");
    json_buffer_append_str(buf, net_sent_str);
    json_buffer_append_str(buf, ",\"network_bytes_received\":");
    json_buffer_append_str(buf, net_recv_str);
    
    // Always include parent_id field, even if NULL (for debugging)
    json_buffer_append_str(buf, ",\"parent_id\":");
    if (call->parent_id && strlen(call->parent_id) > 0) {
        json_buffer_append_str(buf, "\"");
        json_buffer_append_str(buf, call->parent_id);
        json_buffer_append_str(buf, "\"");
        debug_log("[SERIALIZE] Added parent_id=%s to JSON for call_id=%s", call->parent_id, call->call_id ? call->call_id : "NULL");
    } else {
        json_buffer_append_str(buf, "null");
        debug_log("[SERIALIZE] parent_id is NULL/empty for call_id=%s (parent_id=%p)", 
            call->call_id ? call->call_id : "NULL", call->parent_id);
    }
    
    char depth_str[32];
    snprintf(depth_str, sizeof(depth_str), "%d", call->depth);
    json_buffer_append_str(buf, ",\"depth\":");
    json_buffer_append_str(buf, depth_str);
    
    char type_str[32];
    snprintf(type_str, sizeof(type_str), "%d", call->function_type);
    json_buffer_append_str(buf, ",\"function_type\":");
    json_buffer_append_str(buf, type_str);
    
    // Serialize SQL queries
    if (call->sql_queries && Z_TYPE_P(call->sql_queries) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->sql_queries)) > 0) {
        json_buffer_append_str(buf, ",\"sql_queries\":");
        // Use temporary smart_string to serialize, then copy to json_buffer
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->sql_queries);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(buf, temp_buf.c, temp_buf.len);
        }
        smart_string_free(&temp_buf);
    }
    
    // Serialize HTTP requests
    if (call->http_requests && Z_TYPE_P(call->http_requests) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->http_requests)) > 0) {
        json_buffer_append_str(buf, ",\"http_requests\":");
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->http_requests);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(buf, temp_buf.c, temp_buf.len);
        }
        smart_string_free(&temp_buf);
    }
    
    // Serialize cache operations (APCu, Symfony Cache)
    if (call->cache_operations && Z_TYPE_P(call->cache_operations) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->cache_operations)) > 0) {
        json_buffer_append_str(buf, ",\"cache_operations\":");
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->cache_operations);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(buf, temp_buf.c, temp_buf.len);
        }
        smart_string_free(&temp_buf);
    }
    
    // Serialize Redis operations
    if (call->redis_operations && Z_TYPE_P(call->redis_operations) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->redis_operations)) > 0) {
        json_buffer_append_str(buf, ",\"redis_operations\":");
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->redis_operations);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(buf, temp_buf.c, temp_buf.len);
        }
        smart_string_free(&temp_buf);
    }
    
    // Don't serialize children here - all calls are serialized as flat list
    // The agent will rebuild the tree structure using parent_id relationships
    json_buffer_append_str(buf, ",\"children\":[]");
    
    json_buffer_append_str(buf, "}");
}

// Create span context
span_context_t* create_span_context(const char *span_id, const char *trace_id, const char *name) {
    span_context_t *span = emalloc(sizeof(span_context_t));
    memset(span, 0, sizeof(span_context_t));
    span->span_id = span_id ? estrdup(span_id) : NULL;
    span->trace_id = trace_id ? estrdup(trace_id) : NULL;
    span->name = name ? estrdup(name) : NULL;
    span->status = -1;
    return span;
}

// Free span context
// WARNING: During MSHUTDOWN, zvals may be invalid - do not access them
// Set a flag to indicate we're in shutdown to avoid zval access
static int in_shutdown = 0;

void set_span_shutdown_flag(int flag) {
    in_shutdown = flag;
}

void free_span_context(span_context_t *span) {
    if (!span) return;
    
    if (span->span_id) efree(span->span_id);
    if (span->trace_id) efree(span->trace_id);
    if (span->parent_id) efree(span->parent_id);
    if (span->name) efree(span->name);
    
    // Free zvals if allocated - but ONLY if not in shutdown
    // During MSHUTDOWN, the Zend heap is being destroyed and accessing zvals causes corruption
    if (!in_shutdown) {
        // Check type before destroying to avoid corruption
        if (span->tags) {
            if (Z_TYPE_P(span->tags) != IS_UNDEF) {
                zval_ptr_dtor(span->tags);
            }
            efree(span->tags);
            span->tags = NULL;
        }
        if (span->net) {
            if (Z_TYPE_P(span->net) != IS_UNDEF) {
                zval_ptr_dtor(span->net);
            }
            efree(span->net);
            span->net = NULL;
        }
        if (span->sql) {
            if (Z_TYPE_P(span->sql) != IS_UNDEF) {
                zval_ptr_dtor(span->sql);
            }
            efree(span->sql);
            span->sql = NULL;
        }
        if (span->stack) {
            if (Z_TYPE_P(span->stack) != IS_UNDEF) {
                zval_ptr_dtor(span->stack);
            }
            efree(span->stack);
            span->stack = NULL;
        }
        if (span->dumps) {
            if (Z_TYPE_P(span->dumps) != IS_UNDEF) {
                zval_ptr_dtor(span->dumps);
            }
            efree(span->dumps);
            span->dumps = NULL;
        }
    } else {
        // During shutdown: just free the zval pointers without accessing them
        // PHP will clean up the zval contents automatically
        if (span->tags) efree(span->tags);
        if (span->net) efree(span->net);
        if (span->sql) efree(span->sql);
        if (span->stack) efree(span->stack);
        if (span->dumps) efree(span->dumps);
    }
    
    efree(span);
}

// Produce span JSON from individual values - returns char* allocated with malloc (not emalloc)
// This is safe to use after fastcgi_finish_request()
// Uses only malloc/realloc - NO emalloc or smart_string to avoid segfaults
// All string parameters should already be in malloc'd memory (copied before fastcgi_finish_request)
char* produce_span_json_from_values(
    const char *trace_id, const char *span_id, const char *parent_id, const char *name,
    const char *url_scheme, const char *url_host, const char *url_path,
    long start_ts, long end_ts, int cpu_ms, int status, const char *dumps_json,
    const char *cli_args_json, const char *http_request_json, const char *http_response_json
) {
    debug_log("[produce_span_json_from_values] Called: trace_id=%s, span_id=%s", 
        trace_id ? trace_id : "NULL", span_id ? span_id : "NULL");
    
    // All string parameters should already be in malloc'd memory
    // No need to copy them again - they're safe to use
    
    // Use malloc'd buffer (NOT smart_string/emalloc) - safe after fastcgi_finish_request()
    json_buffer_t buf;
    json_buffer_init(&buf);
    
    json_buffer_append_str(&buf, "{\"type\":\"span\",\"trace_id\":\"");
    if (trace_id) {
        json_buffer_append_str(&buf, trace_id);
    } else {
        json_buffer_append_str(&buf, "unknown");
    }
    json_buffer_append_str(&buf, "\",\"span_id\":\"");
    if (span_id) {
        json_buffer_append_str(&buf, span_id);
    } else {
        json_buffer_append_str(&buf, "unknown");
    }
    json_buffer_append_str(&buf, "\"");
    
    if (parent_id) {
        json_buffer_append_str(&buf, ",\"parent_id\":\"");
        json_buffer_append_str(&buf, parent_id);
        json_buffer_append_str(&buf, "\"");
    }
    
    // Use configurable service name
    json_buffer_append_str(&buf, ",\"service\":\"");
    const char *service_name = OPA_G(service) ? OPA_G(service) : "php-fpm";
    json_escape_string_malloc(&buf, service_name, strlen(service_name));
    json_buffer_append_str(&buf, "\",\"name\":\"");
    if (name) {
        json_escape_string_malloc(&buf, name, strlen(name));
    } else {
        json_buffer_append_str(&buf, "unknown");
    }
    json_buffer_append_str(&buf, "\"");
    
    json_buffer_append_str(&buf, ",\"start_ts\":");
    char start_ts_str[32];
    snprintf(start_ts_str, sizeof(start_ts_str), "%ld", start_ts);
    json_buffer_append_str(&buf, start_ts_str);
    
    json_buffer_append_str(&buf, ",\"end_ts\":");
    char end_ts_str[32];
    snprintf(end_ts_str, sizeof(end_ts_str), "%ld", end_ts);
    json_buffer_append_str(&buf, end_ts_str);
    
    json_buffer_append_str(&buf, ",\"duration_ms\":");
    char duration_str[32];
    snprintf(duration_str, sizeof(duration_str), "%ld", end_ts - start_ts);
    json_buffer_append_str(&buf, duration_str);
    
    if (cpu_ms > 0) {
        json_buffer_append_str(&buf, ",\"cpu_ms\":");
        char cpu_str[32];
        snprintf(cpu_str, sizeof(cpu_str), "%d", cpu_ms);
        json_buffer_append_str(&buf, cpu_str);
    }
    
    if (status >= 0) {
        json_buffer_append_str(&buf, ",\"status\":\"");
        if (status == 1) {
            json_buffer_append_str(&buf, "ok");
        } else {
            json_buffer_append_str(&buf, "error");
        }
        json_buffer_append_str(&buf, "\"");
    }
    
    // Add language metadata as top-level fields
    if (OPA_G(language) && strlen(OPA_G(language)) > 0) {
        json_buffer_append_str(&buf, ",\"language\":\"");
        json_escape_string_malloc(&buf, OPA_G(language), strlen(OPA_G(language)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(language_version) && strlen(OPA_G(language_version)) > 0) {
        json_buffer_append_str(&buf, ",\"language_version\":\"");
        json_escape_string_malloc(&buf, OPA_G(language_version), strlen(OPA_G(language_version)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(framework) && strlen(OPA_G(framework)) > 0) {
        json_buffer_append_str(&buf, ",\"framework\":\"");
        json_escape_string_malloc(&buf, OPA_G(framework), strlen(OPA_G(framework)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(framework_version) && strlen(OPA_G(framework_version)) > 0) {
        json_buffer_append_str(&buf, ",\"framework_version\":\"");
        json_escape_string_malloc(&buf, OPA_G(framework_version), strlen(OPA_G(framework_version)));
        json_buffer_append_str(&buf, "\"");
    }
    
    // Add URL components if present
    if (url_scheme || url_host || url_path) {
        if (url_scheme) {
            json_buffer_append_str(&buf, ",\"url_scheme\":\"");
            json_escape_string_malloc(&buf, url_scheme, strlen(url_scheme));
            json_buffer_append_str(&buf, "\"");
        }
        if (url_host) {
            json_buffer_append_str(&buf, ",\"url_host\":\"");
            json_escape_string_malloc(&buf, url_host, strlen(url_host));
            json_buffer_append_str(&buf, "\"");
        }
        if (url_path) {
            json_buffer_append_str(&buf, ",\"url_path\":\"");
            json_escape_string_malloc(&buf, url_path, strlen(url_path));
            json_buffer_append_str(&buf, "\"");
        }
    }
    
    // Add tags - include organization_id, project_id, CLI args and HTTP request/response if present
    json_buffer_append_str(&buf, ",\"tags\":{");
    int tag_first = 1;
    
    // Add organization_id and project_id to tags (agent reads from tags)
    if (OPA_G(organization_id) && strlen(OPA_G(organization_id)) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"organization_id\":\"");
        json_escape_string_malloc(&buf, OPA_G(organization_id), strlen(OPA_G(organization_id)));
        json_buffer_append_str(&buf, "\"");
        tag_first = 0;
    }
    if (OPA_G(project_id) && strlen(OPA_G(project_id)) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"project_id\":\"");
        json_escape_string_malloc(&buf, OPA_G(project_id), strlen(OPA_G(project_id)));
        json_buffer_append_str(&buf, "\"");
        tag_first = 0;
    }
    
    if (cli_args_json && strlen(cli_args_json) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"cli\":");
        json_buffer_append_str(&buf, cli_args_json);
        tag_first = 0;
    }
    // Always include http_request in tags (even if empty) to ensure it's always present
    if (!tag_first) json_buffer_append_str(&buf, ",");
    json_buffer_append_str(&buf, "\"http_request\":");
    if (http_request_json && strlen(http_request_json) > 0) {
        json_buffer_append_str(&buf, http_request_json);
        debug_log("[produce_span_json_from_values] Added http_request to tags, len=%zu, content=%.200s", strlen(http_request_json), http_request_json);
    } else {
        json_buffer_append_str(&buf, "{}");
        debug_log("[produce_span_json_from_values] Added empty http_request object (http_request_json=%p)", http_request_json);
    }
    tag_first = 0;
    if (http_response_json && strlen(http_response_json) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"http_response\":");
        json_buffer_append_str(&buf, http_response_json);
        debug_log("[produce_span_json_from_values] Added http_response to tags, len=%zu", strlen(http_response_json));
        tag_first = 0;
    } else {
        debug_log("[produce_span_json_from_values] http_response_json is NULL or empty (this is OK for CLI requests)");
    }
    // Add expand_spans flag to tags (default: true for multiple spans mode)
    if (!tag_first) json_buffer_append_str(&buf, ",");
    json_buffer_append_str(&buf, "\"expand_spans\":");
    if (OPA_G(expand_spans)) {
        json_buffer_append_str(&buf, "true");
    } else {
        json_buffer_append_str(&buf, "false");
    }
    tag_first = 0;
    json_buffer_append_str(&buf, "}");
    
    // Aggregate network metrics from call stack
    size_t total_bytes_sent = 0;
    size_t total_bytes_received = 0;
    aggregate_network_bytes_from_calls(&total_bytes_sent, &total_bytes_received);
    
    json_buffer_append_str(&buf, ",\"net\":{");
    char net_sent_str[64], net_recv_str[64];
    snprintf(net_sent_str, sizeof(net_sent_str), "%zu", total_bytes_sent);
    snprintf(net_recv_str, sizeof(net_recv_str), "%zu", total_bytes_received);
    json_buffer_append_str(&buf, "\"bytes_sent\":");
    json_buffer_append_str(&buf, net_sent_str);
    json_buffer_append_str(&buf, ",\"bytes_received\":");
    json_buffer_append_str(&buf, net_recv_str);
    json_buffer_append_str(&buf, "}");
    
    // Aggregate SQL queries from call stack
    json_buffer_append_str(&buf, ",\"sql\":");
    int sql_count = aggregate_sql_queries_from_calls(&buf);
    debug_log("[produce_span_json_from_values] Aggregated %d SQL queries from call stack", sql_count);
    
    // Serialize dumps if present
    json_buffer_append_str(&buf, ",\"dumps\":");
    if (dumps_json && strlen(dumps_json) > 0) {
        debug_log("[produce_span_json_from_values] Adding dumps_json, len=%zu, preview=%.100s", strlen(dumps_json), dumps_json);
        json_buffer_append_str(&buf, dumps_json);
    } else {
        debug_log("[produce_span_json_from_values] No dumps_json (dumps_json=%p, len=%zu) - WILL SEND EMPTY ARRAY", dumps_json, dumps_json ? strlen(dumps_json) : 0);
        json_buffer_append_str(&buf, "[]");
    }
    
    // Always serialize call stack - needed for ExecutionStackTree view even when expand_spans is enabled
    // When expand_spans is true, we also send child spans as separate messages for other views,
    // but the full call stack is still needed for the tree visualization
    debug_log("[produce_span_json_from_values] Serializing call stack (expand_spans=%d)", OPA_G(expand_spans));
    json_buffer_append_str(&buf, ",\"stack\":");
    serialize_call_stack_from_root_malloc(&buf);
    debug_log("[produce_span_json_from_values] Call stack serialization completed");
    
    json_buffer_append_str(&buf, "}\n");
    
    
    // Return the buffer data (already malloc'd) - caller will free it
    return buf.data;
}

// Produce child span JSON from call node - send each significant call as separate span
// Returns NULL if call node is not significant (no SQL/HTTP/cache/Redis and duration <= 10ms)
// Safe to use after fastcgi_finish_request()
char* produce_child_span_json_from_call_node(
    call_node_t *call, const char *trace_id, const char *parent_span_id, long root_start_ts
) {
    if (!call || call->magic != OPA_CALL_NODE_MAGIC) {
        return NULL;
    }
    
    // Check if call node is significant (should create a span)
    int has_sql = (call->sql_queries && Z_TYPE_P(call->sql_queries) == IS_ARRAY && 
                   zend_hash_num_elements(Z_ARRVAL_P(call->sql_queries)) > 0);
    int has_http = (call->http_requests && Z_TYPE_P(call->http_requests) == IS_ARRAY && 
                    zend_hash_num_elements(Z_ARRVAL_P(call->http_requests)) > 0);
    int has_cache = (call->cache_operations && Z_TYPE_P(call->cache_operations) == IS_ARRAY && 
                     zend_hash_num_elements(Z_ARRVAL_P(call->cache_operations)) > 0);
    int has_redis = (call->redis_operations && Z_TYPE_P(call->redis_operations) == IS_ARRAY && 
                     zend_hash_num_elements(Z_ARRVAL_P(call->redis_operations)) > 0);
    
    double end_time = call->end_time > 0.0 ? call->end_time : call->start_time + 0.001;
    double duration_ms = (end_time - call->start_time) * 1000.0;
    if (duration_ms < 0.0) duration_ms = 0.0;
    
    // Only create span for significant nodes
    if (!has_sql && !has_http && !has_cache && !has_redis && duration_ms <= 10.0) {
        return NULL; // Not significant, skip
    }
    
    // Build span name from class::function or just function
    char *span_name = NULL;
    if (call->class_name && call->function_name) {
        size_t name_len = strlen(call->class_name) + 2 + strlen(call->function_name) + 1;
        span_name = malloc(name_len);
        if (span_name) {
            snprintf(span_name, name_len, "%s::%s", call->class_name, call->function_name);
        }
    } else if (call->function_name) {
        size_t name_len = strlen(call->function_name) + 1;
        span_name = malloc(name_len);
        if (span_name) {
            strcpy(span_name, call->function_name);
        }
    } else {
        span_name = strdup("function_call");
    }
    
    if (!span_name) {
        return NULL; // Memory allocation failed
    }
    
    // Calculate timestamps relative to root span
    // Approximate: use depth and duration to estimate timing
    long start_ts = root_start_ts + (long)(call->start_time * 1000.0);
    long end_ts = root_start_ts + (long)(end_time * 1000.0);
    if (start_ts < root_start_ts) start_ts = root_start_ts; // Safety check
    
    // Calculate CPU time
    double end_cpu_time = call->end_cpu_time > 0.0 ? call->end_cpu_time : call->start_cpu_time + 0.0005;
    double cpu_ms = (end_cpu_time - call->start_cpu_time) * 1000.0;
    if (cpu_ms < 0.0) cpu_ms = 0.0;
    int cpu_ms_int = (int)cpu_ms;
    
    // Use malloc'd buffer (safe after fastcgi_finish_request)
    json_buffer_t buf;
    json_buffer_init(&buf);
    
    json_buffer_append_str(&buf, "{\"type\":\"span\",\"trace_id\":\"");
    if (trace_id) {
        json_buffer_append_str(&buf, trace_id);
    } else {
        json_buffer_append_str(&buf, "unknown");
    }
    json_buffer_append_str(&buf, "\",\"span_id\":\"");
    if (call->call_id) {
        json_buffer_append_str(&buf, call->call_id);
    } else {
        json_buffer_append_str(&buf, "unknown");
    }
    json_buffer_append_str(&buf, "\"");
    
    if (parent_span_id) {
        json_buffer_append_str(&buf, ",\"parent_id\":\"");
        json_buffer_append_str(&buf, parent_span_id);
        json_buffer_append_str(&buf, "\"");
    }
    
    // Service name
    json_buffer_append_str(&buf, ",\"service\":\"");
    const char *service_name = OPA_G(service) ? OPA_G(service) : "php-fpm";
    json_escape_string_malloc(&buf, service_name, strlen(service_name));
    json_buffer_append_str(&buf, "\",\"name\":\"");
    json_escape_string_malloc(&buf, span_name, strlen(span_name));
    json_buffer_append_str(&buf, "\"");
    
    // Timestamps
    char start_ts_str[32], end_ts_str[32], duration_str[32];
    snprintf(start_ts_str, sizeof(start_ts_str), "%ld", start_ts);
    snprintf(end_ts_str, sizeof(end_ts_str), "%ld", end_ts);
    snprintf(duration_str, sizeof(duration_str), "%.3f", duration_ms);
    
    json_buffer_append_str(&buf, ",\"start_ts\":");
    json_buffer_append_str(&buf, start_ts_str);
    json_buffer_append_str(&buf, ",\"end_ts\":");
    json_buffer_append_str(&buf, end_ts_str);
    json_buffer_append_str(&buf, ",\"duration_ms\":");
    json_buffer_append_str(&buf, duration_str);
    
    if (cpu_ms_int > 0) {
        char cpu_str[32];
        snprintf(cpu_str, sizeof(cpu_str), "%d", cpu_ms_int);
        json_buffer_append_str(&buf, ",\"cpu_ms\":");
        json_buffer_append_str(&buf, cpu_str);
    }
    
    // Status (default to ok for child spans, agent will calculate if needed)
    json_buffer_append_str(&buf, ",\"status\":\"ok\"");
    
    // Language metadata
    if (OPA_G(language) && strlen(OPA_G(language)) > 0) {
        json_buffer_append_str(&buf, ",\"language\":\"");
        json_escape_string_malloc(&buf, OPA_G(language), strlen(OPA_G(language)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(language_version) && strlen(OPA_G(language_version)) > 0) {
        json_buffer_append_str(&buf, ",\"language_version\":\"");
        json_escape_string_malloc(&buf, OPA_G(language_version), strlen(OPA_G(language_version)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(framework) && strlen(OPA_G(framework)) > 0) {
        json_buffer_append_str(&buf, ",\"framework\":\"");
        json_escape_string_malloc(&buf, OPA_G(framework), strlen(OPA_G(framework)));
        json_buffer_append_str(&buf, "\"");
    }
    if (OPA_G(framework_version) && strlen(OPA_G(framework_version)) > 0) {
        json_buffer_append_str(&buf, ",\"framework_version\":\"");
        json_escape_string_malloc(&buf, OPA_G(framework_version), strlen(OPA_G(framework_version)));
        json_buffer_append_str(&buf, "\"");
    }
    
    // Tags
    json_buffer_append_str(&buf, ",\"tags\":{");
    int tag_first = 1;
    
    // Organization and project
    if (OPA_G(organization_id) && strlen(OPA_G(organization_id)) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"organization_id\":\"");
        json_escape_string_malloc(&buf, OPA_G(organization_id), strlen(OPA_G(organization_id)));
        json_buffer_append_str(&buf, "\"");
        tag_first = 0;
    }
    if (OPA_G(project_id) && strlen(OPA_G(project_id)) > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"project_id\":\"");
        json_escape_string_malloc(&buf, OPA_G(project_id), strlen(OPA_G(project_id)));
        json_buffer_append_str(&buf, "\"");
        tag_first = 0;
    }
    
    // Add call metadata to tags
    if (!tag_first) json_buffer_append_str(&buf, ",");
    json_buffer_append_str(&buf, "\"call_id\":\"");
    if (call->call_id) {
        json_buffer_append_str(&buf, call->call_id);
    }
    json_buffer_append_str(&buf, "\"");
    tag_first = 0;
    
    if (call->file) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        json_buffer_append_str(&buf, "\"file\":\"");
        json_escape_string_malloc(&buf, call->file, strlen(call->file));
        json_buffer_append_str(&buf, "\"");
        tag_first = 0;
    }
    
    if (call->line > 0) {
        if (!tag_first) json_buffer_append_str(&buf, ",");
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "%d", call->line);
        json_buffer_append_str(&buf, "\"line\":");
        json_buffer_append_str(&buf, line_str);
        tag_first = 0;
    }
    
    char depth_str[32];
    snprintf(depth_str, sizeof(depth_str), "%d", call->depth);
    if (!tag_first) json_buffer_append_str(&buf, ",");
    json_buffer_append_str(&buf, "\"depth\":");
    json_buffer_append_str(&buf, depth_str);
    tag_first = 0;
    
    json_buffer_append_str(&buf, "}");
    
    // Network metrics
    long net_sent = (long)call->end_bytes_sent - (long)call->start_bytes_sent;
    long net_received = (long)call->end_bytes_received - (long)call->start_bytes_received;
    if (net_sent > 0 || net_received > 0) {
        json_buffer_append_str(&buf, ",\"net\":{");
        char net_sent_str[64], net_recv_str[64];
        snprintf(net_sent_str, sizeof(net_sent_str), "%ld", net_sent);
        snprintf(net_recv_str, sizeof(net_recv_str), "%ld", net_received);
        json_buffer_append_str(&buf, "\"bytes_sent\":");
        json_buffer_append_str(&buf, net_sent_str);
        json_buffer_append_str(&buf, ",\"bytes_received\":");
        json_buffer_append_str(&buf, net_recv_str);
        json_buffer_append_str(&buf, "}");
    } else {
        json_buffer_append_str(&buf, ",\"net\":{}");
    }
    
    // SQL queries
    json_buffer_append_str(&buf, ",\"sql\":");
    if (has_sql) {
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->sql_queries);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(&buf, temp_buf.c, temp_buf.len);
        } else {
            json_buffer_append_str(&buf, "[]");
        }
        smart_string_free(&temp_buf);
    } else {
        json_buffer_append_str(&buf, "[]");
    }
    
    // HTTP requests
    json_buffer_append_str(&buf, ",\"http\":");
    if (has_http) {
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->http_requests);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(&buf, temp_buf.c, temp_buf.len);
        } else {
            json_buffer_append_str(&buf, "[]");
        }
        smart_string_free(&temp_buf);
    } else {
        json_buffer_append_str(&buf, "[]");
    }
    
    // Cache operations
    json_buffer_append_str(&buf, ",\"cache\":");
    if (has_cache) {
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->cache_operations);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(&buf, temp_buf.c, temp_buf.len);
        } else {
            json_buffer_append_str(&buf, "[]");
        }
        smart_string_free(&temp_buf);
    } else {
        json_buffer_append_str(&buf, "[]");
    }
    
    // Redis operations
    json_buffer_append_str(&buf, ",\"redis\":");
    if (has_redis) {
        smart_string temp_buf = {0};
        serialize_zval_json(&temp_buf, call->redis_operations);
        smart_string_0(&temp_buf);
        if (temp_buf.c && temp_buf.len > 0) {
            json_buffer_append(&buf, temp_buf.c, temp_buf.len);
        } else {
            json_buffer_append_str(&buf, "[]");
        }
        smart_string_free(&temp_buf);
    } else {
        json_buffer_append_str(&buf, "[]");
    }
    
    // No stack field (already expanded into separate spans)
    // No dumps (only root span has dumps)
    
    json_buffer_append_str(&buf, "}\n");
    
    free(span_name);
    
    return buf.data;
}

// Produce span JSON - wrapper that copies values and calls produce_span_json_from_values
// This is safe to use before fastcgi_finish_request() (but not after)
// For use after fastcgi_finish_request(), use produce_span_json_from_values directly
char* produce_span_json(span_context_t *span) {
    // Copy all string fields to malloc'd memory BEFORE using them
    char *trace_id_copy = NULL;
    char *span_id_copy = NULL;
    char *parent_id_copy = NULL;
    char *name_copy = NULL;
    
    if (span->trace_id) {
        size_t len = strlen(span->trace_id);
        trace_id_copy = malloc(len + 1);
        if (trace_id_copy) {
            memcpy(trace_id_copy, span->trace_id, len);
            trace_id_copy[len] = '\0';
        }
    }
    
    if (span->span_id) {
        size_t len = strlen(span->span_id);
        span_id_copy = malloc(len + 1);
        if (span_id_copy) {
            memcpy(span_id_copy, span->span_id, len);
            span_id_copy[len] = '\0';
        }
    }
    
    if (span->parent_id) {
        size_t len = strlen(span->parent_id);
        parent_id_copy = malloc(len + 1);
        if (parent_id_copy) {
            memcpy(parent_id_copy, span->parent_id, len);
            parent_id_copy[len] = '\0';
        }
    }
    
    if (span->name) {
        size_t len = strlen(span->name);
        name_copy = malloc(len + 1);
        if (name_copy) {
            memcpy(name_copy, span->name, len);
            name_copy[len] = '\0';
        }
    }
    
    // Copy URL components
    char *url_scheme_copy = NULL;
    char *url_host_copy = NULL;
    char *url_path_copy = NULL;
    
    if (span->url_scheme) {
        size_t len = strlen(span->url_scheme);
        url_scheme_copy = malloc(len + 1);
        if (url_scheme_copy) {
            memcpy(url_scheme_copy, span->url_scheme, len);
            url_scheme_copy[len] = '\0';
        }
    }
    
    if (span->url_host) {
        size_t len = strlen(span->url_host);
        url_host_copy = malloc(len + 1);
        if (url_host_copy) {
            memcpy(url_host_copy, span->url_host, len);
            url_host_copy[len] = '\0';
        }
    }
    
    if (span->url_path) {
        size_t len = strlen(span->url_path);
        url_path_copy = malloc(len + 1);
        if (url_path_copy) {
            memcpy(url_path_copy, span->url_path, len);
            url_path_copy[len] = '\0';
        }
    }
    
    // Copy numeric values
    long start_ts = span->start_ts;
    long end_ts = span->end_ts;
    int cpu_ms = span->cpu_ms;
    int status = span->status;
    
    // Serialize dumps to JSON string (before fastcgi_finish_request, so zvals are safe)
    char *dumps_json = NULL;
    if (span->dumps && Z_TYPE_P(span->dumps) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(span->dumps)) > 0) {
        smart_string dumps_buf = {0};
        serialize_zval_json(&dumps_buf, span->dumps);
        smart_string_0(&dumps_buf);
        
        // Convert smart_string to malloc'd char* (produce_span_json_from_values expects malloc'd string)
        if (dumps_buf.c && dumps_buf.len > 0) {
            dumps_json = malloc(dumps_buf.len + 1);
            if (dumps_json) {
                memcpy(dumps_json, dumps_buf.c, dumps_buf.len);
                dumps_json[dumps_buf.len] = '\0';
            }
        }
        
        // Free smart_string (it uses emalloc, so we use smart_string_free)
        if (dumps_buf.c) {
            smart_string_free(&dumps_buf);
        }
    }
    
    // Call the safe function
    char *result = produce_span_json_from_values(
        trace_id_copy, span_id_copy, parent_id_copy, name_copy,
        url_scheme_copy, url_host_copy, url_path_copy,
        start_ts, end_ts, cpu_ms, status, dumps_json,
        NULL, NULL, NULL  // cli_args_json, http_request_json, http_response_json
    );
    
    // Free dumps JSON if allocated
    if (dumps_json) {
        free(dumps_json);
    }
    
    // Free copied strings (they're now in the JSON output)
    if (trace_id_copy) free(trace_id_copy);
    if (span_id_copy) free(span_id_copy);
    if (parent_id_copy) free(parent_id_copy);
    if (name_copy) free(name_copy);
    if (url_scheme_copy) free(url_scheme_copy);
    if (url_host_copy) free(url_host_copy);
    if (url_path_copy) free(url_path_copy);
    
    return result;
}

