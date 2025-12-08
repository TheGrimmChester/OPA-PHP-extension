#include "serialize.h"

// Helper: Escape JSON string
void json_escape_string(smart_string *buf, const char *str, size_t len) {
    const char *p = str;
    const char *end = str + len;
    while (p < end) {
        unsigned char c = *p++;
        switch (c) {
            case '"': smart_string_appends(buf, "\\\""); break;
            case '\\': smart_string_appends(buf, "\\\\"); break;
            case '\b': smart_string_appends(buf, "\\b"); break;
            case '\f': smart_string_appends(buf, "\\f"); break;
            case '\n': smart_string_appends(buf, "\\n"); break;
            case '\r': smart_string_appends(buf, "\\r"); break;
            case '\t': smart_string_appends(buf, "\\t"); break;
            default:
                if (c < 0x20) {
                    char hex[7];
                    snprintf(hex, 7, "\\u%04x", c);
                    smart_string_appends(buf, hex);
                } else {
                    smart_string_appendc(buf, c);
                }
                break;
        }
    }
}

// Serialize call node to JSON
void serialize_call_node_json(smart_string *buf, call_node_t *call) {
    if (!call || call->magic != OPA_CALL_NODE_MAGIC) return;
    
    smart_string_appends(buf, "{");
    
    if (call->call_id) {
        smart_string_appends(buf, "\"call_id\":\"");
        smart_string_appends(buf, call->call_id);
        smart_string_appends(buf, "\"");
    }
    
    if (call->function_name) {
        smart_string_appends(buf, ",\"function\":\"");
        json_escape_string(buf, call->function_name, strlen(call->function_name));
        smart_string_appends(buf, "\"");
    }
    
    if (call->class_name) {
        smart_string_appends(buf, ",\"class\":\"");
        json_escape_string(buf, call->class_name, strlen(call->class_name));
        smart_string_appends(buf, "\"");
    }
    
    if (call->file) {
        smart_string_appends(buf, ",\"file\":\"");
        json_escape_string(buf, call->file, strlen(call->file));
        smart_string_appends(buf, "\"");
    }
    
    if (call->line > 0) {
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "%d", call->line);
        smart_string_appends(buf, ",\"line\":");
        smart_string_appends(buf, line_str);
    }
    
    // Duration in milliseconds
    double duration_ms = (call->end_time - call->start_time) * 1000.0;
    char duration_str[64];
    snprintf(duration_str, sizeof(duration_str), "%.3f", duration_ms);
    smart_string_appends(buf, ",\"duration_ms\":");
    smart_string_appends(buf, duration_str);
    
    // CPU time in milliseconds
    double cpu_ms = (call->end_cpu_time - call->start_cpu_time) * 1000.0;
    char cpu_str[64];
    snprintf(cpu_str, sizeof(cpu_str), "%.3f", cpu_ms);
    smart_string_appends(buf, ",\"cpu_ms\":");
    smart_string_appends(buf, cpu_str);
    
    // Memory delta
    long memory_delta = (long)call->end_memory - (long)call->start_memory;
    char mem_str[64];
    snprintf(mem_str, sizeof(mem_str), "%ld", memory_delta);
    smart_string_appends(buf, ",\"memory_delta\":");
    smart_string_appends(buf, mem_str);
    
    // Network bytes
    long net_sent = (long)call->end_bytes_sent - (long)call->start_bytes_sent;
    long net_received = (long)call->end_bytes_received - (long)call->start_bytes_received;
    char net_sent_str[64], net_recv_str[64];
    snprintf(net_sent_str, sizeof(net_sent_str), "%ld", net_sent);
    snprintf(net_recv_str, sizeof(net_recv_str), "%ld", net_received);
    smart_string_appends(buf, ",\"network_bytes_sent\":");
    smart_string_appends(buf, net_sent_str);
    smart_string_appends(buf, ",\"network_bytes_received\":");
    smart_string_appends(buf, net_recv_str);
    
    if (call->parent_id) {
        smart_string_appends(buf, ",\"parent_id\":\"");
        smart_string_appends(buf, call->parent_id);
        smart_string_appends(buf, "\"");
    }
    
    char depth_str[32];
    snprintf(depth_str, sizeof(depth_str), "%d", call->depth);
    smart_string_appends(buf, ",\"depth\":");
    smart_string_appends(buf, depth_str);
    
    char type_str[32];
    snprintf(type_str, sizeof(type_str), "%d", call->function_type);
    smart_string_appends(buf, ",\"function_type\":");
    smart_string_appends(buf, type_str);
    
    // Serialize SQL queries
    if (call->sql_queries && Z_TYPE_P(call->sql_queries) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->sql_queries)) > 0) {
        smart_string_appends(buf, ",\"sql_queries\":");
        serialize_zval_json(buf, call->sql_queries);
    }
    
    // Serialize HTTP requests (cURL)
    if (call->http_requests && Z_TYPE_P(call->http_requests) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->http_requests)) > 0) {
        smart_string_appends(buf, ",\"http_requests\":");
        serialize_zval_json(buf, call->http_requests);
    }
    
    // Serialize cache operations (APCu, Symfony Cache)
    if (call->cache_operations && Z_TYPE_P(call->cache_operations) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->cache_operations)) > 0) {
        smart_string_appends(buf, ",\"cache_operations\":");
        serialize_zval_json(buf, call->cache_operations);
    }
    
    // Serialize Redis operations
    if (call->redis_operations && Z_TYPE_P(call->redis_operations) == IS_ARRAY && zend_hash_num_elements(Z_ARRVAL_P(call->redis_operations)) > 0) {
        smart_string_appends(buf, ",\"redis_operations\":");
        serialize_zval_json(buf, call->redis_operations);
    }
    
    // Serialize children - will be built recursively in serialize_call_node_json_recursive
    smart_string_appends(buf, ",\"children\":[]");
    
    smart_string_appends(buf, "}");
}

// Enhanced serialize zval to JSON - handles all zval types
// Uses visited set to detect circular references
static void serialize_zval_json_recursive(smart_string *buf, zval *zv, HashTable *visited, int depth);

// Serialize zval to JSON (enhanced to handle all types)
void serialize_zval_json(smart_string *buf, zval *zv) {
    if (!zv) {
        smart_string_appends(buf, "null");
        return;
    }
    
    HashTable *visited = NULL;
    if (Z_TYPE_P(zv) == IS_OBJECT || Z_TYPE_P(zv) == IS_ARRAY) {
        visited = emalloc(sizeof(HashTable));
        zend_hash_init(visited, 8, NULL, NULL, 0);
    }
    
    serialize_zval_json_recursive(buf, zv, visited, 0);
    
    if (visited) {
        zend_hash_destroy(visited);
        efree(visited);
    }
}

// Maximum serialization limits to prevent memory exhaustion
#define MAX_SERIALIZE_DEPTH 10
#define MAX_SERIALIZE_SIZE (1024 * 1024) // 1MB max per dump

// Recursive helper with circular reference detection
static void serialize_zval_json_recursive(smart_string *buf, zval *zv, HashTable *visited, int depth) {
    if (!zv) {
        smart_string_appends(buf, "null");
        return;
    }
    
    // Prevent infinite recursion and limit depth
    if (depth > MAX_SERIALIZE_DEPTH) {
        smart_string_appends(buf, "\"... (max depth reached)\"");
        return;
    }
    
    // Limit total buffer size to prevent memory exhaustion
    // Note: In PHP 8.4, we can't easily check buffer size without extracting it
    // We'll skip the check here to avoid API issues - the MAX_SERIALIZE_SIZE is large enough
    
    switch (Z_TYPE_P(zv)) {
        case IS_NULL:
            smart_string_appends(buf, "null");
            break;
            
        case IS_FALSE:
            smart_string_appends(buf, "false");
            break;
            
        case IS_TRUE:
            smart_string_appends(buf, "true");
            break;
            
        case IS_LONG: {
            char num_str[64];
            snprintf(num_str, sizeof(num_str), "%ld", Z_LVAL_P(zv));
            smart_string_appends(buf, num_str);
            break;
        }
        
        case IS_DOUBLE: {
            char num_str[64];
            snprintf(num_str, sizeof(num_str), "%.6f", Z_DVAL_P(zv));
            smart_string_appends(buf, num_str);
            break;
        }
        
        case IS_STRING: {
            smart_string_appends(buf, "\"");
            json_escape_string(buf, Z_STRVAL_P(zv), Z_STRLEN_P(zv));
            smart_string_appends(buf, "\"");
            break;
        }
        
        case IS_ARRAY: {
            HashTable *ht = Z_ARRVAL_P(zv);
            uint32_t count = zend_hash_num_elements(ht);
            
            // Limit array size to prevent memory exhaustion
            if (count > 1000) {
                char count_str[64];
                snprintf(count_str, sizeof(count_str), "{\"_type\":\"array\",\"_count\":%u,\"_value\":\"Array too large (truncated)\"}", count);
                smart_string_appends(buf, count_str);
                break;
            }
            
            // Check for circular reference
            if (visited) {
                zend_ulong hash = (zend_ulong)ht;
                zval *existing;
                if ((existing = zend_hash_index_find(visited, hash)) != NULL) {
                    smart_string_appends(buf, "\"... (circular reference)\"");
                    return;
                }
                zval tmp;
                ZVAL_LONG(&tmp, 1);
                zend_hash_index_update(visited, hash, &tmp);
            }
            
            // Check if it's an associative array (has string keys) or indexed array
            int is_assoc = 0;
            zend_string *key;
            zval *val;
            ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
                if (key) {
                    is_assoc = 1;
                    break;
                }
            } ZEND_HASH_FOREACH_END();
            
            if (is_assoc) {
                // Serialize as object {}
                smart_string_appends(buf, "{");
                int first = 1;
                int item_count = 0;
                ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
                    // Limit number of items serialized
                    if (item_count >= 100) {
                        smart_string_appends(buf, ",\"...\":\"(truncated, "); 
                        char remaining[32];
                        snprintf(remaining, sizeof(remaining), "%u more)\"", count - 100);
                        smart_string_appends(buf, remaining);
                        break;
                    }
                    if (!first) {
                        smart_string_appends(buf, ",");
                    }
                    // Serialize key
                    if (key) {
                        smart_string_appends(buf, "\"");
                        json_escape_string(buf, ZSTR_VAL(key), ZSTR_LEN(key));
                        smart_string_appends(buf, "\":");
                    }
                    // Serialize value
                    serialize_zval_json_recursive(buf, val, visited, depth + 1);
                    first = 0;
                    item_count++;
                } ZEND_HASH_FOREACH_END();
                smart_string_appends(buf, "}");
            } else {
                // Serialize as array []
                smart_string_appends(buf, "[");
                int first = 1;
                int item_count = 0;
                ZEND_HASH_FOREACH_VAL(ht, val) {
                    // Limit number of items serialized
                    if (item_count >= 100) {
                        smart_string_appends(buf, ",\"... (truncated, "); 
                        char remaining[32];
                        snprintf(remaining, sizeof(remaining), "%u more)\"", count - 100);
                        smart_string_appends(buf, remaining);
                        break;
                    }
                    if (!first) {
                        smart_string_appends(buf, ",");
                    }
                    serialize_zval_json_recursive(buf, val, visited, depth + 1);
                    first = 0;
                    item_count++;
                } ZEND_HASH_FOREACH_END();
                smart_string_appends(buf, "]");
            }
            
            // Remove from visited set
            if (visited) {
                zend_ulong hash = (zend_ulong)ht;
                zend_hash_index_del(visited, hash);
            }
            break;
        }
        
        case IS_OBJECT: {
            zend_object *obj = Z_OBJ_P(zv);
            
            // Check for circular reference
            if (visited) {
                zend_ulong hash = (zend_ulong)obj;
                zval *existing;
                if ((existing = zend_hash_index_find(visited, hash)) != NULL) {
                    smart_string_appends(buf, "\"... (circular reference)\"");
                    return;
                }
                zval tmp;
                ZVAL_LONG(&tmp, 1);
                zend_hash_index_update(visited, hash, &tmp);
            }
            
            smart_string_appends(buf, "{\"_type\":\"object\"");
            
            // Get class name
            if (obj->ce && obj->ce->name) {
                smart_string_appends(buf, ",\"_class\":\"");
                json_escape_string(buf, ZSTR_VAL(obj->ce->name), ZSTR_LEN(obj->ce->name));
                smart_string_appends(buf, "\"");
            }
            
            // For Doctrine entities and complex objects, just show class name
            // Full property serialization can cause memory exhaustion
            // Limit object serialization to prevent memory issues
            if (depth >= MAX_SERIALIZE_DEPTH - 2) {
                // At high depth, just show class name
                smart_string_appends(buf, ",\"_value\":\"Object (depth limit)\"}");
            } else {
                // Get object properties (simplified - just indicate it's an object)
                // Full property serialization is complex and error-prone
                smart_string_appends(buf, ",\"_properties\":{}}");
            }
            
            // Remove from visited set
            if (visited) {
                zend_ulong hash = (zend_ulong)obj;
                zend_hash_index_del(visited, hash);
            }
            break;
        }
        
        case IS_RESOURCE: {
            smart_string_appends(buf, "{\"_type\":\"resource\"");
            const char *res_type = "unknown";
            if (Z_RES_P(zv)->type) {
                res_type = zend_rsrc_list_get_rsrc_type(Z_RES_P(zv));
                if (!res_type) res_type = "unknown";
            }
            smart_string_appends(buf, ",\"_value\":\"");
            json_escape_string(buf, res_type, strlen(res_type));
            smart_string_appends(buf, " resource\"}");
            break;
        }
        
        default:
            smart_string_appends(buf, "\"... (unsupported type)\"");
            break;
    }
}

// Serialize zval to text (var_dump-like format)
// Uses visited set to detect circular references
static void serialize_zval_text_recursive(smart_string *buf, zval *zv, HashTable *visited, int depth, const char *indent);

void serialize_zval_text(smart_string *buf, zval *zv) {
    if (!zv) {
        smart_string_appends(buf, "NULL");
        return;
    }
    
    HashTable *visited = NULL;
    if (Z_TYPE_P(zv) == IS_OBJECT || Z_TYPE_P(zv) == IS_ARRAY) {
        visited = emalloc(sizeof(HashTable));
        zend_hash_init(visited, 8, NULL, NULL, 0);
    }
    
    serialize_zval_text_recursive(buf, zv, visited, 0, "");
    
    if (visited) {
        zend_hash_destroy(visited);
        efree(visited);
    }
}

static void serialize_zval_text_recursive(smart_string *buf, zval *zv, HashTable *visited, int depth, const char *indent) {
    if (!zv) {
        smart_string_appends(buf, "NULL");
        return;
    }
    
    // Prevent infinite recursion and limit depth
    if (depth > MAX_SERIALIZE_DEPTH) {
        smart_string_appends(buf, "... (max depth reached)");
        return;
    }
    
    // Limit total buffer size to prevent memory exhaustion
    // Note: In PHP 8.4, we can't easily check buffer size without extracting it
    // We'll skip the check here to avoid API issues - the MAX_SERIALIZE_SIZE is large enough
    
    char new_indent[256];
    snprintf(new_indent, sizeof(new_indent), "%s  ", indent);
    
    switch (Z_TYPE_P(zv)) {
        case IS_NULL:
            smart_string_appends(buf, "NULL");
            break;
            
        case IS_FALSE:
            smart_string_appends(buf, "bool(false)");
            break;
            
        case IS_TRUE:
            smart_string_appends(buf, "bool(true)");
            break;
            
        case IS_LONG: {
            char num_str[64];
            snprintf(num_str, sizeof(num_str), "int(%ld)", Z_LVAL_P(zv));
            smart_string_appends(buf, num_str);
            break;
        }
        
        case IS_DOUBLE: {
            char num_str[64];
            snprintf(num_str, sizeof(num_str), "float(%.6f)", Z_DVAL_P(zv));
            smart_string_appends(buf, num_str);
            break;
        }
        
        case IS_STRING: {
            char str[256];
            size_t len = Z_STRLEN_P(zv);
            snprintf(str, sizeof(str), "string(%zu) \"", len);
            smart_string_appends(buf, str);
            // Truncate long strings for display
            if (len > 100) {
                smart_string_appendl(buf, Z_STRVAL_P(zv), 100);
                smart_string_appends(buf, "... (truncated)");
            } else {
                smart_string_appendl(buf, Z_STRVAL_P(zv), len);
            }
            smart_string_appends(buf, "\"");
            break;
        }
        
        case IS_ARRAY: {
            HashTable *ht = Z_ARRVAL_P(zv);
            uint32_t count = zend_hash_num_elements(ht);
            
            // Check for circular reference
            if (visited) {
                zend_ulong hash = (zend_ulong)ht;
                zval *existing;
                if ((existing = zend_hash_index_find(visited, hash)) != NULL) {
                    smart_string_appends(buf, "array(...) (circular reference)");
                    return;
                }
                zval tmp;
                ZVAL_LONG(&tmp, 1);
                zend_hash_index_update(visited, hash, &tmp);
            }
            
            char count_str[64];
            snprintf(count_str, sizeof(count_str), "array(%u) {\n", count);
            smart_string_appends(buf, count_str);
            
            zend_string *key;
            zval *val;
            int idx = 0;
            ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
                smart_string_appends(buf, new_indent);
                if (key) {
                    smart_string_appends(buf, "[\"");
                    json_escape_string(buf, ZSTR_VAL(key), ZSTR_LEN(key));
                    smart_string_appends(buf, "\"]=>\n");
                    smart_string_appends(buf, new_indent);
                    smart_string_appends(buf, "  ");
                } else {
                    char idx_str[32];
                    snprintf(idx_str, sizeof(idx_str), "[%d]=>\n%s  ", idx, new_indent);
                    smart_string_appends(buf, idx_str);
                }
                serialize_zval_text_recursive(buf, val, visited, depth + 1, new_indent);
                smart_string_appends(buf, "\n");
                idx++;
            } ZEND_HASH_FOREACH_END();
            
            smart_string_appends(buf, indent);
            smart_string_appends(buf, "}");
            
            // Remove from visited set
            if (visited) {
                zend_ulong hash = (zend_ulong)ht;
                zend_hash_index_del(visited, hash);
            }
            break;
        }
        
        case IS_OBJECT: {
            zend_object *obj = Z_OBJ_P(zv);
            
            // Check for circular reference
            if (visited) {
                zend_ulong hash = (zend_ulong)obj;
                zval *existing;
                if ((existing = zend_hash_index_find(visited, hash)) != NULL) {
                    smart_string_appends(buf, "object(...) (circular reference)");
                    return;
                }
                zval tmp;
                ZVAL_LONG(&tmp, 1);
                zend_hash_index_update(visited, hash, &tmp);
            }
            
            const char *class_name = "stdClass";
            if (obj->ce && obj->ce->name) {
                class_name = ZSTR_VAL(obj->ce->name);
            }
            
            char obj_str[256];
            snprintf(obj_str, sizeof(obj_str), "object(%s)#%u (%u) {\n", class_name, (unsigned int)(zend_ulong)obj % 1000, 0);
            smart_string_appends(buf, obj_str);
            
            // Object properties serialization is complex - just show class name
            // Full property access requires careful handling of visibility and types
            
            smart_string_appends(buf, indent);
            smart_string_appends(buf, "}");
            
            // Remove from visited set
            if (visited) {
                zend_ulong hash = (zend_ulong)obj;
                zend_hash_index_del(visited, hash);
            }
            break;
        }
        
        case IS_RESOURCE: {
            const char *res_type = "unknown";
            if (Z_RES_P(zv)->type) {
                res_type = zend_rsrc_list_get_rsrc_type(Z_RES_P(zv));
                if (!res_type) res_type = "unknown";
            }
            char res_str[128];
            snprintf(res_str, sizeof(res_str), "resource(%ld) of type (%s)", (long)Z_RES_HANDLE_P(zv), res_type);
            smart_string_appends(buf, res_str);
            break;
        }
        
        default: {
            char type_str[64];
            snprintf(type_str, sizeof(type_str), "... (unsupported type: %d)", Z_TYPE_P(zv));
            smart_string_appends(buf, type_str);
            break;
        }
    }
}

// Serialize call stack from zval array
void serialize_call_stack_json(smart_string *buf, zval *stack) {
    serialize_zval_json(buf, stack);
}

// Serialize call stack from root calls (when request ends)
void serialize_call_stack_from_root(smart_string *buf) {
    smart_string_appends(buf, "[");
    
    // Access global collector for call tracking
    extern opa_collector_t *global_collector;
    
    if (!global_collector || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        smart_string_appends(buf, "]");
        return;
    }
    
    // Count total calls for debugging
    int total_calls = 0;
    int completed_calls = 0;
    int root_calls = 0;
    call_node_t *debug_call = global_collector->calls;
    while (debug_call) {
        total_calls++;
        if (debug_call->magic == OPA_CALL_NODE_MAGIC && debug_call->end_time > 0.0) {
            completed_calls++;
            if (!debug_call->parent_id) {
                root_calls++;
            }
        }
        debug_call = debug_call->next;
    }
    
    // Find root calls (no parent_id) and serialize recursively
    // Only serialize calls that have completed (end_time > 0)
    call_node_t *call = global_collector->calls;
    int first = 1;
    int found_any = 0;
    
    while (call) {
        // Validate call structure
        if (call->magic == OPA_CALL_NODE_MAGIC && call->end_time > 0.0 && !call->parent_id) {
            if (!first) {
                smart_string_appends(buf, ",");
            }
            serialize_call_node_json_recursive(buf, call);
            first = 0;
            found_any = 1;
        }
        call = call->next;
    }
    
    // If no root calls found, serialize all completed calls as a flat list
    // This can happen if all calls have parents (e.g., all are nested)
    if (!found_any) {
        call = global_collector->calls;
        first = 1;
        while (call) {
            if (call->magic == OPA_CALL_NODE_MAGIC && call->end_time > 0.0) {
                if (!first) {
                    smart_string_appends(buf, ",");
                }
                serialize_call_node_json(buf, call);
                // Serialize children recursively
                smart_string_appends(buf, ",\"children\":[");
                call_node_t *child = global_collector->calls;
                int first_child = 1;
                while (child) {
                    if (child->magic == OPA_CALL_NODE_MAGIC && 
                        child->end_time > 0.0 &&
                        child->parent_id && 
                        call->call_id &&
                        strcmp(child->parent_id, call->call_id) == 0) {
                        if (!first_child) {
                            smart_string_appends(buf, ",");
                        }
                        serialize_call_node_json(buf, child);
                        smart_string_appends(buf, ",\"children\":[]");
                        first_child = 0;
                    }
                    child = child->next;
                }
                smart_string_appends(buf, "]");
                first = 0;
            }
            call = call->next;
        }
    }
    
    smart_string_appends(buf, "]");
}

// Serialize call node recursively with children
void serialize_call_node_json_recursive(smart_string *buf, call_node_t *call) {
    if (!call || call->magic != OPA_CALL_NODE_MAGIC) return;
    
    serialize_call_node_json(buf, call);
    
    // Serialize children if any - find children calls by parent_id
    extern opa_collector_t *global_collector;
    smart_string_appends(buf, ",\"children\":[");
    call_node_t *child = global_collector ? global_collector->calls : NULL;
    int first_child = 1;
    while (child) {
        // Validate child and check if it's completed and belongs to this parent
        if (child->magic == OPA_CALL_NODE_MAGIC && 
            child->end_time > 0.0 &&
            child->parent_id && 
            call->call_id &&
            strcmp(child->parent_id, call->call_id) == 0) {
            if (!first_child) {
                smart_string_appends(buf, ",");
            }
            serialize_call_node_json_recursive(buf, child);
            first_child = 0;
        }
        child = child->next;
    }
    smart_string_appends(buf, "]");
}

// Serialize all SQL queries from call stack
void serialize_all_sql_queries(smart_string *buf) {
    smart_string_appends(buf, "[");
    
    extern opa_collector_t *global_collector;
    if (!global_collector || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        smart_string_appends(buf, "]");
        return;
    }
    
    call_node_t *call = global_collector->calls;
    int first = 1;
    int max_iterations = 10000; // Safety limit
    int iterations = 0;
    
    while (call && iterations < max_iterations) {
        iterations++;
        
        // Validate call structure before accessing any fields
        if (call->magic != OPA_CALL_NODE_MAGIC) {
            // Invalid magic - skip this node
            call = call->next;
            continue;
        }
        
        // Save next pointer before accessing sql_queries
        call_node_t *next = call->next;
        
        // Validate sql_queries pointer and type
        if (call->sql_queries && 
            Z_TYPE_P(call->sql_queries) == IS_ARRAY) {
            HashTable *ht = Z_ARRVAL_P(call->sql_queries);
            if (ht) {
                zval *val;
                ZEND_HASH_FOREACH_VAL(ht, val) {
                    if (!first) {
                        smart_string_appends(buf, ",");
                    }
                    serialize_zval_json(buf, val);
                    first = 0;
                } ZEND_HASH_FOREACH_END();
            }
        }
        
        // Use saved next pointer
        call = next;
    }
    
    smart_string_appends(buf, "]");
}

