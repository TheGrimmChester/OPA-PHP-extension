/* opa.c - APM extension for PHP 8.0-8.5
   Features:
   - Precise profiling with entry/exit callbacks
   - Stack sampling profiler
   - PDO, cURL, file I/O instrumentation
   - Network metrics
   - LZ4 compression
   - INI configuration
   - Manual span functions
*/
#include "opa.h"
#include "span.h"
#include "call_node.h"
#include "transport.h"
#include "serialize.h"
#include <time.h>
#include <stdio.h>

// Declare module globals (only in main file)
ZEND_DECLARE_MODULE_GLOBALS(opa)

// Custom INI update handler for double
PHP_INI_MH(OnUpdateSamplingRate) {
    double *p;
    char *base = (char *) mh_arg2;
    p = (double *) (base + (size_t) mh_arg1);
    *p = zend_strtod(ZSTR_VAL(new_value), NULL);
    return SUCCESS;
}

// INI configuration
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("opa.enabled", "1", PHP_INI_ALL, OnUpdateBool, enabled, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY_EX("opa.sampling_rate", "1.0", PHP_INI_ALL, OnUpdateSamplingRate, sampling_rate, zend_opa_globals, opa_globals, NULL)
    STD_PHP_INI_ENTRY("opa.socket_path", "/var/run/opa.sock", PHP_INI_ALL, OnUpdateString, socket_path, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.full_capture_threshold_ms", "100", PHP_INI_ALL, OnUpdateLong, full_capture_threshold_ms, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.stack_depth", "20", PHP_INI_ALL, OnUpdateLong, stack_depth, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.buffer_size", "65536", PHP_INI_ALL, OnUpdateLong, buffer_size, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.collect_internal_functions", "1", PHP_INI_ALL, OnUpdateBool, collect_internal_functions, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.debug_log", "0", PHP_INI_ALL, OnUpdateBool, debug_log_enabled, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.organization_id", "default-org", PHP_INI_ALL, OnUpdateString, organization_id, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.project_id", "default-project", PHP_INI_ALL, OnUpdateString, project_id, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.service", "php-fpm", PHP_INI_ALL, OnUpdateString, service, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.language", "php", PHP_INI_ALL, OnUpdateString, language, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.language_version", "", PHP_INI_ALL, OnUpdateString, language_version, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.framework", "", PHP_INI_ALL, OnUpdateString, framework, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.framework_version", "", PHP_INI_ALL, OnUpdateString, framework_version, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.track_errors", "1", PHP_INI_ALL, OnUpdateBool, track_errors, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.track_logs", "1", PHP_INI_ALL, OnUpdateBool, track_logs, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.log_levels", "critical,error", PHP_INI_ALL, OnUpdateString, log_levels, zend_opa_globals, opa_globals)
    STD_PHP_INI_ENTRY("opa.expand_spans", "1", PHP_INI_ALL, OnUpdateBool, expand_spans, zend_opa_globals, opa_globals)
PHP_INI_END()

// Global state (declared in opa.h, defined here)
HashTable *active_spans = NULL;
pthread_mutex_t active_spans_mutex = PTHREAD_MUTEX_INITIALIZER;
// Root span data stored in malloc'd memory to avoid emalloc segfaults
char *root_span_trace_id = NULL;
char *root_span_span_id = NULL;
char *root_span_parent_id = NULL;
char *root_span_name = NULL;
char *root_span_url_scheme = NULL;
char *root_span_url_host = NULL;
char *root_span_url_path = NULL;
char *root_span_cli_args_json = NULL; // CLI arguments as JSON string (malloc'd)
char *root_span_http_request_json = NULL; // HTTP request details as JSON string (malloc'd)
char *root_span_http_response_json = NULL; // HTTP response details as JSON string (malloc'd)
long root_span_start_ts = 0;
long root_span_end_ts = 0;
int root_span_cpu_ms = 0;
int root_span_status = -1;
zval *root_span_dumps = NULL; // Root span dumps array (emalloc'd, valid until RSHUTDOWN)
pthread_mutex_t root_span_data_mutex = PTHREAD_MUTEX_INITIALIZER;
int profiling_active = 0;
opa_collector_t *global_collector = NULL; 
size_t network_bytes_sent_total = 0;
size_t network_bytes_received_total = 0;
pthread_mutex_t network_mutex = PTHREAD_MUTEX_INITIALIZER;

// NOTE: zend_execute_ex hook is no longer used - Observer API is used instead
// Keeping this variable for potential fallback scenarios
void (*original_zend_execute_ex)(zend_execute_data *execute_data) = NULL;

// Re-entrancy guard: thread-local flag to prevent infinite recursion
// When opa_execute_ex calls functions that trigger zend_execute_ex again,
// this flag ensures we bypass the hook logic and call original directly
static __thread int in_opa_execute_ex = 0;

// Re-entrancy guard for observer callbacks
static __thread int in_opa_observer = 0;

// Stack sampling (disabled for now - can be re-enabled later if needed)
// static timer_t sampling_timer;
// static int sampling_enabled = 0;

// Helper: Generate unique ID
// CRITICAL: This function must NOT call any PHP functions that could trigger observers
// Use manual hex conversion to avoid any PHP function interception (sprintf/snprintf)
char* generate_id() {
    char *id = emalloc(17);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // Manual hex conversion to avoid triggering PHP function observers
    // Format: 16 hex digits + null terminator
    unsigned long value = (unsigned long)(tv.tv_sec * 1000000 + tv.tv_usec) ^ (unsigned long)pthread_self();
    
    // Manual hex conversion - no PHP functions called
    static const char hex_chars[] = "0123456789abcdef";
    int i;
    for (i = 15; i >= 0; i--) {
        id[i] = hex_chars[value & 0xf];
        value >>= 4;
    }
    id[16] = '\0'; // Ensure null termination
    return id;
}

// Helper: Get current timestamp in milliseconds
long get_timestamp_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)(tv.tv_sec * 1000LL + tv.tv_usec / 1000);
}

// Helper: Get current time in seconds with microseconds (for call stack)
double get_time_seconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

// Helper: Get current memory usage
size_t get_memory_usage() {
    return zend_memory_usage(0);
}

// Helper: Get current CPU time (user + system) in seconds
double get_cpu_time() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        double user_time = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1000000.0;
        double system_time = (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1000000.0;
        return user_time + system_time;
    }
    return 0.0;
}

// Helper: Get network bytes sent
size_t get_bytes_sent() {
    pthread_mutex_lock(&network_mutex);
    size_t bytes = network_bytes_sent_total;
    pthread_mutex_unlock(&network_mutex);
    return bytes;
}

// Helper: Extract URL components from $_SERVER variables
// Returns 1 on success, 0 on failure
// Allocates strings with emalloc (caller must free with efree)
int extract_url_components(zval *server, char **scheme, char **host, char **path) {
    debug_log("[extract_url_components] Called, server=%p", server);
    if (!server || Z_TYPE_P(server) != IS_ARRAY) {
        debug_log("[extract_url_components] Server is NULL or not an array");
        return 0;
    }
    
    // Initialize outputs
    *scheme = NULL;
    *host = NULL;
    *path = NULL;
    
    // Try to get path - try multiple sources in order of preference
    zval *request_uri = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
    if (!request_uri || Z_TYPE_P(request_uri) != IS_STRING || Z_STRLEN_P(request_uri) == 0) {
        // Try SCRIPT_NAME + PATH_INFO
        zval *script_name = zend_hash_str_find(Z_ARRVAL_P(server), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
        zval *path_info = zend_hash_str_find(Z_ARRVAL_P(server), "PATH_INFO", sizeof("PATH_INFO")-1);
        
        if (script_name && Z_TYPE_P(script_name) == IS_STRING && Z_STRLEN_P(script_name) > 0) {
            size_t path_len = Z_STRLEN_P(script_name);
            if (path_info && Z_TYPE_P(path_info) == IS_STRING && Z_STRLEN_P(path_info) > 0) {
                path_len += Z_STRLEN_P(path_info);
            }
            char *combined_path = emalloc(path_len + 1);
            snprintf(combined_path, path_len + 1, "%s%s", 
                Z_STRVAL_P(script_name),
                (path_info && Z_TYPE_P(path_info) == IS_STRING) ? Z_STRVAL_P(path_info) : "");
            *path = combined_path;
            debug_log("[extract_url_components] Using SCRIPT_NAME+PATH_INFO: %s", *path);
        } else if (path_info && Z_TYPE_P(path_info) == IS_STRING && Z_STRLEN_P(path_info) > 0) {
            *path = estrndup(Z_STRVAL_P(path_info), Z_STRLEN_P(path_info));
            debug_log("[extract_url_components] Using PATH_INFO: %s", *path);
        } else {
            debug_log("[extract_url_components] No path found in REQUEST_URI, SCRIPT_NAME, or PATH_INFO");
        return 0;
    }
    } else {
    debug_log("[extract_url_components] Found REQUEST_URI: %s", Z_STRVAL_P(request_uri));
    *path = estrndup(Z_STRVAL_P(request_uri), Z_STRLEN_P(request_uri));
    }
    
    // Get scheme (http or https) - optional
    const char *scheme_val = "http";
    zval *scheme_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_SCHEME", sizeof("REQUEST_SCHEME")-1);
    if (!scheme_zv || Z_TYPE_P(scheme_zv) != IS_STRING) {
        scheme_zv = zend_hash_str_find(Z_ARRVAL_P(server), "HTTP_X_FORWARDED_PROTO", sizeof("HTTP_X_FORWARDED_PROTO")-1);
    }
    if (scheme_zv && Z_TYPE_P(scheme_zv) == IS_STRING && Z_STRLEN_P(scheme_zv) > 0) {
        scheme_val = Z_STRVAL_P(scheme_zv);
    }
    *scheme = estrdup(scheme_val);
    
    // Get host (HTTP_HOST or SERVER_NAME) - optional
    zval *host_zv = zend_hash_str_find(Z_ARRVAL_P(server), "HTTP_HOST", sizeof("HTTP_HOST")-1);
    if (!host_zv || Z_TYPE_P(host_zv) != IS_STRING) {
        host_zv = zend_hash_str_find(Z_ARRVAL_P(server), "SERVER_NAME", sizeof("SERVER_NAME")-1);
        if (host_zv && Z_TYPE_P(host_zv) == IS_STRING) {
            // If using SERVER_NAME, also check SERVER_PORT
            zval *port_zv = zend_hash_str_find(Z_ARRVAL_P(server), "SERVER_PORT", sizeof("SERVER_PORT")-1);
            if (port_zv) {
                char port_str[32] = {0};
                int port_valid = 0;
                if (Z_TYPE_P(port_zv) == IS_STRING) {
                    snprintf(port_str, sizeof(port_str), "%s", Z_STRVAL_P(port_zv));
                    port_valid = 1;
                } else if (Z_TYPE_P(port_zv) == IS_LONG) {
                    snprintf(port_str, sizeof(port_str), "%ld", Z_LVAL_P(port_zv));
                    port_valid = 1;
                }
                // Don't modify zval - just skip port if it's not string or long
                if (port_valid) {
                size_t host_len = Z_STRLEN_P(host_zv) + 1 + strlen(port_str) + 1;
                char *host_with_port = emalloc(host_len);
                snprintf(host_with_port, host_len, "%s:%s", Z_STRVAL_P(host_zv), port_str);
                *host = host_with_port;
                    debug_log("[extract_url_components] Success: scheme=%s, host=%s, path=%s", *scheme, *host, *path);
                return 1;
                }
            }
        }
    }
    if (host_zv && Z_TYPE_P(host_zv) == IS_STRING && Z_STRLEN_P(host_zv) > 0) {
        *host = estrndup(Z_STRVAL_P(host_zv), Z_STRLEN_P(host_zv));
        debug_log("[extract_url_components] Success: scheme=%s, host=%s, path=%s", *scheme, *host, *path);
        return 1;
    }
    
    // Success even without host - at least we have path
    debug_log("[extract_url_components] Success (path only): scheme=%s, path=%s", *scheme, *path);
    return 1;
}

// Helper: Get network bytes received
size_t get_bytes_received() {
    pthread_mutex_lock(&network_mutex);
    size_t bytes = network_bytes_received_total;
    pthread_mutex_unlock(&network_mutex);
    return bytes;
}

// Helper: Add bytes sent
void add_bytes_sent(size_t bytes) {
    pthread_mutex_lock(&network_mutex);
    network_bytes_sent_total += bytes;
    pthread_mutex_unlock(&network_mutex);
}

// Helper: Add bytes received
void add_bytes_received(size_t bytes) {
    pthread_mutex_lock(&network_mutex);
    network_bytes_received_total += bytes;
    pthread_mutex_unlock(&network_mutex);
}

// Helper: Serialize CLI arguments from $argv to JSON string
char* serialize_cli_args_json(zval *argv) {
    if (!argv || Z_TYPE_P(argv) != IS_ARRAY) {
        return NULL;
    }
    
    smart_string json = {0};
    smart_string_appends(&json, "{\"script\":");
    
    // First element is script name
    zval *script = zend_hash_index_find(Z_ARRVAL_P(argv), 0);
    if (script && Z_TYPE_P(script) == IS_STRING) {
        smart_string_appends(&json, "\"");
        json_escape_string(&json, Z_STRVAL_P(script), Z_STRLEN_P(script));
        smart_string_appends(&json, "\"");
    } else {
        smart_string_appends(&json, "null");
    }
    
    smart_string_appends(&json, ",\"args\":[");
    
    // Remaining elements are arguments
    zend_ulong num_elements = zend_hash_num_elements(Z_ARRVAL_P(argv));
    int first = 1;
    for (zend_ulong i = 1; i < num_elements; i++) {
        zval *arg = zend_hash_index_find(Z_ARRVAL_P(argv), i);
        if (arg) {
            if (!first) {
                smart_string_appends(&json, ",");
            }
            if (Z_TYPE_P(arg) == IS_STRING) {
                smart_string_appends(&json, "\"");
                json_escape_string(&json, Z_STRVAL_P(arg), Z_STRLEN_P(arg));
                smart_string_appends(&json, "\"");
            } else {
                // Convert to string for non-string args
                zval str_arg;
                ZVAL_COPY(&str_arg, arg);
                convert_to_string(&str_arg);
                smart_string_appends(&json, "\"");
                json_escape_string(&json, Z_STRVAL(str_arg), Z_STRLEN(str_arg));
                smart_string_appends(&json, "\"");
                zval_dtor(&str_arg);
            }
            first = 0;
        }
    }
    
    smart_string_appends(&json, "]}");
    smart_string_0(&json);
    
    // Convert to malloc'd string (safe after fastcgi_finish_request)
    char *result = NULL;
    if (json.c && json.len > 0) {
        result = malloc(json.len + 1);
        if (result) {
            memcpy(result, json.c, json.len);
            result[json.len] = '\0';
        }
    }
    smart_string_free(&json);
    
    return result;
}

// Helper: Serialize HTTP request details from $_SERVER to JSON string
// Simple implementation using snprintf for safety
// Forward declaration
char* serialize_http_request_json_universal(void);
char* safe_serialize_request(void);

// Helper: Calculate JSON-escaped length (worst case: every char becomes 6 chars for \\uXXXX)
static size_t json_escaped_length(const char *str, size_t len) {
    size_t escaped_len = 0;
    const char *p = str;
    const char *end = str + len;
    while (p < end) {
        unsigned char c = *p++;
        switch (c) {
            case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
                escaped_len += 2; // Escaped as \x
                break;
            default:
                if (c < 0x20) {
                    escaped_len += 6; // Escaped as \uXXXX
                } else {
                    escaped_len += 1;
                }
                break;
        }
    }
    return escaped_len;
}

// Helper: Escape JSON string and write to buffer
static size_t json_escape_and_write(char *dest, size_t dest_size, const char *str, size_t len) {
    size_t pos = 0;
    const char *p = str;
    const char *end = str + len;
    while (p < end && pos < dest_size - 1) {
        unsigned char c = *p++;
        switch (c) {
            case '"':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = '"';
                }
                break;
            case '\\':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = '\\';
                }
                break;
            case '\b':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = 'b';
                }
                break;
            case '\f':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = 'f';
                }
                break;
            case '\n':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = 'n';
                }
                break;
            case '\r':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = 'r';
                }
                break;
            case '\t':
                if (pos + 2 < dest_size) {
                    dest[pos++] = '\\';
                    dest[pos++] = 't';
                }
                break;
            default:
                if (c < 0x20) {
                    if (pos + 6 < dest_size) {
                        snprintf(dest + pos, dest_size - pos, "\\u%04x", c);
                        pos += 6;
                    }
                } else {
                    dest[pos++] = c;
                }
                break;
        }
    }
    dest[pos] = '\0';
    return pos;
}

// Helper: Add server field to JSON buffer with proper escaping and reallocation
// Returns new position in buffer, updates *result and *buf_size if reallocation occurs
static int add_server_field_json(char **result, size_t *buf_size, int pos, 
                                 zval *server_zv, const char *server_key, size_t server_key_len,
                                 const char *json_key, size_t max_len, int field_type) {
    // field_type: 0 = string, 1 = integer, 2 = boolean (presence)
    if (!server_zv || Z_TYPE_P(server_zv) != IS_ARRAY) {
        return pos;
    }
    
    zval *zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), server_key, server_key_len);
    if (!zv) {
        return pos;
    }
    
    // Calculate required space for this field
    size_t required_space = 0;
    char temp_buf[128];
    
    if (field_type == 0) { // String field
        if (Z_TYPE_P(zv) != IS_STRING || Z_STRLEN_P(zv) == 0 || Z_STRLEN_P(zv) > max_len) {
            return pos;
        }
        // Calculate: ,"key":"escaped_value"
        size_t escaped_len = json_escaped_length(Z_STRVAL_P(zv), Z_STRLEN_P(zv));
        required_space = 3 + strlen(json_key) + 2 + escaped_len; // ,"key":"value"
    } else if (field_type == 1) { // Integer field
        if (Z_TYPE_P(zv) == IS_LONG) {
            snprintf(temp_buf, sizeof(temp_buf), "%ld", Z_LVAL_P(zv));
            required_space = 3 + strlen(json_key) + 1 + strlen(temp_buf); // ,"key":value
        } else if (Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0 && Z_STRLEN_P(zv) < max_len) {
            // Try to parse as integer
            long port_val = strtol(Z_STRVAL_P(zv), NULL, 10);
            if (port_val > 0 && port_val < 65536) {
                snprintf(temp_buf, sizeof(temp_buf), "%ld", port_val);
                required_space = 3 + strlen(json_key) + 1 + strlen(temp_buf);
            } else {
                return pos;
            }
        } else {
            return pos;
        }
    } else if (field_type == 2) { // Boolean presence
        if (Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0) {
            required_space = 3 + strlen(json_key) + 1 + 4; // ,"key":true
        } else {
            return pos;
        }
    }
    
    // Reallocate buffer if needed
    if (pos + required_space >= *buf_size) {
        size_t new_size = (*buf_size) * 2 + required_space + 100;
        char *new_result = realloc(*result, new_size);
        if (!new_result) {
            return pos; // Reallocation failed, skip this field
        }
        *result = new_result;
        *buf_size = new_size;
    }
    
    // Add field to JSON
    if (field_type == 0) { // String
        pos += snprintf(*result + pos, *buf_size - pos, ",\"%s\":\"", json_key);
        size_t escaped_written = json_escape_and_write(*result + pos, *buf_size - pos, 
                                                       Z_STRVAL_P(zv), Z_STRLEN_P(zv));
        pos += escaped_written;
        pos += snprintf(*result + pos, *buf_size - pos, "\"");
    } else if (field_type == 1) { // Integer
        pos += snprintf(*result + pos, *buf_size - pos, ",\"%s\":%s", json_key, temp_buf);
    } else if (field_type == 2) { // Boolean presence
        pos += snprintf(*result + pos, *buf_size - pos, ",\"%s\":true", json_key);
    }
    
    return pos;
}

// FPM-Optimized Universal Serializer - PG(http_globals) FIRST, then SG(request_info) fallback
// FPM pattern: SG(request_info) NULL → PG(http_globals) populated
// CRITICAL: Must call zend_is_auto_global() to initialize $_SERVER before accessing PG(http_globals)
char* serialize_http_request_json_universal(void) {
    /* CRITICAL FIX: Initialize $_SERVER using zend_is_auto_global() before accessing PG(http_globals) */
    zend_string *server_name = zend_string_init("_SERVER", sizeof("_SERVER")-1, 0);
    zend_is_auto_global(server_name);
    zend_string_release(server_name);
    
    /* FPM PRIORITY 1: PG(http_globals)[TRACK_VARS_SERVER] - NOW properly initialized */
    zval *server = &PG(http_globals)[TRACK_VARS_SERVER];
    
    const char *method = "GET";
    const char *uri = "/";
    const char *query = "";
    size_t method_len = 3;
    size_t uri_len = 1;
    size_t query_len = 0;
    
    if (server && Z_TYPE_P(server) == IS_ARRAY) {
        int num_elements = (int)zend_hash_num_elements(Z_ARRVAL_P(server));
        
        if (num_elements > 0) {
            zval *method_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
            zval *query_zv = zend_hash_str_find(Z_ARRVAL_P(server), "QUERY_STRING", sizeof("QUERY_STRING")-1);
            zval *remote_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REMOTE_ADDR", sizeof("REMOTE_ADDR")-1);
            
            // For Symfony and frameworks with front controllers, prefer PATH_INFO over REQUEST_URI
            // Store both: uri (cleaned route) and request_uri (original full path)
            zval *path_info_zv = zend_hash_str_find(Z_ARRVAL_P(server), "PATH_INFO", sizeof("PATH_INFO")-1);
            zval *uri_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
            const char *request_uri_original = NULL;
            size_t request_uri_original_len = 0;
            
            // Capture original REQUEST_URI (without query string) for storage
            if (uri_zv && Z_TYPE_P(uri_zv) == IS_STRING && Z_STRLEN_P(uri_zv) > 0) {
                const char *raw_uri = Z_STRVAL_P(uri_zv);
                char *query_start = strchr(raw_uri, '?');
                request_uri_original_len = query_start ? (query_start - raw_uri) : Z_STRLEN_P(uri_zv);
                // Store original REQUEST_URI (we'll include it in JSON)
                static char request_uri_buf[2048];
                if (request_uri_original_len < sizeof(request_uri_buf) - 1) {
                    memcpy(request_uri_buf, raw_uri, request_uri_original_len);
                    request_uri_buf[request_uri_original_len] = '\0';
                    request_uri_original = request_uri_buf;
                } else {
                    request_uri_original = raw_uri;
                }
            }
            
            if (path_info_zv && Z_TYPE_P(path_info_zv) == IS_STRING && Z_STRLEN_P(path_info_zv) > 0) {
                // Use PATH_INFO (actual route without front controller)
                uri = Z_STRVAL_P(path_info_zv);
                uri_len = Z_STRLEN_P(path_info_zv);
                debug_log("[serialize_http_request_json_universal] Using PATH_INFO: %s (original REQUEST_URI: %s)", uri, request_uri_original ? request_uri_original : "N/A");
            } else if (uri_zv && Z_TYPE_P(uri_zv) == IS_STRING && Z_STRLEN_P(uri_zv) > 0) {
                // Fallback to REQUEST_URI, but clean it
                const char *raw_uri = Z_STRVAL_P(uri_zv);
                // Remove query string
                char *query_start = strchr(raw_uri, '?');
                size_t raw_uri_len = query_start ? (query_start - raw_uri) : Z_STRLEN_P(uri_zv);
                
                // Remove /index.php prefix if present
                if (raw_uri_len >= 10 && strncmp(raw_uri, "/index.php", 10) == 0) {
                    if (raw_uri_len == 10) {
                        uri = "/";
                        uri_len = 1;
                    } else {
                        uri = raw_uri + 10; // Skip /index.php
                        uri_len = raw_uri_len - 10;
                    }
                    debug_log("[serialize_http_request_json_universal] Cleaned REQUEST_URI (removed /index.php): %s (original: %s)", uri, request_uri_original ? request_uri_original : "N/A");
                } else {
                    // No /index.php prefix, use as-is but remove query string
                    static char cleaned_uri[2048];
                    if (raw_uri_len < sizeof(cleaned_uri) - 1) {
                        memcpy(cleaned_uri, raw_uri, raw_uri_len);
                        cleaned_uri[raw_uri_len] = '\0';
                        uri = cleaned_uri;
                        uri_len = raw_uri_len;
                    } else {
                        uri = raw_uri;
                        uri_len = raw_uri_len;
                    }
                }
            }
            
            if (method_zv && Z_TYPE_P(method_zv) == IS_STRING && uri) {
                method = Z_STRVAL_P(method_zv);
                method_len = Z_STRLEN_P(method_zv);
                if (query_zv && Z_TYPE_P(query_zv) == IS_STRING) {
                    query = Z_STRVAL_P(query_zv);
                    query_len = Z_STRLEN_P(query_zv);
                }
                
                // Calculate request size (headers + body + files)
                size_t request_size = 0;
                size_t body_size = (size_t)SG(request_info).content_length;
                size_t file_size = 0;
                size_t header_size = 0;
                
                // Try to get file sizes from $_FILES (may not be available in RINIT)
                zend_string *files_name = zend_string_init("_FILES", sizeof("_FILES")-1, 0);
                zend_is_auto_global(files_name);
                zval *files_zv = zend_hash_find(&EG(symbol_table), files_name);
                zend_string_release(files_name);
                
                if (files_zv && Z_TYPE_P(files_zv) == IS_ARRAY) {
                    // Iterate through $_FILES array
                    zval *file_entry;
                    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(files_zv), file_entry) {
                        if (Z_TYPE_P(file_entry) == IS_ARRAY) {
                            zval *size_zv = zend_hash_str_find(Z_ARRVAL_P(file_entry), "size", sizeof("size")-1);
                            if (size_zv) {
                                if (Z_TYPE_P(size_zv) == IS_LONG) {
                                    file_size += (size_t)Z_LVAL_P(size_zv);
                                } else if (Z_TYPE_P(size_zv) == IS_DOUBLE) {
                                    file_size += (size_t)Z_DVAL_P(size_zv);
                                }
                            }
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                // Estimate header size from $_SERVER HTTP_* entries
                header_size += method_len + uri_len + 15; // Request line
                
                zend_string *key;
                zval *value;
                ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(server), key, value) {
                    if (key && ZSTR_LEN(key) > 5) {
                        const char *key_str = ZSTR_VAL(key);
                        if (strncmp(key_str, "HTTP_", 5) == 0 && value && Z_TYPE_P(value) == IS_STRING) {
                            size_t header_name_len = ZSTR_LEN(key) - 5;
                            header_size += header_name_len + Z_STRLEN_P(value) + 4; // ": \r\n" = 4 bytes
                        }
                    }
                } ZEND_HASH_FOREACH_END();
                
                request_size = body_size + query_len + file_size + header_size;
                
                // Allocate dynamic buffer with initial size to accommodate extended fields
                size_t buf_size = 6000; // Increased initial buffer size for extended fields
                char *buf = malloc(buf_size);
                if (!buf) {
                    return strdup("{\"method\":\"GET\",\"uri\":\"/\"}");
                }
                
                int pos = snprintf(buf, buf_size,
                    "{\"method\":\"%s\",\"uri\":\"%s\","
                     "\"query_string\":\"%s\",\"remote_addr\":\"%s\","
                     "\"source\":\"PG\"",
                    method,
                    uri,
                    query,
                    remote_zv && Z_TYPE_P(remote_zv) == IS_STRING ? Z_STRVAL_P(remote_zv) : "unknown");
                
                // Add original request_uri if available (for ClickHouse storage)
                if (request_uri_original && strlen(request_uri_original) > 0) {
                    if (pos + 1000 < buf_size) {
                        pos += snprintf(buf + pos, buf_size - pos, ",\"request_uri\":\"%s\"", request_uri_original);
                    }
                }
                
                // Add extended fields from $_SERVER
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_USER_AGENT", sizeof("HTTP_USER_AGENT")-1, "user_agent", 500, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_REFERER", sizeof("HTTP_REFERER")-1, "referer", 1000, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_ACCEPT_LANGUAGE", sizeof("HTTP_ACCEPT_LANGUAGE")-1, "accept_language", 200, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_CONTENT_TYPE", sizeof("HTTP_CONTENT_TYPE")-1, "content_type", 200, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_ACCEPT", sizeof("HTTP_ACCEPT")-1, "accept", 500, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_ACCEPT_ENCODING", sizeof("HTTP_ACCEPT_ENCODING")-1, "accept_encoding", 200, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "SERVER_PORT", sizeof("SERVER_PORT")-1, "port", 10, 1);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_X_REQUEST_ID", sizeof("HTTP_X_REQUEST_ID")-1, "request_id", 100, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_X_TRACE_ID", sizeof("HTTP_X_TRACE_ID")-1, "trace_id", 100, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_ORIGIN", sizeof("HTTP_ORIGIN")-1, "origin", 500, 0);
                pos = add_server_field_json(&buf, &buf_size, pos, server, "HTTP_CONNECTION", sizeof("HTTP_CONNECTION")-1, "connection", 50, 0);
                
                // Handle HTTP_COOKIE - size only for security
                zval *cookie_zv = zend_hash_str_find(Z_ARRVAL_P(server), "HTTP_COOKIE", sizeof("HTTP_COOKIE")-1);
                if (cookie_zv && Z_TYPE_P(cookie_zv) == IS_STRING && Z_STRLEN_P(cookie_zv) > 0) {
                    size_t cookie_size = Z_STRLEN_P(cookie_zv);
                    size_t required_space = 20; // ,"cookie_size":1234
                    if (pos + required_space >= buf_size) {
                        size_t new_size = buf_size * 2 + required_space;
                        char *new_buf = realloc(buf, new_size);
                        if (new_buf) {
                            buf = new_buf;
                            buf_size = new_size;
                        }
                    }
                    if (pos + required_space < buf_size) {
                        pos += snprintf(buf + pos, buf_size - pos, ",\"cookie_size\":%zu", cookie_size);
                    }
                }
                
                // Handle HTTP_AUTHORIZATION - presence only for security
                zval *auth_zv = zend_hash_str_find(Z_ARRVAL_P(server), "HTTP_AUTHORIZATION", sizeof("HTTP_AUTHORIZATION")-1);
                if (auth_zv && Z_TYPE_P(auth_zv) == IS_STRING && Z_STRLEN_P(auth_zv) > 0) {
                    size_t required_space = 30; // ,"authorization_present":true
                    if (pos + required_space >= buf_size) {
                        size_t new_size = buf_size * 2 + required_space;
                        char *new_buf = realloc(buf, new_size);
                        if (new_buf) {
                            buf = new_buf;
                            buf_size = new_size;
                        }
                    }
                    if (pos + required_space < buf_size) {
                        pos += snprintf(buf + pos, buf_size - pos, ",\"authorization_present\":true");
                    }
                }
                
                // Always add request_size (even if 0, for debugging)
                size_t size_str_len = 200; // Enough for size and breakdown
                if (pos + size_str_len >= buf_size) {
                    size_t new_size = buf_size * 2 + size_str_len;
                    char *new_buf = realloc(buf, new_size);
                    if (new_buf) {
                        buf = new_buf;
                        buf_size = new_size;
                    }
                }
                if (pos + size_str_len < buf_size) {
                    pos += snprintf(buf + pos, buf_size - pos, ",\"request_size\":%zu", request_size);
                    // Also add breakdown for debugging
                    pos += snprintf(buf + pos, buf_size - pos, ",\"request_size_breakdown\":{\"body\":%zu,\"query\":%zu,\"files\":%zu,\"headers\":%zu}",
                        body_size, query_len, file_size, header_size);
                }
                
                if (pos < buf_size) {
                    snprintf(buf + pos, buf_size - pos, "}");
                }
                
                return buf;
            }
        }
    }
    
    /* PRIORITY 2: SAPI fallback (CLI/Apache) */
    
    method = SG(request_info).request_method ? SG(request_info).request_method : "GET";
    uri = SG(request_info).request_uri ? SG(request_info).request_uri : "/";
    query = SG(request_info).query_string ? SG(request_info).query_string : "";
    method_len = strlen(method);
    uri_len = strlen(uri);
    query_len = strlen(query);
    
    // Calculate request size for SAPI fallback
    size_t request_size = 0;
    size_t body_size = (size_t)SG(request_info).content_length;
    size_t header_size = method_len + uri_len + 15; // Request line
    header_size += 100; // Estimate for common headers
    
    request_size = body_size + query_len + header_size;
    
    char buf[2048]; // Increased buffer size for request_uri field
    int pos = snprintf(buf, sizeof(buf),
        "{\"method\":\"%s\",\"uri\":\"%s\",\"query_string\":\"%s\","
         "\"source\":\"SAPI\"",
        method, uri, query);
    
    // Add original request_uri if available (for ClickHouse storage)
    // For SAPI fallback, REQUEST_URI from SG(request_info) is the original
    if (SG(request_info).request_uri && strlen(SG(request_info).request_uri) > 0) {
        const char *raw_uri = SG(request_info).request_uri;
        char *query_start = strchr(raw_uri, '?');
        size_t uri_len = query_start ? (query_start - raw_uri) : strlen(raw_uri);
        static char request_uri_buf[2048];
        if (uri_len < sizeof(request_uri_buf) - 1) {
            memcpy(request_uri_buf, raw_uri, uri_len);
            request_uri_buf[uri_len] = '\0';
            pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"request_uri\":\"%s\"", request_uri_buf);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"request_uri\":\"%.2047s\"", raw_uri);
        }
    }
    
    // Always add request_size (even if 0, for debugging)
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"request_size\":%zu", request_size);
    // Also add breakdown for debugging
    pos += snprintf(buf + pos, sizeof(buf) - pos, ",\"request_size_breakdown\":{\"body\":%zu,\"query\":%zu,\"files\":0,\"headers\":%zu}",
        body_size, query_len, header_size);
    
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    
    return strdup(buf);
}

// Safe HTTP request serialization using ONLY SAPI globals - 100% safe in RINIT
// This function uses ONLY SG(request_info) - no $_SERVER access
// $_SERVER superglobals are NOT populated until AFTER RINIT, so we can't use them here
// Uses ONLY confirmed real sapi_request_info fields: request_method, request_uri, query_string, content_length
char* safe_serialize_request(void) {
    char buf[2048];
    
    // ONLY REAL FIELDS from sapi_request_info (confirmed to exist)
    snprintf(buf, sizeof(buf),
        "{\"method\":\"%s\","
         "\"uri\":\"%s\","
         "\"query_string\":\"%s\","
         "\"content_length\":%d}",
        SG(request_info).request_method ? SG(request_info).request_method : "GET",
        SG(request_info).request_uri ? SG(request_info).request_uri : "/",
        SG(request_info).query_string ? SG(request_info).query_string : "",
        SG(request_info).content_length);
    
    debug_log("[safe_serialize_request] Generated JSON from SAPI globals: %.200s", buf);
    
    return strdup(buf);
}

// Enhanced HTTP request serialization with scheme/host/remote_addr from $_SERVER (use in RSHUTDOWN)
// This version adds scheme, host, and remote_addr from $_SERVER superglobal
// $_SERVER is available in RSHUTDOWN, so we can safely access it here
char* serialize_http_request_json(zval *server) {
    // Start with safe data from SG(request_info) - always available
    const char *method = SG(request_info).request_method ? SG(request_info).request_method : "GET";
    const char *uri = SG(request_info).request_uri ? SG(request_info).request_uri : "/";
    const char *query = SG(request_info).query_string ? SG(request_info).query_string : "";
    const char *request_uri_original = NULL;
    
    debug_log("[serialize_http_request_json] Starting with method=%s, uri=%s, query=%s", method, uri, query);
    
    // Extract scheme and host from $_SERVER superglobal (if available)
    const char *scheme = "http";
    const char *host = NULL;
    size_t host_len = 0;
    
    zval *server_zv = server;
    if (!server_zv) {
        // Safely get $_SERVER using zend_is_auto_global() pattern (like php-spx)
        zend_string *server_name = zend_string_init("_SERVER", sizeof("_SERVER")-1, 0);
        zend_is_auto_global(server_name);
        server_zv = zend_hash_find(&EG(symbol_table), server_name);
        zend_string_release(server_name);
    }
    
    if (server_zv && Z_TYPE_P(server_zv) == IS_ARRAY) {
        // For Symfony and frameworks with front controllers, prefer PATH_INFO over REQUEST_URI
        // Store both: uri (cleaned route) and request_uri (original full path)
        zval *path_info_zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "PATH_INFO", sizeof("PATH_INFO")-1);
        zval *request_uri_zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "REQUEST_URI", sizeof("REQUEST_URI")-1);
        
        // Capture original REQUEST_URI (without query string) for storage
        if (request_uri_zv && Z_TYPE_P(request_uri_zv) == IS_STRING && Z_STRLEN_P(request_uri_zv) > 0) {
            const char *raw_uri = Z_STRVAL_P(request_uri_zv);
            char *query_start = strchr(raw_uri, '?');
            size_t request_uri_len = query_start ? (query_start - raw_uri) : Z_STRLEN_P(request_uri_zv);
            // Store original REQUEST_URI (we'll include it in JSON)
            static char request_uri_buf[2048];
            if (request_uri_len < sizeof(request_uri_buf) - 1) {
                memcpy(request_uri_buf, raw_uri, request_uri_len);
                request_uri_buf[request_uri_len] = '\0';
                request_uri_original = request_uri_buf;
            } else {
                request_uri_original = raw_uri;
            }
        }
        
        // Prefer PATH_INFO for cleaned route
        if (path_info_zv && Z_TYPE_P(path_info_zv) == IS_STRING && Z_STRLEN_P(path_info_zv) > 0) {
            uri = Z_STRVAL_P(path_info_zv);
            debug_log("[serialize_http_request_json] Using PATH_INFO: %s (original REQUEST_URI: %s)", uri, request_uri_original ? request_uri_original : "N/A");
        } else if (request_uri_zv && Z_TYPE_P(request_uri_zv) == IS_STRING && Z_STRLEN_P(request_uri_zv) > 0) {
            // Fallback: try to clean REQUEST_URI by removing /index.php prefix
            const char *raw_uri = Z_STRVAL_P(request_uri_zv);
            char *query_start = strchr(raw_uri, '?');
            size_t uri_len = query_start ? (query_start - raw_uri) : Z_STRLEN_P(request_uri_zv);
            // Remove /index.php prefix if present
            if (uri_len >= 10 && strncmp(raw_uri, "/index.php", 10) == 0) {
                if (uri_len == 10) {
                    uri = "/";
                } else {
                    // Create cleaned URI (skip /index.php)
                    static char cleaned_uri[2048];
                    size_t cleaned_len = uri_len - 10;
                    if (cleaned_len < sizeof(cleaned_uri)) {
                        memcpy(cleaned_uri, raw_uri + 10, cleaned_len);
                        cleaned_uri[cleaned_len] = '\0';
                        uri = cleaned_uri;
                    }
                }
                debug_log("[serialize_http_request_json] Cleaned REQUEST_URI (removed /index.php): %s (original: %s)", uri, request_uri_original ? request_uri_original : "N/A");
            }
        }
        
        zval *zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "REQUEST_SCHEME", sizeof("REQUEST_SCHEME")-1);
        if (zv && Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0) {
            scheme = Z_STRVAL_P(zv);
        } else {
            zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_X_FORWARDED_PROTO", sizeof("HTTP_X_FORWARDED_PROTO")-1);
            if (zv && Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0) {
                scheme = Z_STRVAL_P(zv);
            }
        }
        
        zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_HOST", sizeof("HTTP_HOST")-1);
        if (!zv || Z_TYPE_P(zv) != IS_STRING) {
            zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "SERVER_NAME", sizeof("SERVER_NAME")-1);
        }
        if (zv && Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0) {
            host = Z_STRVAL_P(zv);
            host_len = Z_STRLEN_P(zv);
        }
    }
    
    // Allocate buffer (estimate: increased for extended fields)
    size_t method_len = strlen(method);
    size_t uri_len = strlen(uri);
    size_t query_len = strlen(query);
    size_t request_uri_len = request_uri_original ? strlen(request_uri_original) : 0;
    size_t buf_size = 6000 + method_len + uri_len + query_len + (host_len ? host_len : 0) + request_uri_len;
    char *result = malloc(buf_size);
    if (!result) {
        return strdup("{\"scheme\":\"http\",\"method\":\"GET\",\"uri\":\"/\"}");
    }
    
    // Build JSON with all fields
    int pos = snprintf(result, buf_size, 
        "{\"scheme\":\"%s\",\"method\":\"%s\",\"uri\":\"%s\"", 
        scheme, method, uri);
    
    // Add original request_uri if available (for ClickHouse storage)
    if (request_uri_original && strlen(request_uri_original) > 0) {
        if (pos + request_uri_len + 50 < buf_size) {
            pos += snprintf(result + pos, buf_size - pos, ",\"request_uri\":\"%s\"", request_uri_original);
        }
    }
    
    if (host && host_len > 0 && host_len < 200) {
        if (pos + host_len + 50 < buf_size) {
            pos += snprintf(result + pos, buf_size - pos, ",\"host\":\"%.*s\"", (int)host_len, host);
        }
    }
    
    if (query && *query != '\0' && query_len < 500) {
        if (pos + query_len + 50 < buf_size) {
            pos += snprintf(result + pos, buf_size - pos, ",\"query_string\":\"%s\"", query);
        }
    }
    
    // Try to get IP from $_SERVER if available
    if (server_zv && Z_TYPE_P(server_zv) == IS_ARRAY) {
        zval *zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "REMOTE_ADDR", sizeof("REMOTE_ADDR")-1);
        if (!zv || Z_TYPE_P(zv) != IS_STRING) {
            zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_X_FORWARDED_FOR", sizeof("HTTP_X_FORWARDED_FOR")-1);
        }
        if (zv && Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0 && Z_STRLEN_P(zv) < 50) {
            if (pos + 100 < buf_size) {
                pos += snprintf(result + pos, buf_size - pos, ",\"ip\":\"%.*s\"", (int)Z_STRLEN_P(zv), Z_STRVAL_P(zv));
            }
        }
    }
    
    // Add extended fields from $_SERVER
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_USER_AGENT", sizeof("HTTP_USER_AGENT")-1, "user_agent", 500, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_REFERER", sizeof("HTTP_REFERER")-1, "referer", 1000, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_ACCEPT_LANGUAGE", sizeof("HTTP_ACCEPT_LANGUAGE")-1, "accept_language", 200, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_CONTENT_TYPE", sizeof("HTTP_CONTENT_TYPE")-1, "content_type", 200, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_ACCEPT", sizeof("HTTP_ACCEPT")-1, "accept", 500, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_ACCEPT_ENCODING", sizeof("HTTP_ACCEPT_ENCODING")-1, "accept_encoding", 200, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "SERVER_PORT", sizeof("SERVER_PORT")-1, "port", 10, 1);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_X_REQUEST_ID", sizeof("HTTP_X_REQUEST_ID")-1, "request_id", 100, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_X_TRACE_ID", sizeof("HTTP_X_TRACE_ID")-1, "trace_id", 100, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_ORIGIN", sizeof("HTTP_ORIGIN")-1, "origin", 500, 0);
    pos = add_server_field_json(&result, &buf_size, pos, server_zv, "HTTP_CONNECTION", sizeof("HTTP_CONNECTION")-1, "connection", 50, 0);
    
    // Handle HTTP_COOKIE - size only for security
    if (server_zv && Z_TYPE_P(server_zv) == IS_ARRAY) {
        zval *cookie_zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_COOKIE", sizeof("HTTP_COOKIE")-1);
        if (cookie_zv && Z_TYPE_P(cookie_zv) == IS_STRING && Z_STRLEN_P(cookie_zv) > 0) {
            size_t cookie_size = Z_STRLEN_P(cookie_zv);
            size_t required_space = 20; // ,"cookie_size":1234
            if (pos + required_space >= buf_size) {
                size_t new_size = buf_size * 2 + required_space;
                char *new_result = realloc(result, new_size);
                if (new_result) {
                    result = new_result;
                    buf_size = new_size;
                }
            }
            if (pos + required_space < buf_size) {
                pos += snprintf(result + pos, buf_size - pos, ",\"cookie_size\":%zu", cookie_size);
            }
        }
        
        // Handle HTTP_AUTHORIZATION - presence only for security
        zval *auth_zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_AUTHORIZATION", sizeof("HTTP_AUTHORIZATION")-1);
        if (auth_zv && Z_TYPE_P(auth_zv) == IS_STRING && Z_STRLEN_P(auth_zv) > 0) {
            size_t required_space = 30; // ,"authorization_present":true
            if (pos + required_space >= buf_size) {
                size_t new_size = buf_size * 2 + required_space;
                char *new_result = realloc(result, new_size);
                if (new_result) {
                    result = new_result;
                    buf_size = new_size;
                }
            }
            if (pos + required_space < buf_size) {
                pos += snprintf(result + pos, buf_size - pos, ",\"authorization_present\":true");
            }
        }
    }
    
    // Calculate request size (headers + body + files)
    size_t request_size = 0;
    size_t body_size = (size_t)SG(request_info).content_length; // Request body size from Content-Length header
    size_t query_size = query_len; // Query string size
    size_t file_size = 0;
    size_t header_size = 0;
    
    // Calculate file sizes from $_FILES
    zend_string *files_name = zend_string_init("_FILES", sizeof("_FILES")-1, 0);
    zend_is_auto_global(files_name);
    zval *files_zv = zend_hash_find(&EG(symbol_table), files_name);
    zend_string_release(files_name);
    
    if (files_zv && Z_TYPE_P(files_zv) == IS_ARRAY) {
        // Iterate through $_FILES array
        zval *file_entry;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(files_zv), file_entry) {
            if (Z_TYPE_P(file_entry) == IS_ARRAY) {
                // Each file entry has a 'size' field
                zval *size_zv = zend_hash_str_find(Z_ARRVAL_P(file_entry), "size", sizeof("size")-1);
                if (size_zv) {
                    if (Z_TYPE_P(size_zv) == IS_LONG) {
                        file_size += (size_t)Z_LVAL_P(size_zv);
                    } else if (Z_TYPE_P(size_zv) == IS_DOUBLE) {
                        file_size += (size_t)Z_DVAL_P(size_zv);
                    }
                }
            }
        } ZEND_HASH_FOREACH_END();
    }
    
    // Estimate header size from $_SERVER HTTP_* entries
    if (server_zv && Z_TYPE_P(server_zv) == IS_ARRAY) {
        // Add request line (method + URI + protocol): "GET /path HTTP/1.1\r\n"
        header_size += method_len + uri_len + 15; // " GET  HTTP/1.1\r\n" = 15 bytes
        
        // Iterate through $_SERVER entries to find HTTP_* headers
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(server_zv), key, value) {
            if (key && ZSTR_LEN(key) > 5) {
                const char *key_str = ZSTR_VAL(key);
                
                // Check if it's an HTTP header (starts with "HTTP_")
                if (strncmp(key_str, "HTTP_", 5) == 0 && value && Z_TYPE_P(value) == IS_STRING) {
                    // Header format: "Header-Name: value\r\n"
                    // Approximate: key (HTTP_HEADER_NAME) + value + ": \r\n" overhead
                    // Convert HTTP_HEADER_NAME to Header-Name format (approximate size)
                    size_t header_name_len = ZSTR_LEN(key) - 5; // Remove "HTTP_" prefix
                    header_size += header_name_len + Z_STRLEN_P(value) + 4; // ": \r\n" = 4 bytes
                }
            }
        } ZEND_HASH_FOREACH_END();
    } else {
        // Fallback: estimate header size (request line + common headers)
        header_size += method_len + uri_len + 15; // Request line
        header_size += 100; // Estimate for common headers (Host, User-Agent, etc.)
    }
    
    // Calculate total request size
    request_size = body_size + query_size + file_size + header_size;
    
    // Always add request_size field to JSON (even if 0, for debugging)
    // Check if we have enough buffer space, if not, realloc
    size_t size_str_len = 200; // Enough for size and breakdown
    if (pos + size_str_len >= buf_size) {
        size_t new_size = buf_size * 2 + size_str_len;
        char *new_result = realloc(result, new_size);
        if (new_result) {
            result = new_result;
            buf_size = new_size;
        }
    }
    if (result && (pos + size_str_len < buf_size)) {
        pos += snprintf(result + pos, buf_size - pos, ",\"request_size\":%zu", request_size);
        // Also add breakdown for debugging
        pos += snprintf(result + pos, buf_size - pos, ",\"request_size_breakdown\":{\"body\":%zu,\"query\":%zu,\"files\":%zu,\"headers\":%zu}",
            body_size, query_size, file_size, header_size);
    }
    
    if (pos < buf_size) {
        snprintf(result + pos, buf_size - pos, "}");
    }
    
    return result;
}

// Helper: Serialize HTTP response details to JSON string
char* serialize_http_response_json(void) {
    smart_string json = {0};
    smart_string_appends(&json, "{");
    
    int first = 1;
    
    // Response status code - check if function exists in symbol table
    zend_function *http_response_code_func = zend_hash_str_find_ptr(EG(function_table), "http_response_code", sizeof("http_response_code") - 1);
    if (http_response_code_func) {
        zval response_code_func, response_code_ret;
        ZVAL_UNDEF(&response_code_func);
        ZVAL_UNDEF(&response_code_ret);
        
        ZVAL_STRING(&response_code_func, "http_response_code");
        
        zend_fcall_info fci;
        zend_fcall_info_cache fcc;
        if (zend_fcall_info_init(&response_code_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
            fci.size = sizeof(fci);
            ZVAL_UNDEF(&fci.function_name);
            fci.object = NULL;
            fci.param_count = 0;
            fci.params = NULL;
            fci.retval = &response_code_ret;
            
            if (zend_call_function(&fci, &fcc) == SUCCESS) {
                if (Z_TYPE(response_code_ret) == IS_LONG) {
                    if (!first) smart_string_appends(&json, ",");
                    long status_code = Z_LVAL(response_code_ret);
                    char status_str[32];
                    snprintf(status_str, sizeof(status_str), "%ld", status_code);
                    smart_string_appends(&json, "\"status_code\":");
                    smart_string_appends(&json, status_str);
                    first = 0;
                    
                    // Log HTTP response code
                    char fields[128];
                    snprintf(fields, sizeof(fields), "{\"status_code\":%ld}", status_code);
                    log_info("HTTP response code captured", fields);
                }
                zval_dtor(&response_code_ret);
            }
        }
        zval_dtor(&response_code_func);
    }
    
    // Response headers from headers_list() - check if function exists
    size_t header_size = 0;
    size_t body_size_from_header = 0;
    
    zend_function *headers_list_func = zend_hash_str_find_ptr(EG(function_table), "headers_list", sizeof("headers_list") - 1);
    if (headers_list_func) {
        zval headers_list_func_zv, headers_list_ret;
        ZVAL_UNDEF(&headers_list_func_zv);
        ZVAL_UNDEF(&headers_list_ret);
        
        ZVAL_STRING(&headers_list_func_zv, "headers_list");
        
        zend_fcall_info fci;
        zend_fcall_info_cache fcc;
        if (zend_fcall_info_init(&headers_list_func_zv, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
            fci.size = sizeof(fci);
            ZVAL_UNDEF(&fci.function_name);
            fci.object = NULL;
            fci.param_count = 0;
            fci.params = NULL;
            fci.retval = &headers_list_ret;
            
            if (zend_call_function(&fci, &fcc) == SUCCESS) {
                if (Z_TYPE(headers_list_ret) == IS_ARRAY) {
                    if (!first) smart_string_appends(&json, ",");
                    smart_string_appends(&json, "\"headers\":{");
                    int header_first = 1;
                    
                    zval *header;
                    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(headers_list_ret), header) {
                        if (Z_TYPE_P(header) == IS_STRING) {
                            // Parse "Header-Name: value" format
                            const char *header_str = Z_STRVAL_P(header);
                            size_t header_len = Z_STRLEN_P(header);
                            
                            // Calculate header size (header string + ": \r\n" overhead)
                            header_size += header_len + 4; // ": \r\n" = 4 bytes
                            
                            // Check for Content-Length header
                            if (strncasecmp(header_str, "Content-Length:", 15) == 0) {
                                const char *value_start = header_str + 15;
                                while (*value_start == ' ' || *value_start == '\t') value_start++;
                                body_size_from_header = (size_t)strtoul(value_start, NULL, 10);
                            }
                            
                            const char *colon = memchr(header_str, ':', header_len);
                            
                            if (colon) {
                                if (!header_first) smart_string_appends(&json, ",");
                                
                                size_t name_len = colon - header_str;
                                char *header_name = estrndup(header_str, name_len);
                                // Trim whitespace from name
                                while (name_len > 0 && (header_name[name_len-1] == ' ' || header_name[name_len-1] == '\t')) {
                                    name_len--;
                                    header_name[name_len] = '\0';
                                }
                                
                                const char *value_start = colon + 1;
                                while (*value_start == ' ' || *value_start == '\t') value_start++;
                                size_t value_len = header_len - (value_start - header_str);
                                
                                smart_string_appends(&json, "\"");
                                json_escape_string(&json, header_name, strlen(header_name));
                                smart_string_appends(&json, "\":\"");
                                json_escape_string(&json, value_start, value_len);
                                smart_string_appends(&json, "\"");
                                
                                efree(header_name);
                                header_first = 0;
                            }
                        }
                    } ZEND_HASH_FOREACH_END();
                    
                    smart_string_appends(&json, "}");
                    first = 0;
                }
                zval_dtor(&headers_list_ret);
            }
        }
        zval_dtor(&headers_list_func_zv);
    }
    
    // Calculate response size (body + headers)
    size_t response_size = 0;
    size_t body_size = 0;
    
    // Get response body size using ob_get_length()
    zend_function *ob_get_length_func = zend_hash_str_find_ptr(EG(function_table), "ob_get_length", sizeof("ob_get_length") - 1);
    if (ob_get_length_func) {
        zval ob_get_length_func_zv, ob_get_length_ret;
        ZVAL_UNDEF(&ob_get_length_func_zv);
        ZVAL_UNDEF(&ob_get_length_ret);
        
        ZVAL_STRING(&ob_get_length_func_zv, "ob_get_length");
        
        zend_fcall_info fci;
        zend_fcall_info_cache fcc;
        if (zend_fcall_info_init(&ob_get_length_func_zv, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
            fci.size = sizeof(fci);
            ZVAL_UNDEF(&fci.function_name);
            fci.object = NULL;
            fci.param_count = 0;
            fci.params = NULL;
            fci.retval = &ob_get_length_ret;
            
            if (zend_call_function(&fci, &fcc) == SUCCESS) {
                if (Z_TYPE(ob_get_length_ret) == IS_LONG) {
                    body_size = (size_t)Z_LVAL(ob_get_length_ret);
                } else if (Z_TYPE(ob_get_length_ret) == IS_FALSE) {
                    // No output buffer active - will use Content-Length from headers if available
                    body_size = 0;
                }
                zval_dtor(&ob_get_length_ret);
            }
        }
        zval_dtor(&ob_get_length_func_zv);
    }
    
    // If no output buffer, use Content-Length from headers (already extracted above)
    if (body_size == 0) {
        body_size = body_size_from_header;
    }
    
    // Add status line size (e.g., "HTTP/1.1 200 OK\r\n") - approximately 15 bytes
    header_size += 15;
    
    // Calculate total response size
    response_size = body_size + header_size;
    
    // Always add response_size field to JSON (even if 0, for debugging)
    if (!first) smart_string_appends(&json, ",");
    char size_str[64];
    snprintf(size_str, sizeof(size_str), "%zu", response_size);
    smart_string_appends(&json, "\"response_size\":");
    smart_string_appends(&json, size_str);
    first = 0;
    
    // Also add breakdown for debugging
    if (!first) smart_string_appends(&json, ",");
    char body_size_str[64], header_size_str[64];
    snprintf(body_size_str, sizeof(body_size_str), "%zu", body_size);
    snprintf(header_size_str, sizeof(header_size_str), "%zu", header_size);
    smart_string_appends(&json, "\"response_size_breakdown\":{\"body\":");
    smart_string_appends(&json, body_size_str);
    smart_string_appends(&json, ",\"headers\":");
    smart_string_appends(&json, header_size_str);
    smart_string_appends(&json, "}");
    first = 0;
    
    // Optionally add response body content if ob_get_contents is available
    // Note: This is disabled by default - can be enabled via INI setting if needed
    // For now, we'll try to get it if output buffering is active
    zend_function *ob_get_contents_func = zend_hash_str_find_ptr(EG(function_table), "ob_get_contents", sizeof("ob_get_contents") - 1);
    if (ob_get_contents_func && body_size > 0 && body_size < 10240) { // Only for small responses (< 10KB)
        zval ob_get_contents_func_zv, ob_get_contents_ret;
        ZVAL_UNDEF(&ob_get_contents_func_zv);
        ZVAL_UNDEF(&ob_get_contents_ret);
        
        ZVAL_STRING(&ob_get_contents_func_zv, "ob_get_contents");
        
        zend_fcall_info fci;
        zend_fcall_info_cache fcc;
        if (zend_fcall_info_init(&ob_get_contents_func_zv, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
            fci.size = sizeof(fci);
            ZVAL_UNDEF(&fci.function_name);
            fci.object = NULL;
            fci.param_count = 0;
            fci.params = NULL;
            fci.retval = &ob_get_contents_ret;
            
            if (zend_call_function(&fci, &fcc) == SUCCESS) {
                if (Z_TYPE(ob_get_contents_ret) == IS_STRING && Z_STRLEN(ob_get_contents_ret) > 0) {
                    if (!first) smart_string_appends(&json, ",");
                    smart_string_appends(&json, "\"body\":\"");
                    json_escape_string(&json, Z_STRVAL(ob_get_contents_ret), Z_STRLEN(ob_get_contents_ret));
                    smart_string_appends(&json, "\"");
                    first = 0;
                }
                zval_dtor(&ob_get_contents_ret);
            }
        }
        zval_dtor(&ob_get_contents_func_zv);
    }
    
    smart_string_appends(&json, "}");
    smart_string_0(&json);
    
    // Convert to malloc'd string
    char *result = NULL;
    if (json.c && json.len > 0) {
        result = malloc(json.len + 1);
        if (result) {
            memcpy(result, json.c, json.len);
            result[json.len] = '\0';
        }
    }
    smart_string_free(&json);
    
    return result;
}

// Conditional debug logging - only writes when opa.debug_log INI setting is enabled
// Writes to /tmp/opa_debug.log or /app/logs/opa_debug.log
// Note: Cannot use php_error_docref() as it causes fatal errors in RSHUTDOWN with Symfony's error handler
void debug_log(const char *msg, ...) {
    // Early return if debug logging is disabled
    if (!OPA_G(debug_log_enabled)) {
        return;
    }
    
    char buffer[2048];
    va_list args;
    va_start(args, msg);
    vsnprintf(buffer, sizeof(buffer), msg, args);
    va_end(args);
    
    // Try /tmp/ first (always exists), then /app/logs/ (if available)
    const char *log_paths[] = {"/tmp/opa_debug.log", "/app/logs/opa_debug.log", NULL};
    FILE *log = NULL;
    
    for (int i = 0; log_paths[i] != NULL; i++) {
        log = fopen(log_paths[i], "a");
        if (log) {
            // Get timestamp 
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char timestamp[64];
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
            
            fprintf(log, "[%s] [%s:%d:%s] %s\n", timestamp, __FILE__, __LINE__, __FUNCTION__, buffer);
            fflush(log);
            fclose(log);
            break;
        }
    }
}

// Structured JSON logging - always writes to stderr (not conditional)
// Also sends to agent if log tracking is enabled
// Format: {"timestamp":"...","level":"ERROR|WARN|INFO","message":"...","error":"...","fields":{...}}
void log_structured(const char *level, const char *message, const char *error, const char *fields_json) {
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
    
    fprintf(stderr, "{\"timestamp\":\"%s\",\"level\":\"%s\",\"message\":\"%s\"", 
            timestamp, level, message ? message : "");
    
    if (error && strlen(error) > 0) {
        // Escape JSON string
        fprintf(stderr, ",\"error\":\"");
        for (const char *p = error; *p; p++) {
            if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') {
                fprintf(stderr, "\\%c", *p == '\n' ? 'n' : *p == '\r' ? 'r' : *p == '\t' ? 't' : *p);
            } else if (*p >= 0 && *p < 32) {
                fprintf(stderr, "\\u%04x", (unsigned char)*p);
            } else {
                fputc(*p, stderr);
            }
        }
        fprintf(stderr, "\"");
    }
    
    if (fields_json && strlen(fields_json) > 0) {
        fprintf(stderr, ",\"fields\":%s", fields_json);
    }
    
    fprintf(stderr, "}\n");
    fflush(stderr);
    
    // Also send to agent if log tracking is enabled
    // Build a combined message with error if present
    if (OPA_G(enabled) && OPA_G(track_logs) && message) {
        // Make a safe copy of the message to avoid memory corruption issues
        // This ensures the message is valid throughout the send_log_to_agent call
        size_t msg_len = strlen(message);
        char *safe_message = emalloc(msg_len + 1);
        if (safe_message) {
            memcpy(safe_message, message, msg_len);
            safe_message[msg_len] = '\0';
            
            char *combined_message = NULL;
            if (error && strlen(error) > 0) {
                size_t combined_len = msg_len + strlen(error) + 10;
                combined_message = emalloc(combined_len);
                if (combined_message) {
                    snprintf(combined_message, combined_len, "%s: %s", safe_message, error);
                }
            } else {
                combined_message = safe_message;
                safe_message = NULL; // Don't free it, it's being used
            }
            
            if (combined_message) {
                // Send log to agent (file and line are not available in log_structured context)
                send_log_to_agent(level, combined_message, NULL, 0);
                
                // Free the combined message (which might be safe_message or a new allocation)
                efree(combined_message);
            }
            
            // Free safe_message if it wasn't used as combined_message
            if (safe_message) {
                efree(safe_message);
            }
        }
    }
}

// Log error with structured JSON format
void log_error(const char *message, const char *error, const char *fields_json) {
    log_structured("ERROR", message, error, fields_json);
}

// Log warning with structured JSON format
void log_warn(const char *message, const char *fields_json) {
    log_structured("WARN", message, NULL, fields_json);
}

// Log info with structured JSON format
void log_info(const char *message, const char *fields_json) {
    log_structured("INFO", message, NULL, fields_json);
}

// Helper to create fields JSON from key-value pairs
// Usage: log_error("message", "error", "{\"key\":\"value\"}")
// For simple cases, can use: log_error("message", "error", NULL)

// Record SQL query - moved to call_node.c

// Functions moved to separate files:
// - json_escape_string, serialize_* -> serialize.c
// - produce_span_json, free_span_context -> span.c
// - send_message_direct -> transport.c
// - record_sql_query -> call_node.c

// All serialize functions moved to serialize.c
// free_span_context moved to span.c
// send_message_direct moved to transport.c

// Thread-based sender and sampler removed - using direct synchronous sending instead

// Get or create active spans hash table (simplified - no thread-local storage)
HashTable* get_active_spans() {
    if (!active_spans) {
        pthread_mutex_lock(&active_spans_mutex);
        if (!active_spans) {
            // Use malloc instead of emalloc to prevent PHP from automatically destroying
            // the hash table during MSHUTDOWN when zvals are invalid
            active_spans = malloc(sizeof(HashTable));
            // Use NULL destructor to prevent PHP from accessing zvals during destruction
            // We'll manually free spans in RSHUTDOWN before MSHUTDOWN
            zend_hash_init(active_spans, 8, NULL, NULL, 0);
        }
        pthread_mutex_unlock(&active_spans_mutex);
    }
    
    return active_spans;
}

// Check if function is a PDO method
int is_pdo_method(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // Check if it's a method call
    if (execute_data->func->common.scope) {
        const char *class_name = ZSTR_VAL(execute_data->func->common.scope->name);
        debug_log("[is_pdo_method] Checking class: %s", class_name);
        if (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0) {
            if (execute_data->func->common.function_name) {
                const char *method_name = ZSTR_VAL(execute_data->func->common.function_name);
                debug_log("[is_pdo_method] Checking method: %s::%s", class_name, method_name);
                if (strcmp(method_name, "prepare") == 0 ||
                    strcmp(method_name, "query") == 0 ||
                    strcmp(method_name, "exec") == 0 ||
                    strcmp(method_name, "execute") == 0) {
                    debug_log("[is_pdo_method] PDO method detected: %s::%s", class_name, method_name);
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

// PHP 8.4: Store curl class entry pointers for reliable detection (stable across PHP 8.x)
static zend_class_entry *curl_ce = NULL;
static zend_class_entry *curl_multi_ce = NULL;
static zend_class_entry *curl_share_ce = NULL;

// Helper to detect curl calls by checking if first argument is a CurlHandle object
// This is stable across PHP 8.x regardless of function name/type/handler issues
static int is_curl_call(zend_execute_data *execute_data, zval **curl_handle_out) {
    debug_log("[is_curl_call] ENTRY: execute_data=%p", execute_data);
    if (!execute_data) {
        debug_log("[is_curl_call] execute_data=NULL");
        return 0;
    }

    zend_function *func = execute_data->func;
    if (!func) {
        debug_log("[is_curl_call] func=NULL");
        return 0;
    }

    uint32_t num_args = ZEND_CALL_NUM_ARGS(execute_data);
    debug_log("[is_curl_call] num_args=%u", num_args);
    if (num_args < 1) {
        return 0;
    }

    zval *arg1 = ZEND_CALL_ARG(execute_data, 1);
    if (!arg1) {
        debug_log("[is_curl_call] arg1=NULL");
        return 0;
    }

    debug_log("[is_curl_call] arg1 type=%d", Z_TYPE_P(arg1));

    if (Z_TYPE_P(arg1) == IS_OBJECT) {
        zend_class_entry *ce = Z_OBJCE_P(arg1);
        if (!ce) {
            debug_log("[is_curl_call] IS_OBJECT but ce=NULL");
            return 0;
        }

        const char *name = ce->name ? ZSTR_VAL(ce->name) : "<no name>";
        debug_log("[is_curl_call] object ce=%p name=%s", ce, name);

        // 1) pointer match if we ever resolved them in RINIT/MINIT
        if ((curl_ce && ce == curl_ce) ||
            (curl_multi_ce && ce == curl_multi_ce) ||
            (curl_share_ce && ce == curl_share_ce)) {
            debug_log("[is_curl_call] matched by class entry pointer");
            if (curl_handle_out) *curl_handle_out = arg1;
            return 1;
        }

        // 2) name-based match (robust on 8.0+)
        if (ce->name) {
            size_t len = ZSTR_LEN(ce->name);
            const char *cn = ZSTR_VAL(ce->name);

            if ((len == sizeof("CurlHandle")-1      && memcmp(cn, "CurlHandle",      sizeof("CurlHandle")-1)      == 0) ||
                (len == sizeof("CurlMultiHandle")-1 && memcmp(cn, "CurlMultiHandle", sizeof("CurlMultiHandle")-1) == 0) ||
                (len == sizeof("CurlShareHandle")-1 && memcmp(cn, "CurlShareHandle", sizeof("CurlShareHandle")-1) == 0)) {

                debug_log("[is_curl_call] matched by class name=%s", cn);
                if (curl_handle_out) *curl_handle_out = arg1;
                return 1;
            }
        }
    }

    if (Z_TYPE_P(arg1) == IS_RESOURCE) {
        debug_log("[is_curl_call] resource handle, treating as curl for <8.0");
        if (curl_handle_out) *curl_handle_out = arg1;
        return 1;
    }

    debug_log("[is_curl_call] NOT curl (arg1 type %d)", Z_TYPE_P(arg1));
    return 0;
}

// Check if function is a cURL function
int is_curl_function(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // PHP 8.4: Don't rely on func->common.function_name (can be NULL)
    // Instead, use function pointer comparison and CurlHandle class detection
    // In PHP 8.4, internal functions can have type 1 (ZEND_INTERNAL_FUNCTION) or type 4
    debug_log("[is_curl_function] Called: func=%p, func->type=%d, ZEND_INTERNAL_FUNCTION=%d", 
        execute_data->func, execute_data->func->type, ZEND_INTERNAL_FUNCTION);
    
    if (execute_data->func->type != ZEND_INTERNAL_FUNCTION && execute_data->func->type != 4) {
        debug_log("[is_curl_function] Not internal function (type=%d), returning 0", execute_data->func->type);
        return 0;
    }
    
    // Method 1: Check if this is a CurlHandle method by class name
    // In PHP 8.4, curl functions might be called as methods on CurlHandle objects
    debug_log("[is_curl_function] Checking This: Z_TYPE=%d, IS_OBJECT=%d", 
        Z_TYPE(execute_data->This), IS_OBJECT);
    if (Z_TYPE(execute_data->This) == IS_OBJECT) {
        zend_class_entry *ce = Z_OBJCE(execute_data->This);
        if (ce && ce->name) {
            debug_log("[is_curl_function] This is object, class name: %s", ZSTR_VAL(ce->name));
            if (ZSTR_LEN(ce->name) == sizeof("CurlHandle")-1 &&
                memcmp(ZSTR_VAL(ce->name), "CurlHandle", sizeof("CurlHandle")-1) == 0) {
                // This is a CurlHandle method (exec, setopt, etc.)
                debug_log("[is_curl_function] Matched CurlHandle method");
                return 1;
            }
        }
    }
    
    // Method 2: Look up curl functions in function table at runtime and compare function pointers
    // This works even if function structures are copied in PHP 8.4
    HashTable *func_table = EG(function_table);
    if (func_table) {
        const char *curl_funcs[] = {"curl_exec", "curl_setopt", "curl_init", "curl_close", NULL};
        for (int i = 0; curl_funcs[i] != NULL; i++) {
            zend_function *found_func = zend_hash_str_find_ptr(func_table, curl_funcs[i], strlen(curl_funcs[i]));
            if (found_func && found_func == execute_data->func) {
                debug_log("[is_curl_function] Matched %s via runtime function table lookup", curl_funcs[i]);
                return 1;
            }
        }
    }
    
    // Use argument-based detection (stable across PHP 8.x)
    zval *curl_handle = NULL;
    if (is_curl_call(execute_data, &curl_handle)) {
        return 1;
    }
    
    // Method 3: Fallback - check function_name if available (for older PHP versions)
    if (execute_data->func->common.function_name) {
        const char *function_name = ZSTR_VAL(execute_data->func->common.function_name);
        if (strncmp(function_name, "curl_", 5) == 0) {
            return 1;
        }
    }
    
    return 0;
}

// Get curl function type: 1=curl_exec, 2=curl_setopt, 3=curl_multi_exec, 4=curl_init, 5=curl_close, 0=other
// Now uses heuristics based on argument count and return type since we can't reliably identify by function name
int get_curl_function_type(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // Try to get function name if available (fallback for older PHP versions)
    // Add defensive check for common structure
    if (execute_data->func->common.function_name) {
        zend_string *func_name_str = execute_data->func->common.function_name;
        if (func_name_str && ZSTR_VAL(func_name_str)) {
            const char *function_name = ZSTR_VAL(func_name_str);
            if (strcmp(function_name, "curl_exec") == 0) {
                return 1;
            } else if (strcmp(function_name, "curl_setopt") == 0 || strcmp(function_name, "curl_setopt_array") == 0) {
                return 2;
            } else if (strcmp(function_name, "curl_multi_exec") == 0) {
                return 3;
            } else if (strcmp(function_name, "curl_init") == 0) {
                return 4;
            } else if (strcmp(function_name, "curl_close") == 0) {
                return 5;
            }
        }
    }
    
    // TEMPORARILY DISABLED: is_curl_call causes segfault
    // Heuristic: If it's a curl call, use argument count to determine type
    // curl_exec($ch) takes 1 argument and returns string/false
    // curl_setopt($ch, $option, $value) takes 3 arguments
    // curl_init($url) takes 0-1 arguments
    // curl_close($ch) takes 1 argument
    // For now, rely only on function name detection above
    /*
    zval *curl_handle = NULL;
    if (is_curl_call(execute_data, &curl_handle)) {
        int num_args = ZEND_CALL_NUM_ARGS(execute_data);
        // curl_exec typically has 1 arg (the handle)
        // curl_setopt has 3 args
        // curl_close has 1 arg
        // curl_init has 0-1 args
        if (num_args == 1) {
            // Could be curl_exec or curl_close - assume curl_exec for now
            // We can refine this by checking if it's called after curl_setopt
            return 1; // Assume curl_exec
        } else if (num_args == 3) {
            return 2; // curl_setopt
        } else if (num_args == 0 || num_args == 1) {
            return 4; // curl_init
        }
    }
    */
    
    return 0;
}

// Check if function is an APCu function
int is_apcu_function(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // Check if it's an internal function (APCu functions are internal)
    if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        if (execute_data->func->common.function_name) {
            const char *function_name = ZSTR_VAL(execute_data->func->common.function_name);
            // Check for APCu functions we want to monitor
            if (strcmp(function_name, "apcu_fetch") == 0 ||
                strcmp(function_name, "apcu_store") == 0 ||
                strcmp(function_name, "apcu_delete") == 0 ||
                strcmp(function_name, "apcu_clear_cache") == 0 ||
                strcmp(function_name, "apcu_exists") == 0 ||
                strcmp(function_name, "apc_fetch") == 0 ||
                strcmp(function_name, "apc_store") == 0 ||
                strcmp(function_name, "apc_delete") == 0) {
                return 1;
            }
        }
    }
    
    return 0;
}

// Check if method is a Symfony Cache method
int is_symfony_cache_method(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // Check if it's a method call
    if (execute_data->func->common.scope) {
        const char *class_name = ZSTR_VAL(execute_data->func->common.scope->name);
        // Check for Symfony Cache classes
        if (strstr(class_name, "Symfony\\Component\\Cache") != NULL ||
            strstr(class_name, "Symfony\\Contracts\\Cache") != NULL) {
            if (execute_data->func->common.function_name) {
                const char *method_name = ZSTR_VAL(execute_data->func->common.function_name);
                // Check for cache methods we want to monitor
                if (strcmp(method_name, "get") == 0 ||
                    strcmp(method_name, "set") == 0 ||
                    strcmp(method_name, "delete") == 0 ||
                    strcmp(method_name, "has") == 0 ||
                    strcmp(method_name, "clear") == 0 ||
                    strcmp(method_name, "getItem") == 0 ||
                    strcmp(method_name, "save") == 0 ||
                    strcmp(method_name, "deleteItem") == 0) {
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

// Check if method is a Redis method
int is_redis_method(zend_execute_data *execute_data) {
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // Check if it's a method call
    if (execute_data->func->common.scope) {
        const char *class_name = ZSTR_VAL(execute_data->func->common.scope->name);
        // Check for Redis classes
        if (strcmp(class_name, "Redis") == 0 ||
            strcmp(class_name, "RedisCluster") == 0 ||
            strstr(class_name, "Predis\\Client") != NULL) {
            if (execute_data->func->common.function_name) {
                const char *method_name = ZSTR_VAL(execute_data->func->common.function_name);
                // Check for Redis methods we want to monitor (common operations)
                if (strcmp(method_name, "get") == 0 ||
                    strcmp(method_name, "set") == 0 ||
                    strcmp(method_name, "del") == 0 ||
                    strcmp(method_name, "delete") == 0 ||
                    strcmp(method_name, "exists") == 0 ||
                    strcmp(method_name, "hget") == 0 ||
                    strcmp(method_name, "hset") == 0 ||
                    strcmp(method_name, "hgetall") == 0 ||
                    strcmp(method_name, "lpush") == 0 ||
                    strcmp(method_name, "rpop") == 0 ||
                    strcmp(method_name, "llen") == 0 ||
                    strcmp(method_name, "sadd") == 0 ||
                    strcmp(method_name, "smembers") == 0 ||
                    strcmp(method_name, "scard") == 0 ||
                    strcmp(method_name, "incr") == 0 ||
                    strcmp(method_name, "decr") == 0 ||
                    strcmp(method_name, "expire") == 0 ||
                    strcmp(method_name, "ttl") == 0 ||
                    strcmp(method_name, "keys") == 0 ||
                    strcmp(method_name, "mget") == 0 ||
                    strcmp(method_name, "mset") == 0) {
                    return 1;
                }
            }
        }
    }
    
    return 0;
}

// Initialize a new collector structure for tracking function calls during a request
// Returns NULL on allocation failure
opa_collector_t* opa_collector_init(void) {
    opa_collector_t *collector = ecalloc(1, sizeof(opa_collector_t));
    if (!collector) {
        return NULL;
    }
    
    collector->magic = OPA_COLLECTOR_MAGIC;
    collector->calls = NULL;
    collector->call_stack_top = NULL;
    collector->call_stack_depth = 0;
    collector->call_depth = 0;
    collector->call_count = 0;
    collector->active = 0;
    collector->global_sql_queries = NULL;
    pthread_mutex_init(&collector->global_sql_mutex, NULL);
    
    return collector;
}

// Activate collector and reset all counters/timers for a new request
// Must be called at the start of each request to begin profiling
void opa_collector_start(opa_collector_t *collector) {
    if (!collector || collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    collector->active = 1;
    collector->start_time = get_time_seconds();
    collector->start_memory = get_memory_usage();
    collector->call_stack_top = NULL;
    collector->call_stack_depth = 0;
    collector->call_depth = 0;
    collector->call_count = 0;
    collector->calls = NULL;
    
    // Initialize global SQL queries array
    pthread_mutex_lock(&collector->global_sql_mutex);
    if (collector->global_sql_queries) {
        zval_ptr_dtor(collector->global_sql_queries);
        efree(collector->global_sql_queries);
    }
    collector->global_sql_queries = ecalloc(1, sizeof(zval));
    if (collector->global_sql_queries) {
        array_init(collector->global_sql_queries);
    }
    pthread_mutex_unlock(&collector->global_sql_mutex);
}

// Deactivate collector and record end time/memory for the request
// Called at the end of request processing
void opa_collector_stop(opa_collector_t *collector) {
    if (!collector || collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    collector->active = 0;
    collector->end_time = get_time_seconds();
    collector->end_memory = get_memory_usage();
}

void opa_collector_free(opa_collector_t *collector) {
    if (!collector) {
        return;
    }
    
    // Free global SQL queries array
    pthread_mutex_lock(&collector->global_sql_mutex);
    if (collector->global_sql_queries) {
        zval_ptr_dtor(collector->global_sql_queries);
        efree(collector->global_sql_queries);
        collector->global_sql_queries = NULL;
    }
    pthread_mutex_unlock(&collector->global_sql_mutex);
    pthread_mutex_destroy(&collector->global_sql_mutex);
    
    // Free all calls
    call_node_t *call = collector->calls;
    while (call) {
        call_node_t *next = call->next;
        if (call->magic == OPA_CALL_NODE_MAGIC) {
            if (call->call_id) efree(call->call_id);
            if (call->function_name) efree(call->function_name);
            if (call->class_name) efree(call->class_name);
            if (call->file) efree(call->file);
            if (call->parent_id) efree(call->parent_id);
            call->magic = 0;
            efree(call);
        }
        call = next;
    }
    
    collector->magic = 0;
    efree(collector);
}

// Call tracking functions ()
char* opa_enter_function(const char *function_name, const char *class_name, const char *file, int line, int function_type) {
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return NULL;
    }
    
    opa_collector_t *collector = global_collector;
    
    // Create call node
    call_node_t *call = emalloc(sizeof(call_node_t));
    if (!call) {
        return NULL;
    }
    memset(call, 0, sizeof(call_node_t));
    call->magic = OPA_CALL_NODE_MAGIC;
    
    call->call_id = generate_id();
    call->start_time = get_time_seconds();
    call->start_cpu_time = get_cpu_time();
    call->start_memory = get_memory_usage();
    call->start_bytes_sent = get_bytes_sent();
    call->start_bytes_received = get_bytes_received();
    
    if (function_name) {
        call->function_name = estrdup(function_name);
    }
    if (class_name) {
        call->class_name = estrdup(class_name);
    }
    if (file) {
        call->file = estrdup(file);
    }
    call->line = line;
    call->function_type = function_type;
    call->depth = collector->call_depth;
    
    // Set parent from call stack (no depth limit)
    debug_log("[enter_function] call_stack_depth=%d, function=%s", 
        collector->call_stack_depth, function_name ? function_name : "NULL");
    if (collector->call_stack_top) {
        call_node_t *parent = collector->call_stack_top;
        if (parent && parent->magic == OPA_CALL_NODE_MAGIC && parent->call_id) {
            call->parent_id = estrdup(parent->call_id);
            debug_log("[enter_function] Set parent_id=%s for call_id=%s (depth=%d)", 
                parent->call_id, call->call_id, collector->call_stack_depth);
        } else {
            debug_log("[enter_function] No valid parent (parent=%p, magic=%08X)", 
                parent, parent ? parent->magic : 0);
        }
    } else {
        debug_log("[enter_function] No parent (depth=%d), root call for %s", 
            collector->call_stack_depth, function_name ? function_name : "NULL");
    }
    
    // Add to list
    call->next = collector->calls;
    collector->calls = call;
    
    // Push to stack (no depth limit - linked list)
    call->stack_next = collector->call_stack_top;
    collector->call_stack_top = call;
    collector->call_stack_depth++;
    debug_log("[enter_function] Pushed to stack: depth=%d, function=%s, call_id=%s", 
        collector->call_stack_depth, function_name ? function_name : "NULL", call->call_id);
    
    collector->call_depth++;
    collector->call_count++;
    
    // Return copy of ID for caller to free
    char *call_id_copy = estrdup(call->call_id);
    return call_id_copy;
}

void opa_exit_function(const char *call_id) {
    if (!global_collector || !global_collector->active || !call_id) {
        return;
    }
    
    opa_collector_t *collector = global_collector;
    
    // Find call by ID
    call_node_t *call = collector->calls;
    while (call) {
        if (call->magic == OPA_CALL_NODE_MAGIC && call->call_id && strcmp(call->call_id, call_id) == 0) {
            // Update end times
            call->end_time = get_time_seconds();
            call->end_cpu_time = get_cpu_time();
            call->end_memory = get_memory_usage();
            call->end_bytes_sent = get_bytes_sent();
            call->end_bytes_received = get_bytes_received();
            
            // Pop from stack (no limit - linked list)
            if (collector->call_stack_top && collector->call_stack_top->magic == OPA_CALL_NODE_MAGIC && 
                collector->call_stack_top->call_id && strcmp(collector->call_stack_top->call_id, call_id) == 0) {
                collector->call_stack_top = collector->call_stack_top->stack_next;
                if (collector->call_stack_depth > 0) {
                    collector->call_stack_depth--;
                }
            }
            
            break;
        }
        call = call->next;
    }
}

// zend_execute_ex hook ()
// NOTE: This function is no longer used - Observer API handles all function tracking
// Keeping it for potential fallback scenarios
void opa_execute_ex(zend_execute_data *execute_data) {
    // Re-entrancy guard: if we're already inside opa_execute_ex, bypass hook logic immediately
    // This must be checked FIRST to prevent infinite recursion and segfaults
    if (in_opa_execute_ex) {
        if (original_zend_execute_ex) {
            original_zend_execute_ex(execute_data);
        }
        return;
    }
    
    // Fast-path: if not actively profiling, call original immediately
    if (!profiling_active) {
        if (original_zend_execute_ex) {
            original_zend_execute_ex(execute_data);
        }
        return;
    }
    
    // Safety check: ensure original handler exists and execute_data is valid
    if (!original_zend_execute_ex || !execute_data) {
        if (original_zend_execute_ex && execute_data) {
            original_zend_execute_ex(execute_data);
        }
        return;
    }
    
    // Additional safety: check collector is still valid
    if (!global_collector || !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // Safety check: ensure func is valid
    if (!execute_data->func) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // Set re-entrancy guard flag: we're now inside the hook
    // This must be set IMMEDIATELY after safety checks to prevent any recursion
    // Any function calls after this point will bypass the hook via the guard check above
    in_opa_execute_ex = 1;
    
    zend_function *func = execute_data->func;
    
    // Get function information
    const char *function_name = NULL;
    const char *class_name = NULL;
    const char *file = NULL;
    int line = 0;
    int function_type = 0; /* 0=user, 1=internal, 2=method */
    
    if (func->common.function_name) {
        function_name = ZSTR_VAL(func->common.function_name);
    }
    
    if (func->common.scope && func->common.scope->name) {
        class_name = ZSTR_VAL(func->common.scope->name);
    }
    
    // Debug: Log ALL PDO-related function calls (DISABLED to prevent segfaults - debug_log may trigger recursion)
    // Temporarily disabled until we can ensure debug_log is safe to call from hook
    /*
    if (class_name && (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0)) {
        debug_log("[execute_ex] PDO CLASS DETECTED: %s::%s (func_type=%d, scope=%p)", 
            class_name, function_name ? function_name : "NULL", func->type, func->common.scope);
    }
    if (function_name && (strcmp(function_name, "query") == 0 || strcmp(function_name, "exec") == 0 || 
                         strcmp(function_name, "prepare") == 0 || strcmp(function_name, "execute") == 0)) {
        php_printf("[OPA execute_ex] PDO METHOD NAME DETECTED: %s (class=%s, func_type=%d)\n", 
            function_name, class_name ? class_name : "NULL", func->type);
        debug_log("[execute_ex] PDO METHOD NAME DETECTED: %s (class=%s, func_type=%d)", 
            function_name, class_name ? class_name : "NULL", func->type);
    }
    */
    
    // Determine function type
    if (func->type == ZEND_USER_FUNCTION) {
        function_type = class_name ? 2 : 0; /* method if has class, otherwise user function */
    } else if (func->type == ZEND_INTERNAL_FUNCTION) {
        function_type = class_name ? 2 : 1; /* method if has class, otherwise internal function */
    }
    
    // Get file and line information
    // Safety: opline can be NULL or point to invalid memory for internal functions
    // Always use zend_get_executed_lineno() as it's safer and works for all function types
    // Only use opline for user functions where we know it's safe
    if (func->type == ZEND_USER_FUNCTION && execute_data->opline) {
        line = execute_data->opline->lineno;
    } else {
        // Fallback: use executed context (safer for internal functions and when opline is invalid)
        line = zend_get_executed_lineno();
    }
    
    // Get filename from function if available (PHP 8.4)
    if (func->type == ZEND_USER_FUNCTION) {
        zend_op_array *op_array = &func->op_array;
        if (op_array && op_array->filename) {
            file = ZSTR_VAL(op_array->filename);
        }
    }
    
    char *call_id = NULL;
    
    // Only track if we have valid function info
    // NOTE: debug_log calls disabled to prevent segfaults - they may trigger recursion or access unsafe globals
    if (function_name || class_name) {
        // debug_log("[execute_ex] Entering function: %s::%s (type=%d)", 
        //     class_name ? class_name : "NULL", function_name ? function_name : "NULL", function_type);
        call_id = opa_enter_function(function_name, class_name, file, line, function_type);
        // if (call_id) {
        //     debug_log("[execute_ex] Entered function: %s, call_id=%s", function_name ? function_name : "NULL", call_id);
        // } else {
        //     debug_log("[execute_ex] Failed to enter function: %s", function_name ? function_name : "NULL");
        // }
    }
    // else {
    //     debug_log("[execute_ex] Skipping function: no name (func=%p)", execute_data->func);
    // }
    
    // Check if this is a PDO method call and capture SQL BEFORE execution
    // Try both is_pdo_method and direct class name check
    int pdo_method = 0;
    int is_pdo_class = 0;
    int is_pdo_method_name = 0;
    
    // Check if it's a PDO class method
    if (class_name && (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0)) {
        is_pdo_class = 1;
    }
    if (function_name && (strcmp(function_name, "prepare") == 0 || strcmp(function_name, "query") == 0 || 
                         strcmp(function_name, "exec") == 0 || strcmp(function_name, "execute") == 0)) {
        is_pdo_method_name = 1;
    }
    
    // Primary check: use is_pdo_method
    if (execute_data && execute_data->func) {
        pdo_method = is_pdo_method(execute_data);
    }
    
    // Fallback: check directly by class name and function name
    if (!pdo_method && is_pdo_class && is_pdo_method_name) {
        pdo_method = 1;
        // debug_log("[execute_ex] PDO method detected via direct class name check: %s::%s", class_name, function_name);
        
        // Note: Lazy hook registration removed - variables not accessible in this scope
        // The hooks should be registered in RINIT
    }
    
    // Additional check: check func->common.scope directly
    if (!pdo_method && execute_data && execute_data->func && execute_data->func->common.scope) {
        const char *scope_name = ZSTR_VAL(execute_data->func->common.scope->name);
        const char *func_name = execute_data->func->common.function_name ? ZSTR_VAL(execute_data->func->common.function_name) : NULL;
        if (scope_name && func_name && 
            (strcmp(scope_name, "PDO") == 0 || strcmp(scope_name, "PDOStatement") == 0) &&
            (strcmp(func_name, "prepare") == 0 || strcmp(func_name, "query") == 0 || 
             strcmp(func_name, "exec") == 0 || strcmp(func_name, "execute") == 0)) {
            pdo_method = 1;
            // php_printf("[OPA execute_ex] PDO method detected via func->common.scope: %s::%s\n", scope_name, func_name);
            // debug_log("[execute_ex] PDO method detected via func->common.scope: %s::%s", scope_name, func_name);
        }
    }
    
    char *sql = NULL;
    double query_start_time = 0.0;
    
    if (pdo_method && execute_data) {
        query_start_time = get_time_seconds();
        // debug_log("[execute_ex] PDO method detected BEFORE execution: function_name=%s, class_name=%s", 
        //     function_name ? function_name : "NULL", class_name ? class_name : "NULL");
        
        // Get method name
        const char *method_name = function_name;
        const char *pdo_class_name = class_name;
        
        // Extract SQL based on method
        if (method_name) {
            if (strcmp(method_name, "prepare") == 0 || 
                strcmp(method_name, "query") == 0 || 
                strcmp(method_name, "exec") == 0) {
                // SQL is in first argument
                uint32_t num_args = ZEND_CALL_NUM_ARGS(execute_data);
                // debug_log("[execute_ex] PDO method %s called with %u arguments", method_name, num_args);
                if (num_args > 0) {
                    zval *arg = ZEND_CALL_ARG(execute_data, 1);
                    if (arg && Z_TYPE_P(arg) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(arg));
                        // debug_log("[execute_ex] Captured SQL from %s::%s: %s", pdo_class_name ? pdo_class_name : "PDO", method_name, sql);
                    }
                    // else {
                    //     debug_log("[execute_ex] WARNING: First argument is not a string (type=%d)", arg ? Z_TYPE_P(arg) : -1);
                    // }
                }
                // else {
                //     debug_log("[execute_ex] WARNING: PDO method %s called with no arguments", method_name);
                // }
            } else if (strcmp(method_name, "execute") == 0 && pdo_class_name && strcmp(pdo_class_name, "PDOStatement") == 0) {
                // For execute(), get SQL from PDOStatement's queryString property
                debug_log("[execute_ex] PDOStatement::execute detected, trying to get queryString");
                if (Z_TYPE(execute_data->This) == IS_OBJECT) {
                    zend_class_entry *ce = Z_OBJCE(execute_data->This);
                    zval query_string;
                    ZVAL_UNDEF(&query_string);
                    zval *query_string_prop = zend_read_property(ce, Z_OBJ(execute_data->This), "queryString", sizeof("queryString") - 1, 1, &query_string);
                    if (query_string_prop && Z_TYPE_P(query_string_prop) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(query_string_prop));
                        debug_log("[execute_ex] Captured SQL from PDOStatement::execute: %s", sql);
                    } else {
                        debug_log("[execute_ex] WARNING: queryString property not found or not a string (type=%d)", 
                            query_string_prop ? Z_TYPE_P(query_string_prop) : -1);
                    }
                    if (Z_TYPE(query_string) != IS_UNDEF) {
                        zval_dtor(&query_string);
                    }
                } else {
                    debug_log("[execute_ex] WARNING: execute_data->This is not an object (type=%d)", Z_TYPE(execute_data->This));
                }
            }
        } else {
            debug_log("[execute_ex] WARNING: PDO method detected but method_name is NULL");
        }
    }
    
    // Check for curl_exec in BEFORE section (capture timing and handle before execution)
    int curl_func_before = 0;
    zval *curl_handle_before = NULL;
    double curl_start_time = 0.0;
    size_t curl_bytes_sent_before = 0;
    size_t curl_bytes_received_before = 0;
    
    // Try to detect curl by checking arguments BEFORE execution (when they're still available)
    // For internal functions, ZEND_CALL_NUM_ARGS might not work, so try direct access
    if (execute_data && execute_data->func) {
        // Try ZEND_CALL_NUM_ARGS first
        uint32_t num_args_before = ZEND_CALL_NUM_ARGS(execute_data);
        debug_log("[execute_ex] BEFORE: num_args=%u, function_name=%s, func_type=%d", 
            num_args_before, function_name ? function_name : "NULL", execute_data->func->type);
        
        // For internal functions, try accessing arguments directly
        zval *arg1_before = NULL;
        if (num_args_before > 0) {
            arg1_before = ZEND_CALL_ARG(execute_data, 1);
        } else if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
            // For internal functions with num_args=0, we can't access arguments directly
            // This is a limitation - we'll need to detect curl_exec another way
            debug_log("[execute_ex] BEFORE: Internal function with num_args=0, cannot access arguments");
        }
        
        if (arg1_before && Z_TYPE_P(arg1_before) == IS_OBJECT) {
            zend_class_entry *ce = Z_OBJCE_P(arg1_before);
            if (ce && ce->name) {
                const char *class_name = ZSTR_VAL(ce->name);
                size_t class_name_len = ZSTR_LEN(ce->name);
                debug_log("[execute_ex] BEFORE: arg1 is object, class=%.*s", (int)class_name_len, class_name);
                if ((class_name_len == sizeof("CurlHandle")-1 && memcmp(class_name, "CurlHandle", sizeof("CurlHandle")-1) == 0) ||
                    (class_name_len == sizeof("CurlMultiHandle")-1 && memcmp(class_name, "CurlMultiHandle", sizeof("CurlMultiHandle")-1) == 0) ||
                    (class_name_len == sizeof("CurlShareHandle")-1 && memcmp(class_name, "CurlShareHandle", sizeof("CurlShareHandle")-1) == 0)) {
                    curl_func_before = 1;
                    curl_handle_before = arg1_before;
                    curl_start_time = get_time_seconds();
                    curl_bytes_sent_before = get_bytes_sent();
                    curl_bytes_received_before = get_bytes_received();
                    debug_log("[execute_ex] BEFORE: Detected curl call, storing handle");
                }
            }
        }
    }
    
    // Fallback: function name detection
    if (function_name && strcmp(function_name, "curl_exec") == 0) {
        curl_func_before = 1;
        curl_start_time = get_time_seconds();
        curl_bytes_sent_before = get_bytes_sent();
        curl_bytes_received_before = get_bytes_received();
    }
    
    // Check if this is an APCu function call and capture info BEFORE execution
    int apcu_func = is_apcu_function(execute_data);
    char *apcu_key = NULL;
    const char *apcu_operation = NULL;
    double apcu_start_time = 0.0;
    
    if (apcu_func && execute_data && function_name) {
        apcu_start_time = get_time_seconds();
        apcu_operation = function_name;
        
        // Get key from first argument for fetch/store/delete/exists
        if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
            zval *key_arg = ZEND_CALL_ARG(execute_data, 1);
            if (key_arg && Z_TYPE_P(key_arg) == IS_STRING) {
                apcu_key = estrdup(Z_STRVAL_P(key_arg));
            } else if (key_arg && Z_TYPE_P(key_arg) == IS_ARRAY) {
                // For apcu_fetch with array of keys, use first key or "array"
                apcu_key = estrdup("array");
            }
        }
    }
    
    // Reset re-entrancy guard before calling original handler
    // This allows the original handler to execute normally without recursion protection
    in_opa_execute_ex = 0;
    
    // Call original handler
    original_zend_execute_ex(execute_data);
    
    // Re-set re-entrancy guard for post-execution processing
    // Post-processing may call functions that trigger zend_execute_ex
    in_opa_execute_ex = 1;
    
    // Capture SQL query AFTER execution if it's a PDO method
    if (pdo_method && sql) {
        double query_end_time = get_time_seconds();
        double query_duration = query_end_time - query_start_time;
        const char *query_type = function_name ? function_name : "PDO";
        int rows_affected = -1;
        
        debug_log("[execute_ex] PDO method detected: pdo_method=%d, sql=%s, call_id=%s, function_name=%s, class_name=%s", 
            pdo_method, sql ? sql : "NULL", call_id ? call_id : "NULL", 
            function_name ? function_name : "NULL", class_name ? class_name : "NULL");
        
        // ALWAYS record SQL query - use global collector's global_sql_queries array
        // This ensures SQL is captured even without call stack
        if (global_collector && global_collector->magic == OPA_COLLECTOR_MAGIC && global_collector->active) {
            pthread_mutex_lock(&global_collector->global_sql_mutex);
            if (!global_collector->global_sql_queries) {
                global_collector->global_sql_queries = ecalloc(1, sizeof(zval));
                if (global_collector->global_sql_queries) {
                    array_init(global_collector->global_sql_queries);
                }
            }
            if (global_collector->global_sql_queries) {
                zval query_data;
                array_init(&query_data);
                
                add_assoc_string(&query_data, "query", sql);
                add_assoc_double(&query_data, "duration", query_duration);
                add_assoc_double(&query_data, "duration_ms", query_duration * 1000.0);
                add_assoc_double(&query_data, "timestamp", query_start_time);
                add_assoc_string(&query_data, "type", (char *)query_type);
                add_assoc_long(&query_data, "rows_affected", rows_affected);
                
                // Determine query type
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
                
                add_assoc_string(&query_data, "db_system", "mysql");
                
                add_next_index_zval(global_collector->global_sql_queries, &query_data);
                php_printf("[OPA execute_ex] SQL query added to global array: %s, duration=%.6f, array_size=%d\n", 
                    sql, query_duration, zend_hash_num_elements(Z_ARRVAL_P(global_collector->global_sql_queries)));
                debug_log("[execute_ex] SQL query added directly to global array: %s, duration=%.6f", sql, query_duration);
            } else {
            }
            pthread_mutex_unlock(&global_collector->global_sql_mutex);
        } else {
        }
        
        // Also try to record via record_sql_query (for call node tracking)
        if (call_id) {
            record_sql_query(sql, query_duration, NULL, query_type, rows_affected, NULL, "mysql", NULL);
            debug_log("[execute_ex] Also recorded SQL query via record_sql_query: %s, duration=%.6f, call_id=%s", sql, query_duration, call_id);
        } else {
            // Create a root call node if we don't have one
            char *root_call_id = opa_enter_function("__root__", NULL, __FILE__, __LINE__, 0);
            if (root_call_id) {
                efree(root_call_id);
                record_sql_query(sql, query_duration, NULL, query_type, rows_affected, NULL, "mysql", NULL);
                debug_log("[execute_ex] Recorded SQL query after creating root call: %s, duration=%.6f", sql, query_duration);
            }
        }
        efree(sql);
    } else if (pdo_method) {
        debug_log("[execute_ex] PDO method detected but no SQL captured: pdo_method=%d, sql=%p, function_name=%s", 
            pdo_method, sql, function_name ? function_name : "NULL");
    }
    
    // Re-check curl function after execution using argument-based detection
    // Only check if function has at least 1 argument
    zval *curl_handle_after = NULL;
    int curl_func_after = 0;
    int curl_func_type_after = 0;
    
    // Detect curl_exec calls - try is_curl_call first (works when function_name is NULL)
    // Then fallback to function name detection
    debug_log("[execute_ex] Before curl detection: execute_data=%p, func=%p, function_name=%s", 
        execute_data, execute_data ? execute_data->func : NULL, function_name ? function_name : "NULL");
    if (execute_data && execute_data->func) {
        uint32_t num_args = ZEND_CALL_NUM_ARGS(execute_data);
        debug_log("[execute_ex] Checking curl: num_args=%u", num_args);
        if (num_args > 0) {
            debug_log("[execute_ex] Calling is_curl_call with num_args=%u", num_args);
            curl_func_after = is_curl_call(execute_data, &curl_handle_after);
            debug_log("[execute_ex] is_curl_call returned: %d", curl_func_after);
            if (curl_func_after) {
                // Check if it's curl_exec by argument count (curl_exec has 1 arg)
                if (num_args == 1) {
                    curl_func_type_after = 1; // curl_exec
                } else {
                    curl_func_type_after = get_curl_function_type(execute_data);
                }
            }
        } else {
            debug_log("[execute_ex] Skipping is_curl_call: num_args=%u (need > 0)", num_args);
        }
    } else {
        debug_log("[execute_ex] Skipping curl detection: execute_data=%p, func=%p", 
            execute_data, execute_data ? execute_data->func : NULL);
    }
    
    // Fallback: function name detection (for older PHP versions or when is_curl_call fails)
    if (!curl_func_after && function_name && strcmp(function_name, "curl_exec") == 0) {
        curl_func_after = 1;
        curl_func_type_after = 1; // curl_exec
        
        // Try to get curl handle from first argument
        if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
            curl_handle_after = ZEND_CALL_ARG(execute_data, 1);
            // Validate handle is valid
            if (curl_handle_after && (Z_TYPE_P(curl_handle_after) == IS_UNDEF || Z_TYPE_P(curl_handle_after) == IS_NULL)) {
                curl_handle_after = NULL;
            }
        }
    }
    
    debug_log("[execute_ex] AFTER curl check: curl_func_after=%d, curl_func_type_after=%d, call_id=%s", 
        curl_func_after, curl_func_type_after, call_id ? call_id : "NULL");
    
    // Capture HTTP request info AFTER execution if it's curl_exec
    // Declare variables outside the if block so they're accessible
    double curl_duration = 0.0;
    size_t curl_bytes_sent = 0;
    size_t curl_bytes_received = 0;
    int status_code = 0;
    const char *error = NULL;
    char *curl_url = NULL;
    char *curl_method = NULL;
    
    // Process curl_exec calls and capture HTTP request details
    if (curl_func_after && curl_func_type_after == 1) {
        debug_log("[execute_ex] Processing curl_exec - curl_func_after=%d, curl_func_type_after=%d, call_id=%s, curl_handle_after=%p", 
            curl_func_after, curl_func_type_after, call_id ? call_id : "NULL", curl_handle_after);
        
        // Calculate duration and bytes from BEFORE section
        double curl_end_time = get_time_seconds();
        curl_duration = curl_end_time - curl_start_time;
        size_t curl_bytes_sent_after = get_bytes_sent();
        size_t curl_bytes_received_after = get_bytes_received();
        curl_bytes_sent = curl_bytes_sent_after - curl_bytes_sent_before;
        curl_bytes_received = curl_bytes_received_after - curl_bytes_received_before;
        
        // Try to get status code, headers, and other details from curl handle
        char *request_headers_str = NULL;
        char *response_headers_str = NULL;
        char *uri_path = NULL;
        char *query_string = NULL;
        double dns_time = 0.0;
        double connect_time = 0.0;
        double total_time = 0.0;
        size_t response_size = curl_bytes_received;
        size_t request_size = curl_bytes_sent;
        
        // Use curl_handle_after obtained from function name detection
        if (curl_handle_after && (Z_TYPE_P(curl_handle_after) == IS_RESOURCE || Z_TYPE_P(curl_handle_after) == IS_OBJECT)) {
            zval *curl_handle = curl_handle_after;
                // Get full curl_getinfo array with all details
                zval curl_getinfo_func, curl_getinfo_args[1], curl_getinfo_ret;
                ZVAL_UNDEF(&curl_getinfo_func);
                ZVAL_UNDEF(&curl_getinfo_args[0]);
                ZVAL_UNDEF(&curl_getinfo_ret);
                
                ZVAL_STRING(&curl_getinfo_func, "curl_getinfo");
                ZVAL_COPY(&curl_getinfo_args[0], curl_handle);
                
                zend_fcall_info fci;
                zend_fcall_info_cache fcc;
                if (zend_fcall_info_init(&curl_getinfo_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
                    fci.size = sizeof(fci);
                    ZVAL_UNDEF(&fci.function_name);
                    fci.object = NULL;
                    fci.param_count = 1;
                    fci.params = curl_getinfo_args;
                    fci.retval = &curl_getinfo_ret;
                    
                    if (zend_call_function(&fci, &fcc) == SUCCESS) {
                        if (Z_TYPE(curl_getinfo_ret) == IS_ARRAY) {
                            // Get URL
                            zval *url_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "url", sizeof("url") - 1);
                            if (url_val && Z_TYPE_P(url_val) == IS_STRING) {
                                curl_url = estrdup(Z_STRVAL_P(url_val));
                            }
                            
                            // Get HTTP method from request_method or default to GET
                            zval *method_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_method", sizeof("request_method") - 1);
                            if (method_val && Z_TYPE_P(method_val) == IS_STRING) {
                                curl_method = estrdup(Z_STRVAL_P(method_val));
                            } else {
                                curl_method = estrdup("GET");
                            }
                            
                            // Get status code
                            zval *status_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "http_code", sizeof("http_code") - 1);
                            if (status_val && Z_TYPE_P(status_val) == IS_LONG) {
                                status_code = Z_LVAL_P(status_val);
                            }
                            
                            // Get request headers (CURLINFO_HEADER_OUT = 2)
                            zval *header_out_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_header", sizeof("request_header") - 1);
                            if (!header_out_val) {
                                header_out_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_header_out", sizeof("request_header_out") - 1);
                            }
                            if (header_out_val && Z_TYPE_P(header_out_val) == IS_STRING) {
                                request_headers_str = estrdup(Z_STRVAL_P(header_out_val));
                            }
                            
                            // Get request size (size_upload)
                            zval *size_upload_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "size_upload", sizeof("size_upload") - 1);
                            if (size_upload_val && Z_TYPE_P(size_upload_val) == IS_DOUBLE) {
                                request_size = (size_t)Z_DVAL_P(size_upload_val);
                            } else if (size_upload_val && Z_TYPE_P(size_upload_val) == IS_LONG) {
                                request_size = (size_t)Z_LVAL_P(size_upload_val);
                            }
                            
                            // Get response size (size_download)
                            zval *size_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "size_download", sizeof("size_download") - 1);
                            if (size_val && Z_TYPE_P(size_val) == IS_DOUBLE) {
                                response_size = (size_t)Z_DVAL_P(size_val);
                            } else if (size_val && Z_TYPE_P(size_val) == IS_LONG) {
                                response_size = (size_t)Z_LVAL_P(size_val);
                            }
                            
                            // Update global network byte counters with actual values from cURL
                            if (request_size > 0) {
                                add_bytes_sent(request_size);
                            }
                            if (response_size > 0) {
                                add_bytes_received(response_size);
                            }
                            
                            // Update curl_bytes_sent and curl_bytes_received with actual sizes
                            curl_bytes_sent = request_size;
                            curl_bytes_received = response_size;
                            
                            // Get timing information
                            zval *dns_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "namelookup_time", sizeof("namelookup_time") - 1);
                            if (dns_val && Z_TYPE_P(dns_val) == IS_DOUBLE) {
                                dns_time = Z_DVAL_P(dns_val);
                            }
                            
                            zval *conn_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "connect_time", sizeof("connect_time") - 1);
                            if (conn_val && Z_TYPE_P(conn_val) == IS_DOUBLE) {
                                connect_time = Z_DVAL_P(conn_val);
                            }
                            
                            zval *total_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "total_time", sizeof("total_time") - 1);
                            if (total_val && Z_TYPE_P(total_val) == IS_DOUBLE) {
                                total_time = Z_DVAL_P(total_val);
                            }
                            
                            // Extract URI path and query string from URL
                            if (curl_url) {
                                const char *path_start = strstr(curl_url, "://");
                                if (path_start) {
                                    path_start += 3;
                                    const char *path_end = strchr(path_start, '/');
                                    if (path_end) {
                                        const char *query_start = strchr(path_end, '?');
                                        if (query_start) {
                                            size_t path_len = query_start - path_end;
                                            uri_path = estrndup(path_end, path_len);
                                            query_string = estrdup(query_start + 1);
                                        } else {
                                            uri_path = estrdup(path_end);
                                        }
                                    }
                                }
                            }
                        }
                        zval_dtor(&curl_getinfo_ret);
                    }
                }
                zval_dtor(&curl_getinfo_func);
                zval_dtor(&curl_getinfo_args[0]);
                
            // Get error if any
            zval curl_error_func, curl_error_args[1], curl_error_ret;
            ZVAL_UNDEF(&curl_error_func);
            ZVAL_UNDEF(&curl_error_args[0]);
            ZVAL_UNDEF(&curl_error_ret);
            
            ZVAL_STRING(&curl_error_func, "curl_error");
            ZVAL_COPY(&curl_error_args[0], curl_handle);
            
            if (zend_fcall_info_init(&curl_error_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
                fci.size = sizeof(fci);
                ZVAL_UNDEF(&fci.function_name);
                fci.object = NULL;
                fci.param_count = 1;
                fci.params = curl_error_args;
                fci.retval = &curl_error_ret;
                
                if (zend_call_function(&fci, &fcc) == SUCCESS) {
                    if (Z_TYPE(curl_error_ret) == IS_STRING && Z_STRLEN(curl_error_ret) > 0) {
                        error = estrdup(Z_STRVAL(curl_error_ret));
                    }
                    zval_dtor(&curl_error_ret);
                }
            }
            zval_dtor(&curl_error_func);
            zval_dtor(&curl_error_args[0]);
        }
        
        // Log HTTP response code for cURL requests
        if (status_code > 0) {
            char fields[512];
            snprintf(fields, sizeof(fields), 
                "{\"method\":\"%s\",\"url\":\"%s\",\"status_code\":%d,\"duration_ms\":%.2f,\"bytes_sent\":%zu,\"bytes_received\":%zu}",
                curl_method ? curl_method : "GET", 
                curl_url ? curl_url : "unknown", 
                status_code, 
                curl_duration * 1000.0,
                curl_bytes_sent,
                curl_bytes_received);
            if (status_code >= 500) {
                log_error("HTTP request failed with server error", error ? error : "Server error", fields);
            } else if (status_code >= 400) {
                log_warn("HTTP request failed with client error", fields);
            } else {
                log_info("HTTP request completed", fields);
            }
        }
        
        // Record HTTP request with enhanced details
        record_http_request_enhanced(curl_url, curl_method, status_code, curl_bytes_sent, curl_bytes_received, 
            curl_duration, error, uri_path, query_string, request_headers_str, response_headers_str, 
            response_size, request_size, dns_time, connect_time, total_time);
        debug_log("[execute_ex] Recorded HTTP request: %s %s, status=%d, duration=%.6f", 
            curl_method ? curl_method : "GET", curl_url ? curl_url : "unknown", status_code, curl_duration);
        
        if (curl_url) efree((void *)curl_url);
        if (curl_method) efree((void *)curl_method);
        if (error) efree((void *)error);
        if (uri_path) efree((void *)uri_path);
        if (query_string) efree((void *)query_string);
        if (request_headers_str) efree((void *)request_headers_str);
        if (response_headers_str) efree((void *)response_headers_str);
    }
    
    // Capture cache operation info AFTER execution if it's an APCu function
    if (apcu_func && call_id && function_name) {
        double apcu_end_time = get_time_seconds();
        double apcu_duration = apcu_end_time - apcu_start_time;
        int hit = 0;
        size_t data_size = 0;
        
        // Determine hit/miss and data size based on function and return value
        if (strcmp(function_name, "apcu_fetch") == 0 || strcmp(function_name, "apc_fetch") == 0) {
            // For fetch, check if return value is not false (hit) or false (miss)
            // Note: We can't easily access return_value here, so we'll use a heuristic:
            // If duration is very short (< 0.001s), it's likely a hit
            // Otherwise, we'll mark as miss for now
            hit = (apcu_duration < 0.001) ? 1 : 0;
        } else if (strcmp(function_name, "apcu_exists") == 0 || strcmp(function_name, "apc_exists") == 0) {
            // For exists, similar heuristic
            hit = (apcu_duration < 0.001) ? 1 : 0;
        } else if (strcmp(function_name, "apcu_store") == 0 || strcmp(function_name, "apc_store") == 0) {
            // For store, try to get data size from second argument
            if (ZEND_CALL_NUM_ARGS(execute_data) > 1) {
                zval *value_arg = ZEND_CALL_ARG(execute_data, 2);
                if (value_arg) {
                    // Estimate size based on type
                    if (Z_TYPE_P(value_arg) == IS_STRING) {
                        data_size = Z_STRLEN_P(value_arg);
                    } else if (Z_TYPE_P(value_arg) == IS_ARRAY) {
                        data_size = zend_hash_num_elements(Z_ARRVAL_P(value_arg)) * 100; // Rough estimate
                    } else {
                        data_size = sizeof(zval); // Minimum size
                    }
                }
            }
            hit = 1; // Store is always a "hit" (successful write)
        } else if (strcmp(function_name, "apcu_delete") == 0 || strcmp(function_name, "apc_delete") == 0) {
            hit = 1; // Delete is always a "hit" (successful operation)
        } else if (strcmp(function_name, "apcu_clear_cache") == 0) {
            hit = 1; // Clear is always a "hit"
        }
        
        // Record cache operation
        record_cache_operation(apcu_key, apcu_operation, hit, apcu_duration, data_size, "apcu");
        debug_log("[execute_ex] Recorded cache operation: %s key=%s, hit=%d, duration=%.6f", 
            apcu_operation, apcu_key ? apcu_key : "N/A", hit, apcu_duration);
        
        if (apcu_key) efree(apcu_key);
    }
    
    // Exit function tracking only if we entered it
    if (call_id) {
        opa_exit_function(call_id);
        efree(call_id);
    }
    
    // Reset re-entrancy guard at the end of function
    in_opa_execute_ex = 0;
}

/* SQL Profiling Hooks */

// Store original function handlers
static zend_function *orig_mysqli_query_func = NULL;
static zif_handler orig_mysqli_query_handler = NULL;
static zend_function *orig_pdo_query_func = NULL;
static zif_handler orig_pdo_query_handler = NULL;
static zend_function *orig_pdo_exec_func = NULL;
static zif_handler orig_pdo_exec_handler = NULL;
static zend_function *orig_pdo_prepare_func = NULL;
static zif_handler orig_pdo_prepare_handler = NULL;
static zend_function *orig_pdo_stmt_execute_func = NULL;
static zif_handler orig_pdo_stmt_execute_handler = NULL;
static zend_function *orig_curl_exec_func = NULL;
static zif_handler orig_curl_exec_handler = NULL;
// Store curl_getinfo and curl_error function pointers to call directly and bypass observers
static zend_function *curl_getinfo_func = NULL;
static zend_function *curl_error_func = NULL;

// Track if PDO Observer has been registered (to avoid repeated registration)
static int pdo_observer_registered = 0;
static int general_observer_registered = 0;

/* cURL Profiling Hook */

// curl_exec wrapper handler (internal function signature)
static void zif_opa_curl_exec(zend_execute_data *execute_data, zval *return_value) {
    // Fast-path: if not actively profiling, call original immediately
    if (!profiling_active) {
        if (orig_curl_exec_handler) {
            orig_curl_exec_handler(execute_data, return_value);
        } else if (orig_curl_exec_func && orig_curl_exec_func->internal_function.handler) {
            orig_curl_exec_func->internal_function.handler(execute_data, return_value);
        } else {
            ZVAL_FALSE(return_value);
        }
        return;
    }
    
    // Get curl handle from arguments
    zval *curl_handle = NULL;
    if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
        curl_handle = ZEND_CALL_ARG(execute_data, 1);
    }
    
    if (!curl_handle || (Z_TYPE_P(curl_handle) != IS_RESOURCE && Z_TYPE_P(curl_handle) != IS_OBJECT)) {
        // No valid curl handle - just call original and return
        if (orig_curl_exec_handler) {
            orig_curl_exec_handler(execute_data, return_value);
        } else if (orig_curl_exec_func && orig_curl_exec_func->internal_function.handler) {
            orig_curl_exec_func->internal_function.handler(execute_data, return_value);
        } else {
            ZVAL_FALSE(return_value);
        }
        return;
    }
    
    // Record timing before call
    double start_time = get_time_seconds();
    size_t bytes_sent_before = get_bytes_sent();
    size_t bytes_received_before = get_bytes_received();
    
    // Call original handler
    if (orig_curl_exec_handler) {
        orig_curl_exec_handler(execute_data, return_value);
    } else if (orig_curl_exec_func && orig_curl_exec_func->internal_function.handler) {
        orig_curl_exec_func->internal_function.handler(execute_data, return_value);
    } else {
        ZVAL_FALSE(return_value);
        return;
    }
    
    // Calculate duration and bytes after call
    double end_time = get_time_seconds();
    double duration = end_time - start_time;
    size_t bytes_sent_after = get_bytes_sent();
    size_t bytes_received_after = get_bytes_received();
    size_t bytes_sent = bytes_sent_after - bytes_sent_before;
    size_t bytes_received = bytes_received_after - bytes_received_before;
    
    // Extract HTTP request details using curl_getinfo
    char *curl_url = NULL;
    char *curl_method = NULL;
    int status_code = 0;
    const char *error = NULL;
    char *request_headers_str = NULL;
    char *response_headers_str = NULL;
    char *uri_path = NULL;
    char *query_string = NULL;
    double dns_time = 0.0;
    double connect_time = 0.0;
    double total_time = 0.0;
    size_t response_size = bytes_received;
    
    // Call curl_getinfo to get all details
    zval curl_getinfo_func, curl_getinfo_args[1], curl_getinfo_ret;
    ZVAL_UNDEF(&curl_getinfo_func);
    ZVAL_UNDEF(&curl_getinfo_args[0]);
    ZVAL_UNDEF(&curl_getinfo_ret);
    
    ZVAL_STRING(&curl_getinfo_func, "curl_getinfo");
    ZVAL_COPY(&curl_getinfo_args[0], curl_handle);
    
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;
    if (zend_fcall_info_init(&curl_getinfo_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
        fci.size = sizeof(fci);
        ZVAL_UNDEF(&fci.function_name);
        fci.object = NULL;
        fci.param_count = 1;
        fci.params = curl_getinfo_args;
        fci.retval = &curl_getinfo_ret;
        
        if (zend_call_function(&fci, &fcc) == SUCCESS) {
            if (Z_TYPE(curl_getinfo_ret) == IS_ARRAY) {
                // Get URL
                zval *url_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "url", sizeof("url") - 1);
                if (url_val && Z_TYPE_P(url_val) == IS_STRING) {
                    curl_url = estrdup(Z_STRVAL_P(url_val));
                }
                
                // Get HTTP method
                zval *method_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_method", sizeof("request_method") - 1);
                if (method_val && Z_TYPE_P(method_val) == IS_STRING) {
                    curl_method = estrdup(Z_STRVAL_P(method_val));
                } else {
                    curl_method = estrdup("GET");
                }
                
                // Get status code
                zval *status_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "http_code", sizeof("http_code") - 1);
                if (status_val && Z_TYPE_P(status_val) == IS_LONG) {
                    status_code = Z_LVAL_P(status_val);
                }
                
                // Get request headers
                zval *header_out_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_header", sizeof("request_header") - 1);
                if (!header_out_val) {
                    header_out_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "request_header_out", sizeof("request_header_out") - 1);
                }
                if (header_out_val && Z_TYPE_P(header_out_val) == IS_STRING) {
                    request_headers_str = estrdup(Z_STRVAL_P(header_out_val));
                }
                
                // Get response headers
                zval *header_in_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "response_header", sizeof("response_header") - 1);
                if (header_in_val && Z_TYPE_P(header_in_val) == IS_STRING) {
                    response_headers_str = estrdup(Z_STRVAL_P(header_in_val));
                }
                
                // Get response size (size_download)
                zval *size_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "size_download", sizeof("size_download") - 1);
                if (size_val && Z_TYPE_P(size_val) == IS_DOUBLE) {
                    response_size = (size_t)Z_DVAL_P(size_val);
                } else if (size_val && Z_TYPE_P(size_val) == IS_LONG) {
                    response_size = (size_t)Z_LVAL_P(size_val);
                }
                
                // Get request size (size_upload)
                size_t request_size_from_curl = 0;
                zval *size_upload_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "size_upload", sizeof("size_upload") - 1);
                if (size_upload_val && Z_TYPE_P(size_upload_val) == IS_DOUBLE) {
                    request_size_from_curl = (size_t)Z_DVAL_P(size_upload_val);
                } else if (size_upload_val && Z_TYPE_P(size_upload_val) == IS_LONG) {
                    request_size_from_curl = (size_t)Z_LVAL_P(size_upload_val);
                }
                
                // Update global network byte counters with actual values from cURL
                if (request_size_from_curl > 0) {
                    add_bytes_sent(request_size_from_curl);
                }
                if (response_size > 0) {
                    add_bytes_received(response_size);
                }
                
                // Use actual sizes from curl_getinfo instead of difference calculation
                bytes_sent = request_size_from_curl;
                bytes_received = response_size;
                
                // Get timing information
                zval *dns_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "namelookup_time", sizeof("namelookup_time") - 1);
                if (dns_val && Z_TYPE_P(dns_val) == IS_DOUBLE) {
                    dns_time = Z_DVAL_P(dns_val);
                }
                
                zval *conn_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "connect_time", sizeof("connect_time") - 1);
                if (conn_val && Z_TYPE_P(conn_val) == IS_DOUBLE) {
                    connect_time = Z_DVAL_P(conn_val);
                }
                
                zval *total_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "total_time", sizeof("total_time") - 1);
                if (total_val && Z_TYPE_P(total_val) == IS_DOUBLE) {
                    total_time = Z_DVAL_P(total_val);
                }
                
                // Extract URI path and query string from URL
                if (curl_url) {
                    const char *path_start = strstr(curl_url, "://");
                    if (path_start) {
                        path_start += 3;
                        const char *path_end = strchr(path_start, '/');
                        if (path_end) {
                            const char *query_start = strchr(path_end, '?');
                            if (query_start) {
                                size_t path_len = query_start - path_end;
                                uri_path = estrndup(path_end, path_len);
                                query_string = estrdup(query_start + 1);
                            } else {
                                uri_path = estrdup(path_end);
                            }
                        }
                    }
                }
            }
            zval_dtor(&curl_getinfo_ret);
        }
    }
    zval_dtor(&curl_getinfo_func);
    zval_dtor(&curl_getinfo_args[0]);
    
    // Get error if any
    zval curl_error_func, curl_error_args[1], curl_error_ret;
    ZVAL_UNDEF(&curl_error_func);
    ZVAL_UNDEF(&curl_error_args[0]);
    ZVAL_UNDEF(&curl_error_ret);
    
    ZVAL_STRING(&curl_error_func, "curl_error");
    ZVAL_COPY(&curl_error_args[0], curl_handle);
    
    if (zend_fcall_info_init(&curl_error_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
        fci.size = sizeof(fci);
        ZVAL_UNDEF(&fci.function_name);
        fci.object = NULL;
        fci.param_count = 1;
        fci.params = curl_error_args;
        fci.retval = &curl_error_ret;
        
        if (zend_call_function(&fci, &fcc) == SUCCESS) {
            if (Z_TYPE(curl_error_ret) == IS_STRING && Z_STRLEN(curl_error_ret) > 0) {
                error = estrdup(Z_STRVAL(curl_error_ret));
            }
            zval_dtor(&curl_error_ret);
        }
    }
    zval_dtor(&curl_error_func);
    zval_dtor(&curl_error_args[0]);
    
    // Log HTTP response code
    if (status_code > 0) {
        char fields[512];
        snprintf(fields, sizeof(fields), 
            "{\"method\":\"%s\",\"url\":\"%s\",\"status_code\":%d,\"duration_ms\":%.2f,\"bytes_sent\":%zu,\"bytes_received\":%zu}",
            curl_method ? curl_method : "GET", 
            curl_url ? curl_url : "unknown", 
            status_code, 
            duration * 1000.0,
            bytes_sent,
            bytes_received);
        if (status_code >= 500) {
            log_error("HTTP request failed with server error", error ? error : "Server error", fields);
        } else if (status_code >= 400) {
            log_warn("HTTP request failed with client error", fields);
        } else {
            log_info("HTTP request completed", fields);
        }
    }
    
    // Record HTTP request with enhanced details
    // Calculate request_size from bytes_sent (approximate)
    size_t request_size = bytes_sent;
    record_http_request_enhanced(curl_url, curl_method, status_code, bytes_sent, bytes_received, 
        duration, error, uri_path, query_string, request_headers_str, response_headers_str, 
        response_size, request_size, dns_time, connect_time, total_time);
    debug_log("[zif_opa_curl_exec] Recorded HTTP request: %s %s, status=%d, duration=%.6f", 
        curl_method ? curl_method : "GET", curl_url ? curl_url : "unknown", status_code, duration);
    
    // Cleanup
    if (curl_url) efree((void *)curl_url);
    if (curl_method) efree((void *)curl_method);
    if (error) efree((void *)error);
    if (uri_path) efree((void *)uri_path);
    if (query_string) efree((void *)query_string);
    if (request_headers_str) efree((void *)request_headers_str);
    if (response_headers_str) efree((void *)response_headers_str);
}

// Helper: Get current time in microseconds
static double get_microtime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

// MySQLi query hook
PHP_FUNCTION(opaphp_mysqli_query) {
    // Fast-path: if not actively profiling, call original immediately
    if (!profiling_active) {
        if (orig_mysqli_query_handler) {
            orig_mysqli_query_handler(execute_data, return_value);
        } else if (orig_mysqli_query_func && orig_mysqli_query_func->internal_function.handler) {
            orig_mysqli_query_func->internal_function.handler(execute_data, return_value);
        }
        return;
    }
    
    zval *link;
    zend_string *query;
    double start = get_microtime();
    
    // Parse parameters
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT(link)
        Z_PARAM_STR(query)
    ZEND_PARSE_PARAMETERS_END();
    
    // Call original mysqli_query
    if (orig_mysqli_query_handler) {
        orig_mysqli_query_handler(execute_data, return_value);
    } else if (orig_mysqli_query_func && orig_mysqli_query_func->internal_function.handler) {
        orig_mysqli_query_func->internal_function.handler(execute_data, return_value);
    }
    
    double elapsed = (get_microtime() - start) * 1000.0; // Convert to milliseconds
    
    // Get rows affected/returned
    long rows_affected = -1;
    
    // Try to get row count from return_value
    if (return_value && Z_TYPE_P(return_value) != IS_NULL) {
        // Check if return_value is a mysqli_result object (SELECT queries)
        if (Z_TYPE_P(return_value) == IS_OBJECT) {
            zend_class_entry *ce = Z_OBJCE_P(return_value);
            const char *class_name = ZSTR_VAL(ce->name);
            
            // If it's a mysqli_result, get num_rows
            if (strcmp(class_name, "mysqli_result") == 0) {
                zval num_rows_zv;
                zval method_name;
                zval return_zv;
                ZVAL_OBJ(&return_zv, Z_OBJ_P(return_value));
                ZVAL_STRING(&method_name, "num_rows");
                // Try to read property directly
                zval *num_rows_prop = zend_read_property(ce, Z_OBJ_P(return_value), "num_rows", sizeof("num_rows") - 1, 1, &num_rows_zv);
                if (num_rows_prop && Z_TYPE_P(num_rows_prop) == IS_LONG) {
                    rows_affected = Z_LVAL_P(num_rows_prop);
                } else if (call_user_function(EG(function_table), &return_zv, &method_name, &num_rows_zv, 0, NULL) == SUCCESS) {
                    if (Z_TYPE(num_rows_zv) == IS_LONG) {
                        rows_affected = Z_LVAL(num_rows_zv);
                    }
                    zval_ptr_dtor(&num_rows_zv);
                }
                zval_ptr_dtor(&method_name);
            } else if (strcmp(class_name, "mysqli") == 0) {
                // For non-SELECT queries, get affected_rows from the link
                zval affected_rows_zv;
                zval method_name;
                zval link_zv;
                ZVAL_OBJ(&link_zv, Z_OBJ_P(link));
                ZVAL_STRING(&method_name, "affected_rows");
                zval *affected_rows_prop = zend_read_property(Z_OBJCE_P(link), Z_OBJ_P(link), "affected_rows", sizeof("affected_rows") - 1, 1, &affected_rows_zv);
                if (affected_rows_prop && Z_TYPE_P(affected_rows_prop) == IS_LONG) {
                    rows_affected = Z_LVAL_P(affected_rows_prop);
                } else if (call_user_function(EG(function_table), &link_zv, &method_name, &affected_rows_zv, 0, NULL) == SUCCESS) {
                    if (Z_TYPE(affected_rows_zv) == IS_LONG) {
                        rows_affected = Z_LVAL(affected_rows_zv);
                    }
                    zval_ptr_dtor(&affected_rows_zv);
                }
                zval_ptr_dtor(&method_name);
            }
        } else if (Z_TYPE_P(return_value) == IS_TRUE) {
            // For INSERT/UPDATE/DELETE, get affected_rows from link
            zval affected_rows_zv;
            zval method_name;
            zval link_zv;
            ZVAL_OBJ(&link_zv, Z_OBJ_P(link));
            ZVAL_STRING(&method_name, "affected_rows");
            zval *affected_rows_prop = zend_read_property(Z_OBJCE_P(link), Z_OBJ_P(link), "affected_rows", sizeof("affected_rows") - 1, 1, &affected_rows_zv);
            if (affected_rows_prop && Z_TYPE_P(affected_rows_prop) == IS_LONG) {
                rows_affected = Z_LVAL_P(affected_rows_prop);
            } else if (call_user_function(EG(function_table), &link_zv, &method_name, &affected_rows_zv, 0, NULL) == SUCCESS) {
                if (Z_TYPE(affected_rows_zv) == IS_LONG) {
                    rows_affected = Z_LVAL(affected_rows_zv);
                }
                zval_ptr_dtor(&affected_rows_zv);
            }
            zval_ptr_dtor(&method_name);
        }
    }
    
    // Log SQL query with timing
    if (query) {
        
        // Send SQL query data to agent via record_sql_query
        // Duration is in milliseconds, convert to seconds for record_sql_query
        double duration_seconds = elapsed / 1000.0;
        // Note: db_host, db_system, db_dsn extraction removed for simplicity
        record_sql_query(ZSTR_VAL(query), duration_seconds, NULL, "mysqli_query", rows_affected, NULL, NULL, NULL);
    }
}

// PDO::query/exec/prepare hook handler (standalone function, not a method)
// This function replaces the PDO method handlers
static void zif_opa_pdo_query(zend_execute_data *execute_data, zval *return_value) {
    php_printf("[OPA zif_opa_pdo_query] HOOK CALLED!\n");
    debug_log("[PDO method] Hook called");
    
    zend_string *sql = NULL;
    double start = get_microtime();
    const char *method_name = "query"; // Default, will be determined from function
    
    // Determine which method was called by checking the function
    if (execute_data && execute_data->func && execute_data->func->common.function_name) {
        method_name = ZSTR_VAL(execute_data->func->common.function_name);
    }
    
    // Parse parameters (query/exec/prepare take at least 1 parameter: SQL string)
    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_STR(sql)
    ZEND_PARSE_PARAMETERS_END();
    
    debug_log("[PDO method] SQL: %s", sql ? ZSTR_VAL(sql) : "NULL");
    
    // Call original handler based on which method was called
    zif_handler orig_handler = NULL;
    if (strcmp(method_name, "query") == 0 && orig_pdo_query_handler) {
        orig_handler = orig_pdo_query_handler;
    } else if (strcmp(method_name, "exec") == 0 && orig_pdo_exec_handler) {
        orig_handler = orig_pdo_exec_handler;
    } else if (strcmp(method_name, "prepare") == 0 && orig_pdo_prepare_handler) {
        orig_handler = orig_pdo_prepare_handler;
    }
    
    if (orig_handler) {
        orig_handler(execute_data, return_value);
    } else if (orig_pdo_query_func && orig_pdo_query_func->internal_function.handler) {
        orig_pdo_query_func->internal_function.handler(execute_data, return_value);
    }
    
    double elapsed = (get_microtime() - start) * 1000.0;
    
    // Get row count if available
    long row_count = -1;
    if (return_value && Z_TYPE_P(return_value) == IS_OBJECT) {
        zend_class_entry *ce = Z_OBJCE_P(return_value);
        const char *class_name = ZSTR_VAL(ce->name);
        
        // For PDOStatement (from query/prepare), get rowCount()
        if (strcmp(class_name, "PDOStatement") == 0) {
            zval row_count_zv;
            zval method_name_zv;
            zval return_zv;
            ZVAL_OBJ(&return_zv, Z_OBJ_P(return_value));
            ZVAL_STRING(&method_name_zv, "rowCount");
            if (call_user_function(EG(function_table), &return_zv, &method_name_zv, &row_count_zv, 0, NULL) == SUCCESS) {
                if (Z_TYPE(row_count_zv) == IS_LONG) {
                    row_count = Z_LVAL(row_count_zv);
                }
                zval_ptr_dtor(&row_count_zv);
            }
            zval_ptr_dtor(&method_name_zv);
        } else if (strcmp(method_name, "exec") == 0) {
            // For exec(), return_value is the number of affected rows
            if (Z_TYPE_P(return_value) == IS_LONG) {
                row_count = Z_LVAL_P(return_value);
            }
        }
    }
    
    // Log SQL query
    if (sql) {
        if (OPA_G(debug_log_enabled)) {
            php_printf("[OPA SQL Profiling] PDO Query: %s | Time: %.3fms | Rows: %ld\n", 
                   ZSTR_VAL(sql), elapsed, row_count);
        }
        
        // Send SQL query data to agent via record_sql_query
        double duration_seconds = elapsed / 1000.0;
        char query_type_str[64];
        snprintf(query_type_str, sizeof(query_type_str), "PDO::%s", method_name);
        
        debug_log("[PDO %s] Recording SQL query: %s, duration=%.3fs, rows=%ld", 
            method_name, ZSTR_VAL(sql), duration_seconds, row_count);
        
        // Ensure collector is initialized before recording
        if (!global_collector) {
            global_collector = opa_collector_init();
            if (global_collector) {
                opa_collector_start(global_collector);
            }
        }
        
        // Note: db_host, db_system, db_dsn extraction removed for simplicity
        // Can be added back if needed by reading from PDO connection object
        record_sql_query(ZSTR_VAL(sql), duration_seconds, NULL, query_type_str, row_count, NULL, NULL, NULL);
        debug_log("[PDO %s] SQL query recorded", method_name);
    }
}

// PDOStatement::execute hook handler (standalone function, not a method)
// This function replaces the PDOStatement::execute method handler
static void zif_opa_pdo_stmt_execute(zend_execute_data *execute_data, zval *return_value) {
    double start = get_microtime();
    
    // Parse parameters (execute can take 0 or more parameters)
    // We don't need to parse them, just pass through
    ZEND_PARSE_PARAMETERS_START(0, -1)
    ZEND_PARSE_PARAMETERS_END();
    
    // Get SQL query string from PDOStatement
    zval *query_string = zend_read_property(Z_OBJCE_P(getThis()), Z_OBJ_P(getThis()), "queryString", sizeof("queryString")-1, 1, NULL);
    const char *sql = NULL;
    if (query_string && Z_TYPE_P(query_string) == IS_STRING) {
        sql = Z_STRVAL_P(query_string);
    }
    
    // Call original PDOStatement::execute
    if (orig_pdo_stmt_execute_handler) {
        orig_pdo_stmt_execute_handler(execute_data, return_value);
    } else if (orig_pdo_stmt_execute_func && orig_pdo_stmt_execute_func->internal_function.handler) {
        orig_pdo_stmt_execute_func->internal_function.handler(execute_data, return_value);
    }
    
    double elapsed = (get_microtime() - start) * 1000.0;
    
    // Get row count
    long row_count = -1;
    zval row_count_zv;
    zval method_name;
    zval this_zv;
    ZVAL_OBJ(&this_zv, Z_OBJ_P(getThis()));
    ZVAL_STRING(&method_name, "rowCount");
    if (call_user_function(EG(function_table), &this_zv, &method_name, &row_count_zv, 0, NULL) == SUCCESS) {
        if (Z_TYPE(row_count_zv) == IS_LONG) {
            row_count = Z_LVAL(row_count_zv);
            
            // If rowCount is 0, it might be a SELECT query
            // Try to fetch all rows and count them
            if (row_count == 0 && sql && (strncasecmp(sql, "SELECT", 6) == 0)) {
                zval fetch_all_zv;
                zval fetch_all_method;
                ZVAL_STRING(&fetch_all_method, "fetchAll");
                if (call_user_function(EG(function_table), &this_zv, &fetch_all_method, &fetch_all_zv, 0, NULL) == SUCCESS) {
                    if (Z_TYPE(fetch_all_zv) == IS_ARRAY) {
                        row_count = zend_hash_num_elements(Z_ARRVAL(fetch_all_zv));
                    }
                    zval_ptr_dtor(&fetch_all_zv);
                }
                zval_ptr_dtor(&fetch_all_method);
            }
        }
        zval_ptr_dtor(&row_count_zv);
    }
    zval_ptr_dtor(&method_name);
    
    // Log SQL query
    if (sql) {
        
        // Send SQL query data to agent via record_sql_query
        double duration_seconds = elapsed / 1000.0;
        debug_log("[PDOStatement::execute] Recording SQL query: %s, duration=%.3fs, rows=%ld", sql ? sql : "NULL", duration_seconds, row_count);
        // Note: db_host, db_system, db_dsn extraction removed for simplicity
        record_sql_query(sql, duration_seconds, NULL, "PDOStatement::execute", row_count, NULL, NULL, NULL);
        debug_log("[PDOStatement::execute] SQL query recorded");
    }
}

// Per-call observer data structure
// Stored in hash table keyed by execute_data pointer to persist between begin/end callbacks
typedef struct _opa_observer_data {
    char *call_id;                    // Function call ID for tracking
    double start_time;                // Function start time
    double start_cpu_time;            // CPU time at start
    size_t start_memory;              // Memory usage at start
    size_t start_bytes_sent;          // Network bytes sent at start
    size_t start_bytes_received;      // Network bytes received at start
    char *sql;                        // SQL query (for PDO methods)
    double query_start_time;           // SQL query start time
    zval *curl_handle;                // cURL handle (if curl function)
    double curl_start_time;            // cURL start time
    size_t curl_bytes_sent_before;     // Bytes sent before cURL call
    size_t curl_bytes_received_before; // Bytes received before cURL call
    char *apcu_key;                   // APCu cache key
    const char *apcu_operation;        // APCu operation name
    double apcu_start_time;            // APCu start time
    int is_redis_method;               // Flag for Redis methods
    char *redis_key;                  // Redis key being operated on
    const char *redis_command;         // Redis command/method name
    double redis_start_time;           // Redis operation start time
    char *redis_host;                  // Redis connection host
    char *redis_port;                  // Redis connection port
    int is_symfony_cache_method;       // Flag for Symfony Cache methods
} opa_observer_data_t;

// Hash table to store observer data keyed by execute_data pointer
static HashTable *observer_data_table = NULL;
static pthread_mutex_t observer_data_mutex = PTHREAD_MUTEX_INITIALIZER;

// General Zend Observer callbacks for all function calls
// This is the proper way to intercept function calls in PHP 8.0+ (like xdebug)
// Signature: void (*)(zend_execute_data *)
static void opa_observer_fcall_begin(zend_execute_data *execute_data) {
    // Re-entrancy guard: if we're already inside observer, bypass to prevent infinite recursion
    // This is critical when observer callbacks trigger PHP functions (like snprintf, curl_getinfo, etc.)
    if (in_opa_observer) {
        return;
    }
    
    // Fast-path: if not actively profiling, return immediately
    if (!profiling_active) {
        return;
    }
    
    // Safety checks
    if (!execute_data || !execute_data->func || !global_collector || 
        !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Set re-entrancy guard immediately after safety checks
    in_opa_observer = 1;
    
    zend_function *func = execute_data->func;
    
    // Get function information
    const char *function_name = NULL;
    const char *class_name = NULL;
    const char *file = NULL;
    int line = 0;
    int function_type = 0;
    
    if (func->common.function_name) {
        function_name = ZSTR_VAL(func->common.function_name);
    }
    
    if (func->common.scope && func->common.scope->name) {
        class_name = ZSTR_VAL(func->common.scope->name);
    }
    
    // Determine function type
    if (func->type == ZEND_USER_FUNCTION) {
        function_type = class_name ? 2 : 0;
    } else if (func->type == ZEND_INTERNAL_FUNCTION) {
        // Skip internal functions if not collecting them
        if (!OPA_G(collect_internal_functions)) {
            in_opa_observer = 0;
            return;
        }
        function_type = class_name ? 2 : 1;
    } else {
        in_opa_observer = 0;
        return; // Unknown function type
    }
    
    // Get file and line information
    // Safety: opline can be NULL or point to invalid memory for internal functions
    // Always use zend_get_executed_lineno() as it's safer and works for all function types
    // Only use opline for user functions where we know it's safe
    if (func->type == ZEND_USER_FUNCTION && execute_data->opline) {
        line = execute_data->opline->lineno;
    } else {
        // Fallback: use executed context (safer for internal functions and when opline is invalid)
        line = zend_get_executed_lineno();
    }
    
    if (func->type == ZEND_USER_FUNCTION) {
        zend_op_array *op_array = &func->op_array;
        if (op_array && op_array->filename) {
            file = ZSTR_VAL(op_array->filename);
        }
    }
    
    // Allocate observer data structure
    opa_observer_data_t *data = emalloc(sizeof(opa_observer_data_t));
    memset(data, 0, sizeof(opa_observer_data_t));
    
    // Store start time and metrics
    data->start_time = get_time_seconds();
    data->start_cpu_time = get_cpu_time();
    data->start_memory = get_memory_usage();
    data->start_bytes_sent = get_bytes_sent();
    data->start_bytes_received = get_bytes_received();
    
    // Track function entry
    // Skip profiling curl_getinfo and curl_error when called from within observer
    // These are now called directly via internal handlers, but we still skip profiling them
    // to be safe and avoid any potential recursion
    char *call_id = NULL;
    if (function_name && (strcmp(function_name, "curl_getinfo") == 0 || strcmp(function_name, "curl_error") == 0)) {
        // Skip profiling these functions when called from observer context
        // They are now called directly via internal handlers to prevent recursion
        call_id = NULL;
    } else if (function_name || class_name) {
        call_id = opa_enter_function(function_name, class_name, file, line, function_type);
        if (call_id) {
            data->call_id = estrdup(call_id);
            efree(call_id);
        }
    }
    
    // Detect and capture cURL calls
    if (function_name && strcmp(function_name, "curl_exec") == 0) {
        data->curl_start_time = get_time_seconds();
        data->curl_bytes_sent_before = get_bytes_sent();
        data->curl_bytes_received_before = get_bytes_received();
        if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
            zval *arg = ZEND_CALL_ARG(execute_data, 1);
            if (arg && Z_TYPE_P(arg) == IS_OBJECT) {
                data->curl_handle = arg;
            }
        }
    } else if (is_curl_function(execute_data)) {
        data->curl_start_time = get_time_seconds();
        data->curl_bytes_sent_before = get_bytes_sent();
        data->curl_bytes_received_before = get_bytes_received();
        if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
            zval *arg = ZEND_CALL_ARG(execute_data, 1);
            if (arg && Z_TYPE_P(arg) == IS_OBJECT) {
                data->curl_handle = arg;
            }
        }
    }
    
    // Detect APCu functions
    if (is_apcu_function(execute_data) && function_name) {
        data->apcu_start_time = get_time_seconds();
        data->apcu_operation = function_name;
        if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
            zval *key_arg = ZEND_CALL_ARG(execute_data, 1);
            if (key_arg && Z_TYPE_P(key_arg) == IS_STRING) {
                data->apcu_key = estrdup(Z_STRVAL_P(key_arg));
            } else if (key_arg && Z_TYPE_P(key_arg) == IS_ARRAY) {
                data->apcu_key = estrdup("array");
            }
        }
    }
    
    // Detect Redis methods
    if (is_redis_method(execute_data)) {
        data->is_redis_method = 1;
        data->redis_start_time = get_time_seconds();
        data->redis_command = function_name; // Store method name as command
        
        // Extract Redis key from arguments based on method type
        if (function_name) {
            int num_args = ZEND_CALL_NUM_ARGS(execute_data);
            
            // For hash methods (hget, hset, hgetall), second arg is the key (first is hash name)
            if (strcmp(function_name, "hget") == 0 || 
                strcmp(function_name, "hset") == 0 ||
                strcmp(function_name, "hgetall") == 0) {
                if (num_args >= 2) {
                    zval *key_arg = ZEND_CALL_ARG(execute_data, 2);
                    if (key_arg && Z_TYPE_P(key_arg) == IS_STRING) {
                        data->redis_key = estrdup(Z_STRVAL_P(key_arg));
                    }
                }
            }
            // For all other methods, first arg is typically the key
            else if (num_args >= 1) {
                zval *key_arg = ZEND_CALL_ARG(execute_data, 1);
                if (key_arg && Z_TYPE_P(key_arg) == IS_STRING) {
                    data->redis_key = estrdup(Z_STRVAL_P(key_arg));
                } else if (key_arg && Z_TYPE_P(key_arg) == IS_LONG) {
                    // Some methods might pass integer keys, convert to string
                    char key_buf[32];
                    snprintf(key_buf, sizeof(key_buf), "%ld", Z_LVAL_P(key_arg));
                    data->redis_key = estrdup(key_buf);
                }
            }
            
            // For methods with no key argument (like keys with pattern), use method name
            if (!data->redis_key && function_name) {
                data->redis_key = estrdup(function_name);
            }
        }
        
        // Extract Redis connection host and port from the Redis object
        // Use execute_data->This for observer callbacks (not getThis())
        if (execute_data && Z_TYPE(execute_data->This) == IS_OBJECT) {
            zval host_zv, port_zv;
            zval method_host, method_port;
            zval this_obj;
            ZVAL_OBJ(&this_obj, Z_OBJ(execute_data->This));
            
            // Call getHost() method
            ZVAL_STRING(&method_host, "getHost");
            if (call_user_function(EG(function_table), &this_obj, &method_host, &host_zv, 0, NULL) == SUCCESS) {
                if (Z_TYPE(host_zv) == IS_STRING && Z_STRLEN(host_zv) > 0) {
                    data->redis_host = estrdup(Z_STRVAL(host_zv));
                }
                zval_ptr_dtor(&host_zv);
            }
            zval_ptr_dtor(&method_host);
            
            // Call getPort() method
            ZVAL_STRING(&method_port, "getPort");
            if (call_user_function(EG(function_table), &this_obj, &method_port, &port_zv, 0, NULL) == SUCCESS) {
                if (Z_TYPE(port_zv) == IS_LONG && Z_LVAL(port_zv) > 0) {
                    char port_buf[16];
                    snprintf(port_buf, sizeof(port_buf), "%ld", Z_LVAL(port_zv));
                    data->redis_port = estrdup(port_buf);
                }
                zval_ptr_dtor(&port_zv);
            }
            zval_ptr_dtor(&method_port);
        }
    }
    
    // Detect Symfony Cache methods
    if (is_symfony_cache_method(execute_data)) {
        data->is_symfony_cache_method = 1;
    }
    
    // Store observer data in hash table keyed by execute_data pointer
    pthread_mutex_lock(&observer_data_mutex);
    if (!observer_data_table) {
        // Use malloc instead of emalloc to prevent PHP from automatically destroying
        // the hash table during request shutdown when zvals are invalid
        observer_data_table = malloc(sizeof(HashTable));
        if (observer_data_table) {
            zend_hash_init(observer_data_table, 64, NULL, NULL, 0);
        }
    }
    // CRITICAL: Validate before accessing hash table
    if (observer_data_table && observer_data_table->nTableSize > 0) {
        // Validate execute_data pointer is reasonable
        if (execute_data && (zend_ulong)execute_data > 0x1000) {
            zval data_zv;
            ZVAL_PTR(&data_zv, data);
            zend_hash_index_update(observer_data_table, (zend_ulong)execute_data, &data_zv);
        }
    }
    pthread_mutex_unlock(&observer_data_mutex);
    
    // Reset re-entrancy guard before returning
    in_opa_observer = 0;
}

// Signature: void (*)(zend_execute_data *, zval *)
static void opa_observer_fcall_end(zend_execute_data *execute_data, zval *return_value) {
    // Re-entrancy guard: if we're already inside observer, bypass to prevent infinite recursion
    // This is critical when observer callbacks trigger PHP functions (like curl_getinfo, etc.)
    if (in_opa_observer) {
        return;
    }
    
    // Fast-path: if not actively profiling, return immediately
    if (!profiling_active) {
        return;
    }
    
    // Safety checks
    if (!execute_data || !execute_data->func || !global_collector || 
        !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return;
    }
    
    // Set re-entrancy guard immediately after safety checks
    in_opa_observer = 1;
    
    // Retrieve observer data from hash table
    // CRITICAL: Keep mutex locked during entire access to prevent race condition with RSHUTDOWN
    pthread_mutex_lock(&observer_data_mutex);
    opa_observer_data_t *data = NULL;
    
    // CRITICAL: Double-check profiling_active and table existence after acquiring lock
    // RSHUTDOWN sets profiling_active=0 BEFORE destroying the hash table, so if profiling_active
    // is 0, the hash table might be in the process of being destroyed or already destroyed
    // We MUST check this AFTER acquiring the mutex to avoid race conditions
    if (!profiling_active) {
        // Profiling was disabled by RSHUTDOWN - skip hash table access to prevent crash
        pthread_mutex_unlock(&observer_data_mutex);
        in_opa_observer = 0;
        return;
    }
    
    // Check if table exists (could be destroyed by RSHUTDOWN)
    if (!observer_data_table) {
        pthread_mutex_unlock(&observer_data_mutex);
        in_opa_observer = 0;
        return;
    }
    
    // CRITICAL: The segfault is happening inside zend_hash_index_find at a consistent offset (0x1c3e)
    // This suggests the hash table's internal structure (arData array) is corrupted
    // Even with extensive validation, zend_hash_index_find crashes when accessing arData elements
    // 
    // The root cause appears to be that zend_hash_index_find accesses the hash table's arData array
    // which might contain invalid pointers or be partially freed, even though the hash table structure
    // itself looks valid (nTableSize, nTableMask, arData pointer all look correct).
    //
    // Since we can't prevent this from C without signal handlers, and validation isn't sufficient,
    // we need to be extremely defensive. We'll skip the hash table lookup if there's ANY doubt.
    
    // Additional check: verify table still exists and profiling is still active
    // (RSHUTDOWN might have destroyed table or disabled profiling between checks)
    if (!profiling_active || !observer_data_table) {
        pthread_mutex_unlock(&observer_data_mutex);
        in_opa_observer = 0;
        return;
    }
    
    if (observer_data_table) {
        // CRITICAL: Access ALL hash table fields through the global pointer ONLY
        // Do NOT create a local copy - the memory might be freed between checks
        // Validate structure fields directly through observer_data_table pointer
        
        // Step 1: Verify pointer is reasonable (basic sanity check)
        uintptr_t ht_addr = (uintptr_t)observer_data_table;
        if (ht_addr > 0x1000 && ht_addr < 0x7fffffffffff) {
            // Step 2: Check nTableSize (first field access - if this crashes, table is definitely invalid)
            // CRITICAL: Re-check profiling_active before accessing hash table fields
            // RSHUTDOWN might have disabled profiling between the initial check and here
            if (!profiling_active) {
                // Profiling was disabled - skip hash table access
            } else {
                uint32_t table_size = 0;
                uint32_t table_mask = 0;
                void *ar_data = NULL;
                
                // Safely read hash table fields - if any access fails, we skip the lookup
                // We can't use try-catch in C, so we validate extensively before each access
                table_size = observer_data_table->nTableSize;
                if (table_size > 0 && table_size < 1000000) {
                    table_mask = observer_data_table->nTableMask;
                    if (table_mask != 0) {
                        ar_data = observer_data_table->arData;
                        if (ar_data != NULL) {
                            uintptr_t arData_addr = (uintptr_t)ar_data;
                            if (arData_addr > 0x1000 && arData_addr < 0x7fffffffffff) {
                                // Validate execute_data pointer
                                if (execute_data && (zend_ulong)execute_data > 0x1000) {
                                    // Final validation: re-check all fields are still consistent
                                    // (RSHUTDOWN might have destroyed table between field reads)
                                    // Also re-check profiling_active - it might have been disabled
                                    if (profiling_active &&
                                        observer_data_table != NULL &&
                                        observer_data_table->nTableSize == table_size &&
                                        observer_data_table->nTableMask == table_mask &&
                                        observer_data_table->arData == ar_data) {
                                        // CRITICAL: The segfault happens INSIDE zend_hash_index_find when it accesses
                                        // the hash table's arData array elements. The arData array might contain
                                        // invalid pointers or the array itself might be partially freed.
                                        // 
                                        // The crash happens at a consistent offset (0x1c3e) inside zend_hash_index_find,
                                        // suggesting the hash table structure is corrupted in a specific way.
                                        // 
                                        // Since validation isn't sufficient and we can't prevent this from C,
                                        // we'll try the lookup with all validations passed.
                                        // If it still crashes, the issue is deeper than we can fix with validation.
                                        //
                                        // FINAL CHECK: Re-verify profiling_active one more time right before the call
                                        // This is the last chance to avoid the crash
                                        if (profiling_active && observer_data_table != NULL) {
                                            zval *data_zv = zend_hash_index_find(observer_data_table, (zend_ulong)execute_data);
                                            if (data_zv && Z_TYPE_P(data_zv) == IS_PTR) {
                                                data = (opa_observer_data_t *)Z_PTR_P(data_zv);
                                                // Additional validation: check if data pointer is reasonable
                                                if (data && (uintptr_t)data < 0x1000) {
                                                    data = NULL; // Invalid pointer
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&observer_data_mutex);
    
    if (!data) {
        // Reset re-entrancy guard before returning
        in_opa_observer = 0;
        return; // No data stored, skip processing
    }
    
    zend_function *func = execute_data->func;
    const char *function_name = NULL;
    const char *class_name = NULL;
    
    if (func->common.function_name) {
        function_name = ZSTR_VAL(func->common.function_name);
    }
    if (func->common.scope && func->common.scope->name) {
        class_name = ZSTR_VAL(func->common.scope->name);
    }
    
    // Track function exit
    if (data->call_id) {
        opa_exit_function(data->call_id);
    }
    
    // Handle cURL calls
    if (data->curl_handle || (function_name && strcmp(function_name, "curl_exec") == 0)) {
        double curl_end_time = get_time_seconds();
        double curl_duration = curl_end_time - data->curl_start_time;
        size_t curl_bytes_sent_after = get_bytes_sent();
        size_t curl_bytes_received_after = get_bytes_received();
        size_t bytes_sent = curl_bytes_sent_after - data->curl_bytes_sent_before;
        size_t bytes_received = curl_bytes_received_after - data->curl_bytes_received_before;
        
        // Extract cURL information (similar to existing logic in opa_execute_ex)
        char *curl_url = NULL;
        char *curl_method = NULL;
        int status_code = 0;
        char *error = NULL;
        
        if (data->curl_handle && Z_TYPE_P(data->curl_handle) == IS_OBJECT) {
            // RE-ENABLED: Call curl_getinfo and curl_error with proper recursion prevention
            // Use re-entrancy guard to prevent observer callbacks (profiling_active is global, don't modify it)
            int old_guard = in_opa_observer;
            in_opa_observer = 1;  // Set guard - observer checks this first and returns early
            
            // Get curl_getinfo data using zend_call_function
            // With profiling_active=0 and in_opa_observer=1, observer should not trigger
            zval curl_getinfo_func_zv, curl_getinfo_args[1], curl_getinfo_ret;
            ZVAL_UNDEF(&curl_getinfo_func_zv);
            ZVAL_UNDEF(&curl_getinfo_args[0]);
            ZVAL_UNDEF(&curl_getinfo_ret);
            
            ZVAL_STRING(&curl_getinfo_func_zv, "curl_getinfo");
            ZVAL_COPY(&curl_getinfo_args[0], data->curl_handle);
            
            zend_fcall_info fci;
            zend_fcall_info_cache fcc;
            
            if (zend_fcall_info_init(&curl_getinfo_func_zv, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
                fci.param_count = 1;
                fci.params = curl_getinfo_args;
                fci.retval = &curl_getinfo_ret;
                
                if (zend_call_function(&fci, &fcc) == SUCCESS) {
                    // CRITICAL: Validate return value before accessing
                    // Check if return value is valid and is an array
                    if (Z_TYPE(curl_getinfo_ret) == IS_ARRAY && Z_ARRVAL(curl_getinfo_ret) != NULL) {
                        HashTable *ht = Z_ARRVAL(curl_getinfo_ret);
                        // Additional safety check: verify hash table is valid
                        if (ht && ht->nTableSize > 0) {
                            zval *url_val = zend_hash_str_find(ht, "url", sizeof("url") - 1);
                            if (url_val && Z_TYPE_P(url_val) == IS_STRING) {
                                curl_url = estrdup(Z_STRVAL_P(url_val));
                            }
                            
                            zval *method_val = zend_hash_str_find(ht, "request_method", sizeof("request_method") - 1);
                            if (method_val && Z_TYPE_P(method_val) == IS_STRING) {
                                curl_method = estrdup(Z_STRVAL_P(method_val));
                            } else {
                                curl_method = estrdup("GET");
                            }
                            
                            zval *status_val = zend_hash_str_find(ht, "http_code", sizeof("http_code") - 1);
                            if (status_val && Z_TYPE_P(status_val) == IS_LONG) {
                                status_code = Z_LVAL_P(status_val);
                            }
                        }
                    }
                    // Always destroy the return value, even if invalid
                    if (Z_TYPE(curl_getinfo_ret) != IS_UNDEF) {
                        zval_dtor(&curl_getinfo_ret);
                    }
                }
                zval_dtor(&curl_getinfo_func_zv);
                zval_dtor(&curl_getinfo_args[0]);
            }
            
            // Get curl_error
            zval curl_error_func_zv, curl_error_args[1], curl_error_ret;
            ZVAL_UNDEF(&curl_error_func_zv);
            ZVAL_UNDEF(&curl_error_args[0]);
            ZVAL_UNDEF(&curl_error_ret);
            
            ZVAL_STRING(&curl_error_func_zv, "curl_error");
            ZVAL_COPY(&curl_error_args[0], data->curl_handle);
            
            if (zend_fcall_info_init(&curl_error_func_zv, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
                fci.param_count = 1;
                fci.params = curl_error_args;
                fci.retval = &curl_error_ret;
                
                if (zend_call_function(&fci, &fcc) == SUCCESS) {
                    // CRITICAL: Validate return value before accessing
                    if (Z_TYPE(curl_error_ret) == IS_STRING && Z_STRVAL(curl_error_ret) != NULL) {
                        if (Z_STRLEN(curl_error_ret) > 0) {
                            error = estrdup(Z_STRVAL(curl_error_ret));
                        }
                    }
                    // Always destroy the return value, even if invalid
                    if (Z_TYPE(curl_error_ret) != IS_UNDEF) {
                        zval_dtor(&curl_error_ret);
                    }
                }
                zval_dtor(&curl_error_func_zv);
                zval_dtor(&curl_error_args[0]);
            }
            
            // Restore guard
            in_opa_observer = old_guard;
        }
        
        // Record HTTP request
        record_http_request(curl_url ? curl_url : "unknown", curl_method ? curl_method : "GET", 
                          status_code, bytes_sent, bytes_received, curl_duration, error);
        
        if (curl_url) efree(curl_url);
        if (curl_method) efree(curl_method);
        if (error) efree(error);
    }
    
    // Handle APCu cache operations
    if (data->apcu_operation) {
        double apcu_end_time = get_time_seconds();
        double apcu_duration = apcu_end_time - data->apcu_start_time;
        int hit = 0;
        size_t data_size = 0;
        
        // Determine hit/miss based on return value and operation
        if (strcmp(data->apcu_operation, "apcu_fetch") == 0) {
            if (return_value && Z_TYPE_P(return_value) != IS_FALSE) {
                hit = 1;
                if (Z_TYPE_P(return_value) == IS_STRING) {
                    data_size = Z_STRLEN_P(return_value);
                } else if (Z_TYPE_P(return_value) == IS_ARRAY) {
                    data_size = zend_hash_num_elements(Z_ARRVAL_P(return_value)) * 100; // Estimate
                }
            }
        } else if (strcmp(data->apcu_operation, "apcu_store") == 0 || 
                   strcmp(data->apcu_operation, "apcu_add") == 0) {
            hit = 1; // Store/add is always a "hit" (successful operation)
        } else if (strcmp(data->apcu_operation, "apcu_delete") == 0 ||
                   strcmp(data->apcu_operation, "apcu_clear_cache") == 0) {
            hit = 1; // Delete/clear is always a "hit"
        }
        
        record_cache_operation(data->apcu_key, data->apcu_operation, hit, apcu_duration, data_size, "apcu");
    }
    
    // Handle Redis operations
    if (data->is_redis_method) {
        double redis_end_time = get_time_seconds();
        double redis_duration = redis_end_time - data->redis_start_time;
        int hit = 0;
        const char *error = NULL;
        
        // Determine hit/miss and extract error based on return value and command
        if (data->redis_command) {
            // For get operations: hit = 1 if return value is not FALSE
            if (strcmp(data->redis_command, "get") == 0 || strcmp(data->redis_command, "hget") == 0) {
                if (return_value && Z_TYPE_P(return_value) != IS_FALSE) {
                    hit = 1;
                } else {
                    error = "Key not found";
                }
            }
            // For exists: hit = 1 if return value > 0
            else if (strcmp(data->redis_command, "exists") == 0) {
                if (return_value && Z_TYPE_P(return_value) == IS_LONG && Z_LVAL_P(return_value) > 0) {
                    hit = 1;
                }
            }
            // For del: hit = 1 if return value > 0
            else if (strcmp(data->redis_command, "del") == 0 || strcmp(data->redis_command, "delete") == 0) {
                if (return_value && Z_TYPE_P(return_value) == IS_LONG && Z_LVAL_P(return_value) > 0) {
                    hit = 1;
                } else {
                    error = "Key not found or deletion failed";
                }
            }
            // For set, hset, lpush, sadd, incr, decr, expire: hit = 1 (always successful if no exception)
            else if (strcmp(data->redis_command, "set") == 0 ||
                     strcmp(data->redis_command, "hset") == 0 ||
                     strcmp(data->redis_command, "lpush") == 0 ||
                     strcmp(data->redis_command, "sadd") == 0 ||
                     strcmp(data->redis_command, "incr") == 0 ||
                     strcmp(data->redis_command, "decr") == 0 ||
                     strcmp(data->redis_command, "expire") == 0) {
                if (return_value && Z_TYPE_P(return_value) != IS_FALSE) {
                    hit = 1;
                } else {
                    error = "Operation failed";
                }
            }
            // For operations returning arrays (hgetall, smembers): hit = 1 if array is not empty
            else if (strcmp(data->redis_command, "hgetall") == 0 ||
                     strcmp(data->redis_command, "smembers") == 0) {
                if (return_value && Z_TYPE_P(return_value) == IS_ARRAY) {
                    HashTable *ht = Z_ARRVAL_P(return_value);
                    if (ht && zend_hash_num_elements(ht) > 0) {
                        hit = 1;
                    }
                }
            }
            // For operations returning integers (llen, scard, ttl): hit = 1 if return value >= 0
            else if (strcmp(data->redis_command, "llen") == 0 ||
                     strcmp(data->redis_command, "scard") == 0 ||
                     strcmp(data->redis_command, "ttl") == 0) {
                if (return_value && Z_TYPE_P(return_value) == IS_LONG) {
                    hit = 1; // Always record as hit for these operations
                }
            }
            // For rpop: hit = 1 if return value is not FALSE
            else if (strcmp(data->redis_command, "rpop") == 0) {
                if (return_value && Z_TYPE_P(return_value) != IS_FALSE) {
                    hit = 1;
                } else {
                    error = "List empty or operation failed";
                }
            }
            // For keys: hit = 1 if return value is an array
            else if (strcmp(data->redis_command, "keys") == 0) {
                if (return_value && Z_TYPE_P(return_value) == IS_ARRAY) {
                    hit = 1;
                }
            }
            // Default: check if return value indicates success
            else {
                if (return_value && Z_TYPE_P(return_value) != IS_FALSE) {
                    hit = 1;
                } else {
                    error = "Operation failed";
                }
            }
        }
        
        // Record Redis operation
        record_redis_operation(data->redis_command, data->redis_key, hit, redis_duration, error, data->redis_host, data->redis_port);
    }
    
    // Clean up observer data
    if (data->call_id) efree(data->call_id);
    if (data->sql) efree(data->sql);
    if (data->apcu_key) efree(data->apcu_key);
    if (data->redis_key) efree(data->redis_key);
    if (data->redis_host) efree(data->redis_host);
    if (data->redis_port) efree(data->redis_port);
    
    // Remove from hash table and free
    // CRITICAL: Add same extensive validation as in the find operation
    // Use local pointer to avoid race condition with RSHUTDOWN
    pthread_mutex_lock(&observer_data_mutex);
    HashTable *ht_del = observer_data_table; // Local copy to avoid race condition
    if (ht_del) {
        // CRITICAL: Extensive validation to prevent segfaults in zend_hash_index_del
        // Check if hash table is properly initialized
        if (ht_del->nTableSize > 0 && 
            ht_del->nTableMask != 0 &&
            ht_del->arData != NULL) {
            
            // Additional validation: check arData pointer is reasonable
            uintptr_t arData_addr = (uintptr_t)ht_del->arData;
            if (arData_addr > 0x1000 && arData_addr < 0x7fffffffffff) {
                // Validate execute_data pointer is reasonable
                if (execute_data && (zend_ulong)execute_data > 0x1000) {
                    // Double-check table still exists (could be destroyed by RSHUTDOWN)
                    if (observer_data_table == ht_del) {
                        // Now safe to call zend_hash_index_del
                        zend_hash_index_del(ht_del, (zend_ulong)execute_data);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&observer_data_mutex);
    
    efree(data);
    
    // Reset re-entrancy guard before returning
    in_opa_observer = 0;
}

// Observer initialization function for all function calls
// Returns handlers structure to register callbacks
// Signature: zend_observer_fcall_handlers (*)(zend_execute_data *)
static zend_observer_fcall_handlers opa_observer_fcall_init(zend_execute_data *execute_data) {
    zend_observer_fcall_handlers handlers = {0};
    
    // Fast-path: if not actively profiling, don't register handlers
    if (!profiling_active) {
        return handlers;
    }
    
    // Safety checks
    if (!execute_data || !execute_data->func || !global_collector || 
        !global_collector->active || global_collector->magic != OPA_COLLECTOR_MAGIC) {
        return handlers;
    }
    
    zend_function *func = execute_data->func;
    
    // Skip internal functions if not collecting them
    if (func->type == ZEND_INTERNAL_FUNCTION && !OPA_G(collect_internal_functions)) {
        return handlers;
    }
    
    // Register handlers for all functions (user and internal if enabled)
    handlers.begin = opa_observer_fcall_begin;
    handlers.end = opa_observer_fcall_end;
    
    return handlers;
}

// Zend Observer callbacks for PDO methods
// This is the proper way to intercept PDO calls in PHP 8.0+
// Signature: void (*)(zend_execute_data *)
static void opa_observer_pdo_fcall_begin(zend_execute_data *execute_data) {
    // Fast-path: if not actively profiling, return immediately
    if (!profiling_active) {
        return;
    }
    
    // Called before PDO method execution
    if (!execute_data || !execute_data->func) {
        return;
    }
    
    zend_function *func = execute_data->func;
    const char *class_name = NULL;
    const char *method_name = NULL;
    
    if (func->common.scope && func->common.scope->name) {
        class_name = ZSTR_VAL(func->common.scope->name);
    }
    if (func->common.function_name) {
        method_name = ZSTR_VAL(func->common.function_name);
    }
    
    if (class_name && (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0)) {
        if (method_name && (strcmp(method_name, "query") == 0 || strcmp(method_name, "exec") == 0 || 
                           strcmp(method_name, "prepare") == 0 || strcmp(method_name, "execute") == 0)) {
        }
    }
}

// Store start time for each PDO call (keyed by execute_data pointer)
static HashTable *pdo_call_times = NULL;

// Signature: void (*)(zend_execute_data *, zval *)
static void opa_observer_pdo_fcall_end(zend_execute_data *execute_data, zval *return_value) {
    // Fast-path: if not actively profiling, return immediately
    if (!profiling_active) {
        return;
    }
    
    // Called after PDO method execution
    if (!execute_data || !execute_data->func) {
        return;
    }
    
    zend_function *func = execute_data->func;
    const char *class_name = NULL;
    const char *method_name = NULL;
    char *sql = NULL;
    double start_time = 0.0;
    double duration = 0.0;
    
    if (func->common.scope && func->common.scope->name) {
        class_name = ZSTR_VAL(func->common.scope->name);
    }
    if (func->common.function_name) {
        method_name = ZSTR_VAL(func->common.function_name);
    }
    
    if (class_name && (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0)) {
        if (method_name && (strcmp(method_name, "query") == 0 || strcmp(method_name, "exec") == 0 || 
                           strcmp(method_name, "prepare") == 0 || strcmp(method_name, "execute") == 0)) {
            
            // Calculate duration (approximate - we don't have precise start time from begin callback)
            // For more accurate timing, we'd need to store start time in begin callback
            duration = 0.001; // Default small duration, can be improved with timing storage
            
            // Extract SQL
            if (strcmp(method_name, "query") == 0 || strcmp(method_name, "exec") == 0 || strcmp(method_name, "prepare") == 0) {
                if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
                    zval *arg = ZEND_CALL_ARG(execute_data, 1);
                    if (arg && Z_TYPE_P(arg) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(arg));
                    }
                }
            } else if (strcmp(method_name, "execute") == 0 && strcmp(class_name, "PDOStatement") == 0) {
                // For PDOStatement::execute, get SQL from the statement object
                // Access execute_data->This safely (it's available in observer callbacks)
                if (execute_data && Z_TYPE(execute_data->This) == IS_OBJECT) {
                    zval *query_string_prop = zend_read_property(Z_OBJCE(execute_data->This), Z_OBJ(execute_data->This), "queryString", sizeof("queryString") - 1, 1, NULL);
                    if (query_string_prop && Z_TYPE_P(query_string_prop) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(query_string_prop));
                    }
                }
            }
            
            if (sql) {
                long rows_affected = -1;
                
                // Get row count safely - only for exec() which returns integer directly
                if (return_value && Z_TYPE_P(return_value) == IS_LONG && strcmp(method_name, "exec") == 0) {
                    rows_affected = Z_LVAL_P(return_value);
                }
                // For query() and prepare(), we can't safely get row count from observer callback
                // without risking heap corruption, so leave it as -1
                
                // Record SQL query using record_sql_query
                char query_type_str[64];
                snprintf(query_type_str, sizeof(query_type_str), "PDO::%s", method_name);
                
                
                // Ensure collector is initialized and active (should already be done in RINIT)
                if (!global_collector || global_collector->magic != OPA_COLLECTOR_MAGIC || !global_collector->active) {
                    if (!global_collector) {
                        global_collector = opa_collector_init();
                    }
                    if (global_collector && global_collector->magic == OPA_COLLECTOR_MAGIC) {
                        opa_collector_start(global_collector);
                        // Initialize global SQL queries array
                        pthread_mutex_lock(&global_collector->global_sql_mutex);
                        if (!global_collector->global_sql_queries) {
                            global_collector->global_sql_queries = ecalloc(1, sizeof(zval));
                            if (global_collector->global_sql_queries) {
                                array_init(global_collector->global_sql_queries);
                            }
                        }
                        pthread_mutex_unlock(&global_collector->global_sql_mutex);
                    }
                }
                
                // Use record_sql_query for consistent SQL recording
                // Note: db_host, db_system, db_dsn are NULL for now - can be extracted from PDO connection if needed
                if (global_collector && global_collector->magic == OPA_COLLECTOR_MAGIC && global_collector->active) {
                    record_sql_query(sql, duration, NULL, query_type_str, rows_affected, NULL, NULL, NULL);
                }
                
                efree(sql);
            }
        }
    }
}

// Observer initialization function - called for each function call
// Returns handlers structure to register callbacks
// Signature: zend_observer_fcall_handlers (*)(zend_execute_data *)
static zend_observer_fcall_handlers opa_observer_pdo_init(zend_execute_data *execute_data) {
    zend_observer_fcall_handlers handlers = {0};
    
    // Fast-path: if not actively profiling, don't register handlers
    if (!profiling_active) {
        return handlers;
    }
    
    // Only observe PDO and PDOStatement methods
    if (execute_data && execute_data->func) {
        zend_function *func = execute_data->func;
        const char *class_name = NULL;
        const char *method_name = NULL;
        
        if (func->common.scope && func->common.scope->name) {
            class_name = ZSTR_VAL(func->common.scope->name);
        }
        if (func->common.function_name) {
            method_name = ZSTR_VAL(func->common.function_name);
        }
        
        if (class_name && (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0)) {
            if (method_name && (strcmp(method_name, "query") == 0 || strcmp(method_name, "exec") == 0 || 
                               strcmp(method_name, "prepare") == 0 || strcmp(method_name, "execute") == 0)) {
                handlers.begin = opa_observer_pdo_fcall_begin;
                handlers.end = opa_observer_pdo_fcall_end;
            }
        }
    }
    
    return handlers;
}

PHP_MINIT_FUNCTION(opa) {
    REGISTER_INI_ENTRIES();
    
    // TEMPORARILY DISABLED: Auto-detect PHP version (uses OPA_G which crashes)
    /*
    if (!OPA_G(language_version) || strlen(OPA_G(language_version)) == 0) {
        const char *php_version = PHP_VERSION;
        char *version_copy = estrdup(php_version);
        char *first_dot = strchr(version_copy, '.');
        if (first_dot) {
            char *second_dot = strchr(first_dot + 1, '.');
            if (second_dot) {
                *second_dot = '\0';
            }
        }
        if (OPA_G(language_version)) {
            efree(OPA_G(language_version));
        }
        OPA_G(language_version) = version_copy;
    }
    */
    
    // NOTE: zend_execute_ex hook is DISABLED - we now use Observer API instead
    // The Observer API is the recommended approach in PHP 8.0+ and eliminates recursion issues
    // Keeping original_zend_execute_ex variable for potential fallback, but not hooking it
    // if (zend_execute_ex && !original_zend_execute_ex) {
    //     original_zend_execute_ex = zend_execute_ex;
    //     zend_execute_ex = opa_execute_ex;
    // }
    
    // Register Zend Observer for PDO methods
    // This is the proper way to intercept PDO calls in PHP 8.0+ (bypasses handler replacement issues)
    if (!pdo_observer_registered) {
        // zend_observer_fcall_register takes a function pointer of type zend_observer_fcall_init
        // The function signature must match: zend_observer_fcall (*)(zend_execute_data *)
        zend_observer_fcall_register(opa_observer_pdo_init);
        pdo_observer_registered = 1;
        
        // NOTE: pdo_call_times hash table is initialized but never actually used
        // It was intended for storing call start times but the implementation was never completed
        // Keeping it for now but it's safe to skip initialization
        // if (!pdo_call_times) {
        //     pdo_call_times = emalloc(sizeof(HashTable));
        //     zend_hash_init(pdo_call_times, 8, NULL, NULL, 0);
        // }
    }
    
    // Register general Zend Observer for all function calls
    // This replaces the zend_execute_ex hook and eliminates recursion issues
    if (!general_observer_registered) {
        zend_observer_fcall_register(opa_observer_fcall_init);
        general_observer_registered = 1;
    }
    
    // Register SQL profiling hooks (lazy registration - try in RINIT as well)
    // MySQLi hook
    orig_mysqli_query_func = zend_hash_str_find_ptr(CG(function_table), "mysqli_query", sizeof("mysqli_query")-1);
    if (orig_mysqli_query_func && orig_mysqli_query_func->type == ZEND_INTERNAL_FUNCTION) {
        // Store original handler
        orig_mysqli_query_handler = orig_mysqli_query_func->internal_function.handler;
        // Replace with our handler
        orig_mysqli_query_func->internal_function.handler = zif_opaphp_mysqli_query;
        if (OPA_G(debug_log_enabled)) {
        }
    }
    
    // PDO hooks are now handled via Zend Observer API (registered above)
    // No need for handler replacement - Observer API intercepts all calls
    
    // PHP 8.4: Get CurlHandle class entry pointers for reliable detection
    // NOTE: EG(class_table) is NULL at MINIT time in PHP 8.4, so we defer lookup to RINIT
    // Initialize pointers to NULL - will be set in RINIT when class_table is available
    curl_ce = NULL;
    curl_multi_ce = NULL;
    curl_share_ce = NULL;
    
    // Hook curl_exec directly at MINIT time
    zend_function *curl_func = zend_hash_str_find_ptr(CG(function_table), "curl_exec", sizeof("curl_exec")-1);
    if (curl_func && curl_func->type == ZEND_INTERNAL_FUNCTION) {
        orig_curl_exec_func = curl_func;
        // Store original handler BEFORE replacing it
        orig_curl_exec_handler = curl_func->internal_function.handler;
        // Replace handler with our wrapper
        curl_func->internal_function.handler = zif_opa_curl_exec;
        debug_log("[MINIT] Hooked curl_exec: orig handler=%p", orig_curl_exec_handler);
    } else {
        debug_log("[MINIT] curl_exec not found or not internal");
    }
    
    // Store curl_getinfo and curl_error function pointers for direct calls (bypasses observers)
    curl_getinfo_func = zend_hash_str_find_ptr(CG(function_table), "curl_getinfo", sizeof("curl_getinfo")-1);
    curl_error_func = zend_hash_str_find_ptr(CG(function_table), "curl_error", sizeof("curl_error")-1);
    if (curl_getinfo_func && curl_getinfo_func->type == ZEND_INTERNAL_FUNCTION) {
        debug_log("[MINIT] Found curl_getinfo function");
    }
    if (curl_error_func && curl_error_func->type == ZEND_INTERNAL_FUNCTION) {
        debug_log("[MINIT] Found curl_error function");
    }
    
    // Initialize error and log tracking
    opa_init_error_tracking();
    
    return SUCCESS;
}

// Destructor callback for span hash table entries
// Called automatically by zend_hash_destroy to free span memory
// Not used during MSHUTDOWN (null_span_dtor is used instead to avoid zval access issues)
static int free_span_dtor(zval *zv) {
    // Check if valid before accessing
    if (!zv || Z_TYPE_P(zv) != IS_PTR) {
        return ZEND_HASH_APPLY_REMOVE;
    }
    
    span_context_t *span = Z_PTR_P(zv);
    if (span) {
        free_span_context(span);
    }
    return ZEND_HASH_APPLY_REMOVE;
}

// Null destructor used during MSHUTDOWN to safely clear hash table without accessing zvals
// PHP's memory manager is shutting down, so we can't safely free zval data
static int null_span_dtor(zval *zv) {
    return ZEND_HASH_APPLY_REMOVE;
}

// Module shutdown - restores original zend_execute_ex hook and cleans up resources
PHP_MSHUTDOWN_FUNCTION(opa) {
    // Cleanup PDO call times hash table
    // NOTE: pdo_call_times is never actually used (initialization was commented out)
    // But if it was somehow initialized, clean it up safely
    if (pdo_call_times) {
        // Since it uses NULL destructor, it's safe to destroy
        zend_hash_destroy(pdo_call_times);
        efree(pdo_call_times);
        pdo_call_times = NULL;
    }
    // Restore original zend_execute_ex hook to prevent issues during module unload
    if (original_zend_execute_ex) {
        zend_execute_ex = original_zend_execute_ex;
        original_zend_execute_ex = NULL;
    }
    
    // Restore original SQL profiling handlers
    if (orig_mysqli_query_func && orig_mysqli_query_handler) {
        orig_mysqli_query_func->internal_function.handler = orig_mysqli_query_handler;
        orig_mysqli_query_func = NULL;
        orig_mysqli_query_handler = NULL;
    }
    
    if (orig_pdo_query_func && orig_pdo_query_handler) {
        orig_pdo_query_func->internal_function.handler = orig_pdo_query_handler;
        orig_pdo_query_func = NULL;
        orig_pdo_query_handler = NULL;
    }
    
    if (orig_pdo_stmt_execute_func && orig_pdo_stmt_execute_handler) {
        orig_pdo_stmt_execute_func->internal_function.handler = orig_pdo_stmt_execute_handler;
        orig_pdo_stmt_execute_func = NULL;
        orig_pdo_stmt_execute_handler = NULL;
    }
    
    if (orig_curl_exec_func && orig_curl_exec_handler) {
        orig_curl_exec_func->internal_function.handler = orig_curl_exec_handler;
        orig_curl_exec_func = NULL;
        orig_curl_exec_handler = NULL;
    }
    
    // During MSHUTDOWN, the Zend heap is being destroyed
    // PHP will try to destroy the hash table automatically via zend_hash_graceful_reverse_destroy
    // The hash table should have been destroyed in RSHUTDOWN, but if it wasn't,
    // we cannot safely destroy it here because zvals are invalid
    
    // The best approach is to ensure the hash table is NULL before MSHUTDOWN
    // If it's not NULL, we just set it to NULL and let PHP handle the cleanup
    // (though this may cause a memory leak, it's better than a crash)
    
    if (active_spans) {
        pthread_mutex_lock(&active_spans_mutex);
        // Just set to NULL - the hash table structure will leak, but it's better than crashing
        // The spans inside were already freed in RSHUTDOWN (or will be cleaned up by PHP)
        active_spans = NULL;
        pthread_mutex_unlock(&active_spans_mutex);
    }
    
    // Free collector if still exists (should have been freed in RSHUTDOWN)
    if (global_collector) {
        opa_collector_free(global_collector);
        global_collector = NULL;
    }
    
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

// Helper function to update INI setting from environment variable
// This allows environment variables to override INI settings at runtime
static void update_ini_from_env(const char *env_name, const char *ini_name) {
    char *env_value = getenv(env_name);
    if (env_value && strlen(env_value) > 0) {
        zend_string *ini_key = zend_string_init(ini_name, strlen(ini_name), 0);
        zend_string *ini_value = zend_string_init(env_value, strlen(env_value), 0);
        
        // Use zend_alter_ini_entry to update the INI setting
        // This will trigger the OnUpdate handler which updates the global
        zend_alter_ini_entry(ini_key, ini_value, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
        
        zend_string_release(ini_key);
        zend_string_release(ini_value);
        
        // Note: We can't use OPA_G(debug_log_enabled) here because it might not be set yet
        // The debug log will be shown if debug_log is enabled after this override
    }
}

PHP_RINIT_FUNCTION(opa) {
    // RINIT: Reset per-request state AND register hooks (PDO classes available by RINIT time)
    // Following user guidance: Move PDO hook registration to RINIT where classes are available
    
    // Initialize observer data hash table for this request
    // CRITICAL: Check if table was already destroyed by RSHUTDOWN
    pthread_mutex_lock(&observer_data_mutex);
    if (!observer_data_table) {
        // Use malloc instead of emalloc to prevent PHP from automatically destroying
        // the hash table during request shutdown when zvals are invalid
        observer_data_table = malloc(sizeof(HashTable));
        if (observer_data_table) {
            zend_hash_init(observer_data_table, 64, NULL, NULL, 0);
        }
    } else {
        // Table exists - verify it's still valid (not destroyed)
        // If it was destroyed, it might have invalid structure
        if (observer_data_table->nTableSize == 0 || observer_data_table->arData == NULL) {
            // Table is corrupted or destroyed - reinitialize it
            if (observer_data_table->arData != NULL) {
                zend_hash_destroy(observer_data_table);
            }
            free(observer_data_table);
            observer_data_table = malloc(sizeof(HashTable));
            if (observer_data_table) {
                zend_hash_init(observer_data_table, 64, NULL, NULL, 0);
            }
        }
    }
    pthread_mutex_unlock(&observer_data_mutex);
    
    // Try to register SQL hooks lazily if they weren't found at MINIT
    if (!orig_mysqli_query_func) {
        orig_mysqli_query_func = zend_hash_str_find_ptr(CG(function_table), "mysqli_query", sizeof("mysqli_query")-1);
        if (orig_mysqli_query_func && orig_mysqli_query_func->type == ZEND_INTERNAL_FUNCTION) {
            orig_mysqli_query_handler = orig_mysqli_query_func->internal_function.handler;
            orig_mysqli_query_func->internal_function.handler = zif_opaphp_mysqli_query;
        }
    }
    
    // PDO hooks are now registered via Zend Observer API in MINIT
    // No need to do handler replacement here - Observer API handles it
    
    // Verify PDO class is available (for debugging)
    zend_class_entry *pdo_ce = NULL;
    if (CG(class_table)) {
        pdo_ce = zend_hash_str_find_ptr(CG(class_table), "PDO", sizeof("PDO")-1);
        // PDO class check - not needed for Observer API but kept for reference
    }
    
    // Look up curl class entries at RINIT time when class_table is available
    if (!curl_ce && EG(class_table)) {
        curl_ce = zend_hash_str_find_ptr(EG(class_table), "CurlHandle", sizeof("CurlHandle")-1);
        if (curl_ce) {
        }
    }

    if (!curl_multi_ce && EG(class_table)) {
        curl_multi_ce = zend_hash_str_find_ptr(EG(class_table), "CurlMultiHandle", sizeof("CurlMultiHandle")-1);
        if (curl_multi_ce) {
        }
    }

    if (!curl_share_ce && EG(class_table)) {
        curl_share_ce = zend_hash_str_find_ptr(EG(class_table), "CurlShareHandle", sizeof("CurlShareHandle")-1);
        if (curl_share_ce) {
        }
    }
    
    
    // Reset per-request state
    // Check if profiling should be enabled
    // OPA_ENABLE environment variable overrides INI setting for both CLI and web modes
    // This allows on-the-fly profiling via environment variables
    char *opa_enable_env = getenv("OPA_ENABLE");
    if (opa_enable_env) {
        // Check for "1" or case-insensitive "true"
        if (strcmp(opa_enable_env, "1") == 0 || 
            strcmp(opa_enable_env, "true") == 0 ||
            strcmp(opa_enable_env, "TRUE") == 0 ||
            strcmp(opa_enable_env, "True") == 0) {
            // OPA_ENABLE=1 or OPA_ENABLE=true: enable profiling
            profiling_active = 1;
            if (OPA_G(debug_log_enabled)) {
                const char *mode = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0) ? "CLI" : "Web";
                debug_log("[RINIT] Profiling enabled via OPA_ENABLE environment variable (value: %s, mode: %s)", 
                    opa_enable_env, mode);
            }
        } else {
            // OPA_ENABLE set to false/0: disable profiling (overrides INI)
            profiling_active = 0;
            if (OPA_G(debug_log_enabled)) {
                const char *mode = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0) ? "CLI" : "Web";
                debug_log("[RINIT] Profiling disabled via OPA_ENABLE=%s (overrides INI setting, mode: %s)", 
                    opa_enable_env, mode);
            }
        }
    } else {
        // OPA_ENABLE not set: use INI setting (which may have been overridden by OPA_ENABLED env var)
        profiling_active = OPA_G(enabled) ? 1 : 0;
        if (OPA_G(debug_log_enabled)) {
            const char *mode = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0) ? "CLI" : "Web";
            debug_log("[RINIT] Profiling: OPA_ENABLE not set, using INI setting: %d (mode: %s)", 
                OPA_G(enabled), mode);
        }
    }
    
    // Only initialize collector if profiling is active (after profiling_active is set)
    if (profiling_active) {
        // Set memory_limit to -1 (unlimited) when profiling is enabled
        // This prevents memory exhaustion during profiling operations
        zend_string *memory_limit_key = zend_string_init("memory_limit", sizeof("memory_limit") - 1, 0);
        zend_string *memory_limit_value = zend_string_init("-1", sizeof("-1") - 1, 0);
        zend_alter_ini_entry(memory_limit_key, memory_limit_value, PHP_INI_USER, PHP_INI_STAGE_RUNTIME);
        zend_string_release(memory_limit_key);
        zend_string_release(memory_limit_value);
        
        if (OPA_G(debug_log_enabled)) {
            debug_log("[RINIT] Set memory_limit to -1 (unlimited) for profiling");
        }
        
        // Pre-resolve agent address in RINIT (before observer callbacks) to avoid DNS calls from unsafe contexts
        pre_resolve_agent_address();
        
        // Initialize and start collector EARLY in RINIT (before any PDO calls)
        // This ensures the collector is ready when observer callbacks try to record SQL
        if (!global_collector) {
            global_collector = opa_collector_init();
        }
        if (global_collector && global_collector->magic == OPA_COLLECTOR_MAGIC) {
            opa_collector_start(global_collector);
            
            // Initialize global SQL queries array
            pthread_mutex_lock(&global_collector->global_sql_mutex);
            if (global_collector->global_sql_queries) {
                zval_ptr_dtor(global_collector->global_sql_queries);
                efree(global_collector->global_sql_queries);
            }
            global_collector->global_sql_queries = ecalloc(1, sizeof(zval));
            if (global_collector->global_sql_queries) {
                array_init(global_collector->global_sql_queries);
            }
            pthread_mutex_unlock(&global_collector->global_sql_mutex);
        }
    }
    
    network_bytes_sent_total = 0;
    network_bytes_received_total = 0;
    
    // Create root span for this request (per-request state)
    // IMPORTANT: In Apache mod_php with threads, we MUST reset root span data for EACH request
    // because multiple requests can run in parallel and share the same global variables
    pthread_mutex_lock(&root_span_data_mutex);
    
    // Always reset root span data for new request (don't reuse from previous request)
    if (root_span_span_id) {
        free(root_span_span_id);
        root_span_span_id = NULL;
    }
    if (root_span_trace_id) {
        free(root_span_trace_id);
        root_span_trace_id = NULL;
    }
    if (root_span_name) {
        free(root_span_name);
        root_span_name = NULL;
    }
    if (root_span_url_path) {
        free(root_span_url_path);
        root_span_url_path = NULL;
    }
    if (root_span_http_request_json) {
        free(root_span_http_request_json);
        root_span_http_request_json = NULL;
    }
    if (root_span_cli_args_json) {
        free(root_span_cli_args_json);
        root_span_cli_args_json = NULL;
    }
    
    // Create new root span for this request
    root_span_span_id = strdup(generate_id());
    root_span_trace_id = strdup(generate_id());
    root_span_start_ts = get_timestamp_ms();
    root_span_cpu_ms = 0;
    root_span_status = -1;
    
    // PERFECTLY SAFE - Capture HTTP request info using SG(request_info) only
    // This is the safest approach - no zval access, no $_SERVER superglobals
    int is_cli = 0;
    if (sapi_module.name && strcmp(sapi_module.name, "cli") == 0) {
        is_cli = 1;
    }
    
    debug_log("[RINIT] Checking HTTP request: is_cli=%d, request_method=%s, request_uri=%s", 
        is_cli, 
        SG(request_info).request_method ? SG(request_info).request_method : "NULL",
        SG(request_info).request_uri ? SG(request_info).request_uri : "NULL");
    
    // Try to capture HTTP request - check both PG(http_globals) and SG(request_info)
    if (!is_cli) {
            // HTTP request - use universal serializer (tries PG first for FPM, then SG for Apache)
            char *req_info = serialize_http_request_json_universal();
            
            debug_log("[RINIT] serialize_http_request_json_universal() returned: %p, content=%.200s", req_info, req_info ? req_info : "NULL");
            
            if (req_info && strlen(req_info) > 2) {
                // Free old value if exists
                if (root_span_http_request_json) {
                    free(root_span_http_request_json);
                }
                root_span_http_request_json = req_info;
                debug_log("[RINIT] root_span_http_request_json set to: %p, content=%.200s", root_span_http_request_json, root_span_http_request_json ? root_span_http_request_json : "NULL");
                
                // Try to extract method and URI from the JSON or use SG(request_info) as fallback
                const char *method = SG(request_info).request_method ? SG(request_info).request_method : "GET";
                const char *uri = SG(request_info).request_uri ? SG(request_info).request_uri : "/";
                
                // Also try PG(http_globals) for method/URI if SG doesn't have them
                // For Symfony and other frameworks with front controllers, prefer PATH_INFO over REQUEST_URI
                zval *server = &PG(http_globals)[TRACK_VARS_SERVER];
                if (server && Z_TYPE_P(server) == IS_ARRAY) {
                    zval *method_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
                    if (method_zv && Z_TYPE_P(method_zv) == IS_STRING) {
                        method = Z_STRVAL_P(method_zv);
                    }
                    
                    // Prefer PATH_INFO for Symfony/frameworks (actual route without front controller)
                    // Fallback to REQUEST_URI if PATH_INFO not available
                    zval *path_info_zv = zend_hash_str_find(Z_ARRVAL_P(server), "PATH_INFO", sizeof("PATH_INFO")-1);
                    if (path_info_zv && Z_TYPE_P(path_info_zv) == IS_STRING && Z_STRLEN_P(path_info_zv) > 0) {
                        uri = Z_STRVAL_P(path_info_zv);
                        debug_log("[RINIT] Using PATH_INFO for URI: %s", uri);
                    } else {
                        zval *uri_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
                        if (uri_zv && Z_TYPE_P(uri_zv) == IS_STRING) {
                            uri = Z_STRVAL_P(uri_zv);
                            // Remove query string from REQUEST_URI if present
                            char *query_start = strchr(uri, '?');
                            if (query_start) {
                                size_t uri_len = query_start - uri;
                                char *clean_uri = malloc(uri_len + 1);
                                if (clean_uri) {
                                    memcpy(clean_uri, uri, uri_len);
                                    clean_uri[uri_len] = '\0';
                                    // Try to remove /index.php prefix if present
                                    if (strncmp(clean_uri, "/index.php", 10) == 0) {
                                        if (clean_uri[10] == '\0') {
                                            uri = "/";
                                        } else {
                                            uri = clean_uri + 10;
                                        }
                                    } else {
                                        uri = clean_uri;
                                    }
                                }
                            } else {
                                // Remove /index.php prefix if present
                                if (strncmp(uri, "/index.php", 10) == 0) {
                                    if (uri[10] == '\0') {
                                        uri = "/";
                                    } else {
                                        uri = uri + 10;
                                    }
                                }
                            }
                            debug_log("[RINIT] Using REQUEST_URI (cleaned): %s", uri);
                        }
                    }
                }
                
                if (uri && *uri != '\0') {
                    size_t name_len = strlen(method) + 1 + strlen(uri) + 1;
                    root_span_name = malloc(name_len);
                    if (root_span_name) {
                        snprintf(root_span_name, name_len, "%s %s", method, uri);
                    }
                    root_span_url_path = strdup(uri);
                } else {
                    root_span_name = strdup("PHP Request");
                }
            } else {
                debug_log("[RINIT] req_info is NULL or too short, using fallback");
                root_span_name = strdup("PHP Request");
                root_span_http_request_json = strdup("{\"method\":\"GET\",\"uri\":\"/\"}");
                if (req_info) free(req_info);
            }
        } else if (is_cli) {
            // CLI request - capture command arguments
            root_span_name = strdup("PHP CLI");
            root_span_http_request_json = strdup("{\"method\":\"CLI\"}");
        } else {
            // Fallback
            root_span_name = strdup("PHP Request");
            root_span_http_request_json = strdup("{\"method\":\"GET\",\"uri\":\"/\"}");
        }
    
    // Initialize root_span_dumps array for this request
    // This must be done inside the mutex lock to ensure thread safety
    // Always reset/reinitialize for new request (clear any leftover dumps from previous request)
    if (root_span_dumps) {
        // Clean up previous request's dumps
        zval_ptr_dtor(root_span_dumps);
        efree(root_span_dumps);
        root_span_dumps = NULL;
    }
    // Always create fresh dumps array for new request
    root_span_dumps = emalloc(sizeof(zval));
    array_init(root_span_dumps);
    debug_log("[RINIT] Initialized root_span_dumps array for new request");
    
    pthread_mutex_unlock(&root_span_data_mutex);
    
    // Initialize and start collector for function call tracking
    // This is required for capturing SQL, HTTP, cache, and Redis operations
    // MUST be done in RINIT (not RSHUTDOWN) so it's ready when observer callbacks execute
    if (!global_collector) {
        global_collector = opa_collector_init();
    }
    if (global_collector && global_collector->magic == OPA_COLLECTOR_MAGIC) {
        opa_collector_start(global_collector);
        
        // Initialize global SQL queries array
        pthread_mutex_lock(&global_collector->global_sql_mutex);
        if (global_collector->global_sql_queries) {
            zval_ptr_dtor(global_collector->global_sql_queries);
            efree(global_collector->global_sql_queries);
        }
        global_collector->global_sql_queries = ecalloc(1, sizeof(zval));
        if (global_collector->global_sql_queries) {
            array_init(global_collector->global_sql_queries);
        }
        pthread_mutex_unlock(&global_collector->global_sql_mutex);
    }
    
    return SUCCESS;
}

// TEMPORARILY DISABLED: Moved collector initialization out of RINIT
/*
    // TEMPORARILY MINIMIZED: Disable all complex operations in RINIT to isolate crash
    // Only do absolute minimum - check if enabled and set flag
    if (OPA_G(enabled)) {
        profiling_active = 1;
        
        // Minimal root span setup - defer everything else to RSHUTDOWN
        pthread_mutex_lock(&root_span_data_mutex);
        if (!root_span_span_id) {
            root_span_span_id = strdup(generate_id());
            root_span_trace_id = strdup(generate_id());
            root_span_start_ts = get_timestamp_ms();
            root_span_cpu_ms = 0;
            root_span_status = -1;
            root_span_name = strdup("PHP Request");
            root_span_http_request_json = strdup("{\"scheme\":\"http\"}");
        }
        pthread_mutex_unlock(&root_span_data_mutex);
        
        // Defer collector initialization and other setup
        // Will be done on first function call or in RSHUTDOWN
    }
    
    return SUCCESS;
}

// TEMPORARILY DISABLED: Full RINIT code moved here to isolate crash
/*
    debug_log("[RINIT] Called, sapi=%s, enabled=%d", sapi_name, OPA_G(enabled));
    
    // Activate profiling for this request if enabled (both CLI and FPM)
    if (OPA_G(enabled)) {
        profiling_active = 1;
        
        debug_log("[RINIT] Profiling activated: enabled=%d, sampling_rate=%.2f, collect_internal=%d",
            OPA_G(enabled), OPA_G(sampling_rate), OPA_G(collect_internal_functions));
        
        // Create and start collector ()
        if (!global_collector) {
            global_collector = opa_collector_init();
        }
        if (global_collector) {
            opa_collector_start(global_collector);
        }
        
        // Reset network counters
        network_bytes_sent_total = 0;
        network_bytes_received_total = 0;
        
        // Create root span for this request
        // Root span tracks the entire request lifecycle and is created early in RINIT
        pthread_mutex_lock(&root_span_data_mutex);
        if (!root_span_span_id) {
            root_span_span_id = strdup(generate_id());
            root_span_trace_id = strdup(generate_id());
            root_span_start_ts = get_timestamp_ms();
            root_span_cpu_ms = 0;
            root_span_status = -1;
            
            // Extract URL components (for web requests, not CLI)
            // PERFECTLY SAFE - Capture HTTP request info using SG(request_info) only (no $_SERVER access)
            if (!is_cli && SG(request_info).request_method) {
                // HTTP request - use safe_serialize_request() which only uses SG(request_info)
                char *req_info = safe_serialize_request();
                debug_log("[RINIT] safe_serialize_request() returned: %p, content=%.200s", req_info, req_info ? req_info : "NULL");
                
                // Free old value if exists
                if (root_span_http_request_json) {
                    free(root_span_http_request_json);
                }
                root_span_http_request_json = req_info;
                debug_log("[RINIT] root_span_http_request_json set to: %p, content=%.200s", root_span_http_request_json, root_span_http_request_json ? root_span_http_request_json : "NULL");
                
                // Build span name from method + URI
                const char *method = SG(request_info).request_method ? SG(request_info).request_method : "GET";
                const char *uri = SG(request_info).request_uri ? SG(request_info).request_uri : "/";
                
                if (uri && *uri != '\0') {
                    size_t name_len = strlen(method) + 1 + strlen(uri) + 1;
                            root_span_name = malloc(name_len);
                            if (root_span_name) {
                        snprintf(root_span_name, name_len, "%s %s", method, uri);
                            }
                    root_span_url_path = strdup(uri);
                        } else {
                    root_span_name = strdup("PHP Request");
                }
            } else if (is_cli) {
                // CLI mode: capture command arguments
                // For CLI, we still use symbol table as PG(http_globals) may not be populated
                zval *server = NULL;
                if ((server = zend_hash_str_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER")-1)) != NULL) {
                    if (Z_TYPE_P(server) == IS_ARRAY) {
                        zval *argv = zend_hash_str_find(Z_ARRVAL_P(server), "argv", sizeof("argv")-1);
                        if (argv && Z_TYPE_P(argv) == IS_ARRAY) {
                            char *cli_args_json = serialize_cli_args_json(argv);
                            if (cli_args_json) {
                                root_span_cli_args_json = cli_args_json;
                                
                                // Update span name to include command
                                zval *script = zend_hash_index_find(Z_ARRVAL_P(argv), 0);
                                if (script && Z_TYPE_P(script) == IS_STRING) {
                                    size_t name_len = 5 + Z_STRLEN_P(script) + 1; // "CLI: " + script + null
                                    zend_ulong num_args = zend_hash_num_elements(Z_ARRVAL_P(argv));
                                    if (num_args > 1) {
                                        // Estimate space for args (rough estimate)
                                        name_len += (num_args - 1) * 20; // ~20 chars per arg
                                    }
                                    char *cli_name = malloc(name_len);
                                    if (cli_name) {
                                        snprintf(cli_name, name_len, "CLI: %s", Z_STRVAL_P(script));
                                        // Add args if present
                                        if (num_args > 1) {
                                            size_t pos = strlen(cli_name);
                                            for (zend_ulong i = 1; i < num_args && pos < name_len - 1; i++) {
                                                zval *arg = zend_hash_index_find(Z_ARRVAL_P(argv), i);
                                                if (arg && Z_TYPE_P(arg) == IS_STRING) {
                                                    size_t arg_len = Z_STRLEN_P(arg);
                                                    if (pos + 1 + arg_len < name_len - 1) {
                                                        cli_name[pos++] = ' ';
                                                        memcpy(cli_name + pos, Z_STRVAL_P(arg), arg_len);
                                                        pos += arg_len;
                                                    } else {
                                                        // Truncate if too long
                                                        cli_name[pos++] = ' ';
                                                        cli_name[pos++] = '.';
                                                        cli_name[pos++] = '.';
                                                        cli_name[pos++] = '.';
                                                        break;
                                                    }
                                                }
                                            }
                                            cli_name[pos] = '\0';
                                        }
                                        if (root_span_name) free(root_span_name);
                                        root_span_name = cli_name;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (!root_span_name) {
                // Last resort: try to get any available info from $_SERVER
                zval *server = NULL;
                if ((server = zend_hash_str_find(&EG(symbol_table), "_SERVER", sizeof("_SERVER")-1)) != NULL) {
                    if (Z_TYPE_P(server) == IS_ARRAY) {
                        zval *request_uri = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
                        if (request_uri && Z_TYPE_P(request_uri) == IS_STRING && Z_STRLEN_P(request_uri) > 0) {
                            root_span_name = strdup(Z_STRVAL_P(request_uri));
                        } else {
                            zval *script_name = zend_hash_str_find(Z_ARRVAL_P(server), "SCRIPT_NAME", sizeof("SCRIPT_NAME")-1);
                            if (script_name && Z_TYPE_P(script_name) == IS_STRING && Z_STRLEN_P(script_name) > 0) {
                                root_span_name = strdup(Z_STRVAL_P(script_name));
                            } else {
                root_span_name = strdup("PHP Request");
                            }
                        }
                    } else {
                        root_span_name = strdup("PHP Request");
                    }
                } else {
                    root_span_name = strdup("PHP Request");
                }
            }
            
            debug_log("[RINIT] Created root span: id=%s, name=%s", root_span_span_id, root_span_name);
        }
        pthread_mutex_unlock(&root_span_data_mutex);
        
        debug_log("[RINIT] Profiling activated, collector=%p, sapi=%s", global_collector, sapi_name);
        
        // Initialize active spans if not already done (simplified - no cleanup of leftover spans)
        // : don't clean up leftover spans in RINIT, just ensure hash table exists
        // Use malloc instead of emalloc to prevent PHP from automatically destroying
        // the hash table during MSHUTDOWN when zvals are invalid
        if (!active_spans) {
            active_spans = malloc(sizeof(HashTable));
            // Use NULL destructor to prevent PHP from accessing zvals during destruction
            // We'll manually free spans in RSHUTDOWN before MSHUTDOWN
            zend_hash_init(active_spans, 8, NULL, NULL, 0);
        }
        // Note: We don't clean up leftover spans here - RSHUTDOWN should handle it
        // If RSHUTDOWN didn't run, the spans will be overwritten or cleaned up naturally
    } else {
        debug_log("[RINIT] Profiling NOT activated: enabled=%d", OPA_G(enabled));
    }
    
    // Log RINIT end only if debug logging is enabled
    if (OPA_G(debug_log_enabled)) {
        FILE *rinit_log10 = fopen("/app/logs/opa_debug.log", "a");
        if (rinit_log10) {
            fprintf(rinit_log10, "[RINIT] END - returning SUCCESS\n");
            fflush(rinit_log10);
            fclose(rinit_log10);
        }
    }
    
    return SUCCESS;
}
*/

// Helper function to find parent span_id for a call node (span expansion)
// Returns root_span_id if no significant parent found, or parent's call_id if parent is significant
static char* find_parent_span_id_for_call(call_node_t *call, call_node_t *all_calls, const char *root_span_id) {
    if (!call || !call->parent_id || strlen(call->parent_id) == 0) {
        return (char*)root_span_id; // Root-level call, parent is root span
    }
    
    // Find parent call node
    call_node_t *parent_call = all_calls;
    while (parent_call) {
        if (parent_call->call_id && strcmp(parent_call->call_id, call->parent_id) == 0) {
            // Found parent call - check if it's significant
            int has_sql = (parent_call->sql_queries && Z_TYPE_P(parent_call->sql_queries) == IS_ARRAY && 
                           zend_hash_num_elements(Z_ARRVAL_P(parent_call->sql_queries)) > 0);
            int has_http = (parent_call->http_requests && Z_TYPE_P(parent_call->http_requests) == IS_ARRAY && 
                            zend_hash_num_elements(Z_ARRVAL_P(parent_call->http_requests)) > 0);
            int has_cache = (parent_call->cache_operations && Z_TYPE_P(parent_call->cache_operations) == IS_ARRAY && 
                             zend_hash_num_elements(Z_ARRVAL_P(parent_call->cache_operations)) > 0);
            int has_redis = (parent_call->redis_operations && Z_TYPE_P(parent_call->redis_operations) == IS_ARRAY && 
                             zend_hash_num_elements(Z_ARRVAL_P(parent_call->redis_operations)) > 0);
            
            double end_time = parent_call->end_time > 0.0 ? parent_call->end_time : parent_call->start_time + 0.001;
            double duration_ms = (end_time - parent_call->start_time) * 1000.0;
            if (duration_ms < 0.0) duration_ms = 0.0;
            
            // If parent is significant, use its call_id as span_id
            if (has_sql || has_http || has_cache || has_redis || duration_ms > 10.0) {
                return parent_call->call_id; // Parent will be sent as span, use its call_id
            } else {
                // Parent is not significant, traverse up
                return find_parent_span_id_for_call(parent_call, all_calls, root_span_id);
            }
        }
        parent_call = parent_call->next;
    }
    
    // Parent call not found, use root span
    return (char*)root_span_id;
}

PHP_RSHUTDOWN_FUNCTION(opa) {
    // Skip logging in CLI mode
    int is_cli = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0);
    
    
    debug_log("[RSHUTDOWN] START - is_cli=%d, collector=%p", is_cli, global_collector);
    
    // Disable profiling first to stop hook processing ()
    profiling_active = 0;
    
    // Clean up observer data hash table
    pthread_mutex_lock(&observer_data_mutex);
    if (observer_data_table) {
        // Free all remaining observer data entries
        zend_hash_destroy(observer_data_table);
        free(observer_data_table); // Use free (not efree) since we used malloc
        observer_data_table = NULL;
    }
    pthread_mutex_unlock(&observer_data_mutex);
    
    // Get root span data from malloc'd global variables (NOT from emalloc'd structure)
    // This is safe even after fastcgi_finish_request()
    char *json_str = NULL;
    size_t json_len = 0;
    
    pthread_mutex_lock(&root_span_data_mutex);
    debug_log("[RSHUTDOWN] root_span_span_id=%p, collector=%p", root_span_span_id, global_collector);
    if (root_span_span_id) {
        debug_log("[RSHUTDOWN] About to produce span JSON, collector=%p", global_collector);
        // Data already in malloc'd memory - safe to use directly
        long end_ts = get_timestamp_ms(); // Finalize end_ts
        // Status is calculated by agent based on HTTP response codes and error indicators
        // Extension only provides data (HTTP response, dumps, etc.) - agent does the calculation
        int status = root_span_status; // Use existing status (defaults to -1, meaning "not set")
        
        // Serialize root span dumps to JSON (before fastcgi_finish_request, so zvals are safe)
        char *dumps_json = NULL;
        debug_log("[RSHUTDOWN] Checking root_span_dumps: ptr=%p", root_span_dumps);
        if (root_span_dumps) {
            debug_log("[RSHUTDOWN] root_span_dumps type=%d (IS_ARRAY=%d)", Z_TYPE_P(root_span_dumps), IS_ARRAY);
            if (Z_TYPE_P(root_span_dumps) == IS_ARRAY) {
                int dumps_count = zend_hash_num_elements(Z_ARRVAL_P(root_span_dumps));
                debug_log("[RSHUTDOWN] root_span_dumps found, count=%d, span_id=%s", dumps_count, root_span_span_id ? root_span_span_id : "NULL");
                if (dumps_count > 0) {
                    smart_string dumps_buf = {0};
                    serialize_zval_json(&dumps_buf, root_span_dumps);
                    smart_string_0(&dumps_buf);
                    
                    debug_log("[RSHUTDOWN] Serialized dumps, len=%zu, preview=%.200s", dumps_buf.len, dumps_buf.c ? dumps_buf.c : "NULL");
                    
                    // Convert smart_string to malloc'd char*
                    if (dumps_buf.c && dumps_buf.len > 0) {
                        dumps_json = malloc(dumps_buf.len + 1);
                        if (dumps_json) {
                            memcpy(dumps_json, dumps_buf.c, dumps_buf.len);
                            dumps_json[dumps_buf.len] = '\0';
                            debug_log("[RSHUTDOWN] Allocated dumps_json, len=%zu", dumps_buf.len);
                        } else {
                            debug_log("[RSHUTDOWN] Failed to malloc dumps_json");
                        }
                    } else {
                        debug_log("[RSHUTDOWN] dumps_buf is empty or NULL");
                    }
                    
                    // Free smart_string
                    if (dumps_buf.c) {
                        smart_string_free(&dumps_buf);
                    }
                } else {
                    debug_log("[RSHUTDOWN] root_span_dumps is empty (count=0)");
                }
            } else {
                debug_log("[RSHUTDOWN] root_span_dumps is not an array, type=%d", Z_TYPE_P(root_span_dumps));
            }
        } else {
            debug_log("[RSHUTDOWN] root_span_dumps is NULL");
        }
        
        // Capture HTTP request details if not already captured
        // Note: _SERVER might not be available in RSHUTDOWN, so we capture in RINIT instead
        // This is just a fallback
        if (!root_span_http_request_json && !is_cli) {
            root_span_http_request_json = strdup("{\"scheme\":\"http\"}");
        }
        
        // Enhance HTTP request JSON with full $_SERVER data (safer in RSHUTDOWN)
        // Use zend_is_auto_global() pattern (like php-spx) to safely access $_SERVER
        // Only enhance if we have valid data from RINIT
        if (!is_cli && root_span_http_request_json && strlen(root_span_http_request_json) > 2) {
            debug_log("[RSHUTDOWN] Before enhancement: http_request_json=%.200s", root_span_http_request_json);
            zend_string *server_name = zend_string_init("_SERVER", sizeof("_SERVER")-1, 0);
            zend_is_auto_global(server_name);
            zval *server = zend_hash_find(&EG(symbol_table), server_name);
            zend_string_release(server_name);
            
            if (server && Z_TYPE_P(server) == IS_ARRAY) {
                // Re-serialize with full $_SERVER data (scheme, host, remote_addr, etc.)
                char *enhanced_json = serialize_http_request_json(server);
                if (enhanced_json && strlen(enhanced_json) > 2) {
                    debug_log("[RSHUTDOWN] Enhanced http_request_json: old=%.200s, new=%.200s", root_span_http_request_json, enhanced_json);
                    free(root_span_http_request_json); // Free old minimal JSON
                    root_span_http_request_json = enhanced_json;
                } else {
                    debug_log("[RSHUTDOWN] Enhanced JSON is empty or NULL (len=%zu), keeping original", enhanced_json ? strlen(enhanced_json) : 0);
                    if (enhanced_json) free(enhanced_json);
                }
            } else {
                debug_log("[RSHUTDOWN] $_SERVER not available or not an array, keeping original JSON");
            }
        } else {
            debug_log("[RSHUTDOWN] Not enhancing http_request_json: is_cli=%d, json=%p, len=%zu", is_cli, root_span_http_request_json, root_span_http_request_json ? strlen(root_span_http_request_json) : 0);
        }
        
        // Capture HTTP response headers if not CLI
        char *http_resp_json = NULL;
        if (!is_cli && sapi_module.name && strcmp(sapi_module.name, "fpm-fcgi") == 0) {
            http_resp_json = serialize_http_response_json();
            if (http_resp_json) {
                root_span_http_response_json = http_resp_json;
            }
        }
        
        // Produce JSON from malloc'd data (already safe, no need to copy again)
        debug_log("[RSHUTDOWN] Calling produce_span_json_from_values with dumps_json=%p, len=%zu", dumps_json, dumps_json ? strlen(dumps_json) : 0);
        debug_log("[RSHUTDOWN] HTTP request JSON: %p, len=%zu", root_span_http_request_json, root_span_http_request_json ? strlen(root_span_http_request_json) : 0);
        debug_log("[RSHUTDOWN] HTTP response JSON: %p, len=%zu", root_span_http_response_json, root_span_http_response_json ? strlen(root_span_http_response_json) : 0);
        json_str = produce_span_json_from_values(
            root_span_trace_id, root_span_span_id, root_span_parent_id, root_span_name,
            root_span_url_scheme, root_span_url_host, root_span_url_path,
            root_span_start_ts, end_ts, root_span_cpu_ms, status, dumps_json,
            root_span_cli_args_json, root_span_http_request_json, root_span_http_response_json,
            NULL  // tags_json (root span doesn't have custom tags)
        );
        debug_log("[RSHUTDOWN] Span JSON produced, json_str=%p, len=%zu", json_str, json_str ? strlen(json_str) : 0);
        
        // Free dumps JSON if allocated
        if (dumps_json) {
            free(dumps_json);
        }
        
        if (json_str) {
            json_len = strlen(json_str);
        }
    }
    pthread_mutex_unlock(&root_span_data_mutex);
    
    // Finish request to client BEFORE sending data ()
    // This ensures the client receives the response immediately
    opa_finish_request();
    
    // Now send profiling data in background (client connection is already closed)
    // Use pre-encoded JSON string in regular C memory - safe after fastcgi_finish_request()
    if (json_str && json_len > 0) {
        // During RSHUTDOWN, PHP's memory manager is still active, so emalloc/efree is safe
        // Convert malloc'd string to emalloc'd for send_message_direct
        // (send_message_direct expects emalloc'd and will efree it)
        char *msg_copy = estrdup(json_str);
        if (msg_copy) {
            free(json_str); // Free malloc'd version
            send_message_direct(msg_copy, 1);
        } else {
            // NOTE: Do NOT call log_error() here during shutdown - it could cause issues
            // Just free the malloc'd string and continue
            free(json_str);
        }
    } else if (json_str) {
        free(json_str); // Free if not sent
    }
    
    // Send child spans as separate messages (if expand_spans is enabled)
    // All sending happens here in RSHUTDOWN after fastcgi_finish_request()
    if (OPA_G(expand_spans) && root_span_span_id && root_span_trace_id && global_collector && 
        global_collector->magic == OPA_COLLECTOR_MAGIC && global_collector->calls) {
        
        debug_log("[RSHUTDOWN] expand_spans enabled, sending child spans from call stack");
        
        // Iterate through all calls and send significant ones as child spans
        call_node_t *call = global_collector->calls;
        int child_spans_sent = 0;
        long root_start_ts = root_span_start_ts;
        
        while (call) {
            if (call->magic == OPA_CALL_NODE_MAGIC && call->start_time > 0.0) {
                // Check if this call is significant
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
                
                if (has_sql || has_http || has_cache || has_redis || duration_ms > 10.0) {
                    // Significant call - send as child span
                    char *parent_span_id = find_parent_span_id_for_call(call, global_collector->calls, root_span_span_id);
                    
                    char *child_json = produce_child_span_json_from_call_node(
                        call, root_span_trace_id, parent_span_id, root_start_ts
                    );
                    
                    if (child_json) {
                        debug_log("[RSHUTDOWN] Sending child span: call_id=%s, parent_span_id=%s", 
                            call->call_id ? call->call_id : "NULL", parent_span_id);
                        
                        // Convert to emalloc'd for send_message_direct
                        char *msg_copy = estrdup(child_json);
                        if (msg_copy) {
                            free(child_json); // Free malloc'd version
                            send_message_direct(msg_copy, 1);
                            child_spans_sent++;
                        } else {
                            free(child_json);
                            log_error("Failed to allocate memory for child span message", "estrdup failed", "");
                        }
                    }
                }
            }
            call = call->next;
        }
        
        debug_log("[RSHUTDOWN] Sent %d child spans (expand_spans mode)", child_spans_sent);
    }
    
    // Free collector () - this will free all calls
    if (global_collector) {
        opa_collector_free(global_collector);
        global_collector = NULL;
    }
    
    // Reset network counters
    network_bytes_sent_total = 0;
    network_bytes_received_total = 0;
    
    // Destroy active_spans hash table in RSHUTDOWN (before MSHUTDOWN)
    // This prevents PHP from trying to destroy it automatically during MSHUTDOWN
    // when zvals are invalid
    if (active_spans) {
        pthread_mutex_lock(&active_spans_mutex);
        if (active_spans) {
            // During RSHUTDOWN, zvals should still be valid
            // Free all spans using the destructor
            zend_hash_apply(active_spans, free_span_dtor);
            
            // Clear the hash table completely to prevent PHP from trying to destroy it
            // This should make the hash table empty, so PHP won't try to access zvals
            zend_hash_clean(active_spans);
            
            // Now destroy the hash table structure
            zend_hash_destroy(active_spans);
            free(active_spans); // Use free (not efree) since we used malloc
            active_spans = NULL;
        }
        pthread_mutex_unlock(&active_spans_mutex);
    }
    
    // Clear root span data
    pthread_mutex_lock(&root_span_data_mutex);
    if (root_span_trace_id) { free(root_span_trace_id); root_span_trace_id = NULL; }
    if (root_span_span_id) { free(root_span_span_id); root_span_span_id = NULL; }
    if (root_span_parent_id) { free(root_span_parent_id); root_span_parent_id = NULL; }
    if (root_span_name) { free(root_span_name); root_span_name = NULL; }
    if (root_span_url_scheme) { free(root_span_url_scheme); root_span_url_scheme = NULL; }
    if (root_span_url_host) { free(root_span_url_host); root_span_url_host = NULL; }
    if (root_span_dumps) { 
        zval_ptr_dtor(root_span_dumps); 
        efree(root_span_dumps); 
        root_span_dumps = NULL; 
    }
    if (root_span_url_path) { free(root_span_url_path); root_span_url_path = NULL; }
    if (root_span_cli_args_json) { free(root_span_cli_args_json); root_span_cli_args_json = NULL; }
    if (root_span_http_request_json) { free(root_span_http_request_json); root_span_http_request_json = NULL; }
    if (root_span_http_response_json) { free(root_span_http_response_json); root_span_http_response_json = NULL; }
    if (root_span_dumps) {
        zval_ptr_dtor(root_span_dumps);
        efree(root_span_dumps);
        root_span_dumps = NULL;
    }
    root_span_start_ts = 0;
    root_span_end_ts = 0;
    root_span_cpu_ms = 0;
    root_span_status = -1;
    pthread_mutex_unlock(&root_span_data_mutex);
    
    // Cleanup error tracking
    // TEMPORARILY DISABLED: Error tracking cleanup (causes compilation issues)
    // opa_cleanup_error_tracking();
    
    return SUCCESS;
}

// Forward declarations for PHP functions (defined in opa_api.c)
PHP_FUNCTION(opa_start_span);
PHP_FUNCTION(opa_end_span);
PHP_FUNCTION(opa_add_tag);
PHP_FUNCTION(opa_set_parent);
PHP_FUNCTION(dump);
PHP_FUNCTION(opa_dump);
PHP_FUNCTION(opa_enable);
PHP_FUNCTION(opa_disable);
PHP_FUNCTION(opa_is_enabled);
PHP_FUNCTION(opa_track_error);

// Forward declarations for arginfo (defined in opa_api.c)
ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_start_span, 0, 0, 1)
    ZEND_ARG_INFO(0, name)
    ZEND_ARG_ARRAY_INFO(0, tags, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_end_span, 0, 0, 1)
    ZEND_ARG_INFO(0, span_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_add_tag, 0, 0, 3)
    ZEND_ARG_INFO(0, span_id)
    ZEND_ARG_INFO(0, key)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_set_parent, 0, 0, 2)
    ZEND_ARG_INFO(0, span_id)
    ZEND_ARG_INFO(0, parent_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_dump, 0, 0, 0)
    ZEND_ARG_VARIADIC_INFO(0, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_dump, 0, 0, 0)
    ZEND_ARG_VARIADIC_INFO(0, vars)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_enable, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_disable, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_is_enabled, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_opa_track_error, 0, 0, 2)
    ZEND_ARG_INFO(0, error_type)
    ZEND_ARG_INFO(0, error_message)
    ZEND_ARG_INFO(0, file)
    ZEND_ARG_INFO(0, line)
    ZEND_ARG_ARRAY_INFO(0, stack_trace, 1)
ZEND_END_ARG_INFO()

// Function entries
static const zend_function_entry opa_functions[] = {
    PHP_FE(opa_start_span, arginfo_opa_start_span)
    PHP_FE(opa_end_span, arginfo_opa_end_span)
    PHP_FE(opa_add_tag, arginfo_opa_add_tag)
    PHP_FE(opa_set_parent, arginfo_opa_set_parent)
    PHP_FE(dump, arginfo_dump)
    PHP_FE(opa_dump, arginfo_opa_dump)
    PHP_FE(opa_enable, arginfo_opa_enable)
    PHP_FE(opa_disable, arginfo_opa_disable)
    PHP_FE(opa_is_enabled, arginfo_opa_is_enabled)
    PHP_FE(opa_track_error, arginfo_opa_track_error)
    PHP_FE_END
};

zend_module_entry opa_module_entry = {
    STANDARD_MODULE_HEADER,
    "opa",
    opa_functions,
    PHP_MINIT(opa),
    PHP_MSHUTDOWN(opa),
    PHP_RINIT(opa),
    PHP_RSHUTDOWN(opa),
    NULL,
    "1.0.0",
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_OPA
ZEND_GET_MODULE(opa)
#endif
