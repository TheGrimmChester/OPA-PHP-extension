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

// zend_execute_ex hook ()
void (*original_zend_execute_ex)(zend_execute_data *execute_data) = NULL;

// Stack sampling (disabled for now - can be re-enabled later if needed)
// static timer_t sampling_timer;
// static int sampling_enabled = 0;

// Helper: Generate unique ID
char* generate_id() {
    char *id = emalloc(17);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snprintf(id, 17, "%016lx", (unsigned long)(tv.tv_sec * 1000000 + tv.tv_usec) ^ (unsigned long)pthread_self());
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

// FPM-Optimized Universal Serializer - PG(http_globals) FIRST, then SG(request_info) fallback
// FPM pattern: SG(request_info) NULL → PG(http_globals) populated
// CRITICAL: Must call zend_is_auto_global() to initialize $_SERVER before accessing PG(http_globals)
char* serialize_http_request_json_universal(void) {
    char debug_buf[512];
    int debug_len;
    
    /* TARGETED DEBUG 1: Verify function is being called */
    fprintf(stderr, "[OPA SERIALIZER] FUNCTION CALLED\n");
    
    /* CRITICAL FIX: Initialize $_SERVER using zend_is_auto_global() before accessing PG(http_globals) */
    zend_string *server_name = zend_string_init("_SERVER", sizeof("_SERVER")-1, 0);
    zend_is_auto_global(server_name);
    zend_string_release(server_name);
    
    fprintf(stderr, "[OPA SERIALIZER] Called zend_is_auto_global(_SERVER)\n");
    
    /* TARGETED DEBUG 2: Check PG(http_globals) state AFTER initialization */
    zval *pg_server = &PG(http_globals)[TRACK_VARS_SERVER];
    debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                        "[OPA SERIALIZER] PG server=%p, type=%d, is_array=%d, num_elements=%d\n", 
                        pg_server,
                        pg_server ? Z_TYPE_P(pg_server) : -1,
                        pg_server && Z_TYPE_P(pg_server) == IS_ARRAY,
                        (pg_server && Z_TYPE_P(pg_server) == IS_ARRAY) ? (int)zend_hash_num_elements(Z_ARRVAL_P(pg_server)) : 0);
    fprintf(stderr, "%s", debug_buf);
    
    /* FPM PRIORITY 1: PG(http_globals)[TRACK_VARS_SERVER] - NOW properly initialized */
    zval *server = &PG(http_globals)[TRACK_VARS_SERVER];
    
    /* TARGETED DEBUG 3: Detailed PG(http_globals) investigation */
    if (server && Z_TYPE_P(server) == IS_ARRAY) {
        int num_elements = (int)zend_hash_num_elements(Z_ARRVAL_P(server));
        debug_len = snprintf(debug_buf, sizeof(debug_buf), "[OPA SERIALIZER] PG server is ARRAY with %d elements\n", num_elements);
        fprintf(stderr, "%s", debug_buf);
        
        if (num_elements > 0) {
            zval *method_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
            zval *uri_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
            zval *query_zv = zend_hash_str_find(Z_ARRVAL_P(server), "QUERY_STRING", sizeof("QUERY_STRING")-1);
            zval *remote_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REMOTE_ADDR", sizeof("REMOTE_ADDR")-1);
            
            /* TARGETED DEBUG 4: Check what we found */
            debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                                "[OPA SERIALIZER] Found: method=%p, uri=%p, query=%p, remote=%p\n",
                                method_zv, uri_zv, query_zv, remote_zv);
            fprintf(stderr, "%s", debug_buf);
            
            if (method_zv) {
                debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                                    "[OPA SERIALIZER] method_zv type=%d, is_string=%d\n",
                                    Z_TYPE_P(method_zv), Z_TYPE_P(method_zv) == IS_STRING);
                fprintf(stderr, "%s", debug_buf);
            }
            if (uri_zv) {
                debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                                    "[OPA SERIALIZER] uri_zv type=%d, is_string=%d\n",
                                    Z_TYPE_P(uri_zv), Z_TYPE_P(uri_zv) == IS_STRING);
                fprintf(stderr, "%s", debug_buf);
            }
            
            if (method_zv && Z_TYPE_P(method_zv) == IS_STRING &&
                uri_zv && Z_TYPE_P(uri_zv) == IS_STRING) {
                
                debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                                    "[OPA SERIALIZER] PG HIT! method=%s uri=%s\n",
                                    Z_STRVAL_P(method_zv), Z_STRVAL_P(uri_zv));
                fprintf(stderr, "%s", debug_buf);
                
                char buf[1024];
                snprintf(buf, sizeof(buf),
                    "{\"method\":\"%s\",\"uri\":\"%s\","
                     "\"query_string\":\"%s\",\"remote_addr\":\"%s\","
                     "\"source\":\"PG\"}",
                    Z_STRVAL_P(method_zv),
                    Z_STRVAL_P(uri_zv),
                    query_zv && Z_TYPE_P(query_zv) == IS_STRING ? Z_STRVAL_P(query_zv) : "",
                    remote_zv && Z_TYPE_P(remote_zv) == IS_STRING ? Z_STRVAL_P(remote_zv) : "unknown");
                
                debug_len = snprintf(debug_buf, sizeof(debug_buf), "[OPA SERIALIZER] Returning JSON: %s\n", buf);
                fprintf(stderr, "%s", debug_buf);
                
                return strdup(buf);
            } else {
                fprintf(stderr, "[OPA SERIALIZER] PG found but method/uri not valid strings\n");
            }
        } else {
            fprintf(stderr, "[OPA SERIALIZER] PG server array is EMPTY (num_elements=0)\n");
        }
    } else {
        fprintf(stderr, "[OPA SERIALIZER] PG server is NOT an array (server=%p, type=%d)\n", 
                server, server ? Z_TYPE_P(server) : -1);
    }
    
    /* PRIORITY 2: SAPI fallback (CLI/Apache) */
    debug_len = snprintf(debug_buf, sizeof(debug_buf), 
                        "[OPA] PG MISS -> SAPI method=%s uri=%s\n",
                        SG(request_info).request_method ? SG(request_info).request_method : "NULL",
                        SG(request_info).request_uri ? SG(request_info).request_uri : "NULL");
    fprintf(stderr, "%s", debug_buf);
    
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"method\":\"%s\",\"uri\":\"%s\",\"query_string\":\"%s\","
         "\"source\":\"SAPI\"}",
        SG(request_info).request_method ? SG(request_info).request_method : "GET",
        SG(request_info).request_uri ? SG(request_info).request_uri : "/",
        SG(request_info).query_string ? SG(request_info).query_string : "");
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
    
    // Allocate buffer (estimate: 400 + lengths)
    size_t method_len = strlen(method);
    size_t uri_len = strlen(uri);
    size_t query_len = strlen(query);
    size_t buf_size = 400 + method_len + uri_len + query_len + (host_len ? host_len : 0);
    char *result = malloc(buf_size);
    if (!result) {
        return strdup("{\"scheme\":\"http\",\"method\":\"GET\",\"uri\":\"/\"}");
    }
    
    // Build JSON with all fields
    int pos = snprintf(result, buf_size, 
        "{\"scheme\":\"%s\",\"method\":\"%s\",\"uri\":\"%s\"", 
        scheme, method, uri);
    
    if (host && host_len > 0 && host_len < 200) {
        pos += snprintf(result + pos, buf_size - pos, ",\"host\":\"%.*s\"", (int)host_len, host);
    }
    
    if (query && *query != '\0' && query_len < 500) {
        pos += snprintf(result + pos, buf_size - pos, ",\"query_string\":\"%s\"", query);
    }
    
    // Try to get IP from $_SERVER if available
    if (server_zv && Z_TYPE_P(server_zv) == IS_ARRAY) {
        zval *zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "REMOTE_ADDR", sizeof("REMOTE_ADDR")-1);
        if (!zv || Z_TYPE_P(zv) != IS_STRING) {
            zv = zend_hash_str_find(Z_ARRVAL_P(server_zv), "HTTP_X_FORWARDED_FOR", sizeof("HTTP_X_FORWARDED_FOR")-1);
        }
        if (zv && Z_TYPE_P(zv) == IS_STRING && Z_STRLEN_P(zv) > 0 && Z_STRLEN_P(zv) < 50) {
            pos += snprintf(result + pos, buf_size - pos, ",\"ip\":\"%.*s\"", (int)Z_STRLEN_P(zv), Z_STRVAL_P(zv));
        }
    }
    
    snprintf(result + pos, buf_size - pos, "}");
    
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
    zend_function *headers_list_func = zend_hash_str_find_ptr(EG(function_table), "headers_list", sizeof("headers_list") - 1);
    if (headers_list_func) {
        zval headers_list_func, headers_list_ret;
        ZVAL_UNDEF(&headers_list_func);
        ZVAL_UNDEF(&headers_list_ret);
        
        ZVAL_STRING(&headers_list_func, "headers_list");
        
        zend_fcall_info fci;
        zend_fcall_info_cache fcc;
        if (zend_fcall_info_init(&headers_list_func, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
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
        zval_dtor(&headers_list_func);
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
        if (strcmp(class_name, "PDO") == 0 || strcmp(class_name, "PDOStatement") == 0) {
            if (execute_data->func->common.function_name) {
                const char *method_name = ZSTR_VAL(execute_data->func->common.function_name);
                if (strcmp(method_name, "prepare") == 0 ||
                    strcmp(method_name, "query") == 0 ||
                    strcmp(method_name, "exec") == 0 ||
                    strcmp(method_name, "execute") == 0) {
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
                    strcmp(method_name, "lpush") == 0 ||
                    strcmp(method_name, "rpop") == 0 ||
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
void opa_execute_ex(zend_execute_data *execute_data) {
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
    
    // Determine function type
    if (func->type == ZEND_USER_FUNCTION) {
        function_type = class_name ? 2 : 0; /* method if has class, otherwise user function */
    } else if (func->type == ZEND_INTERNAL_FUNCTION) {
        function_type = class_name ? 2 : 1; /* method if has class, otherwise internal function */
    }
    
    // Get file and line information
    if (execute_data->opline) {
        line = execute_data->opline->lineno;
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
    if (function_name || class_name) {
        debug_log("[execute_ex] Entering function: %s::%s (type=%d)", 
            class_name ? class_name : "NULL", function_name ? function_name : "NULL", function_type);
        call_id = opa_enter_function(function_name, class_name, file, line, function_type);
        if (call_id) {
            debug_log("[execute_ex] Entered function: %s, call_id=%s", function_name ? function_name : "NULL", call_id);
        } else {
            debug_log("[execute_ex] Failed to enter function: %s", function_name ? function_name : "NULL");
        }
    } else {
        debug_log("[execute_ex] Skipping function: no name (func=%p)", execute_data->func);
    }
    
    // Check if this is a PDO method call and capture SQL BEFORE execution
    int pdo_method = is_pdo_method(execute_data);
    char *sql = NULL;
    double query_start_time = 0.0;
    
    if (pdo_method && execute_data) {
        query_start_time = get_time_seconds();
        
        // Get method name
        const char *method_name = function_name;
        const char *pdo_class_name = class_name;
        
        // Extract SQL based on method
        if (method_name) {
            if (strcmp(method_name, "prepare") == 0 || 
                strcmp(method_name, "query") == 0 || 
                strcmp(method_name, "exec") == 0) {
                // SQL is in first argument
                if (ZEND_CALL_NUM_ARGS(execute_data) > 0) {
                    zval *arg = ZEND_CALL_ARG(execute_data, 1);
                    if (arg && Z_TYPE_P(arg) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(arg));
                        debug_log("[execute_ex] Captured SQL from %s: %s", method_name, sql);
                    }
                }
            } else if (strcmp(method_name, "execute") == 0 && pdo_class_name && strcmp(pdo_class_name, "PDOStatement") == 0) {
                // For execute(), get SQL from PDOStatement's queryString property
                if (Z_TYPE(execute_data->This) == IS_OBJECT) {
                    zend_class_entry *ce = Z_OBJCE(execute_data->This);
                    zval query_string;
                    ZVAL_UNDEF(&query_string);
                    zval *query_string_prop = zend_read_property(ce, Z_OBJ(execute_data->This), "queryString", sizeof("queryString") - 1, 1, &query_string);
                    if (query_string_prop && Z_TYPE_P(query_string_prop) == IS_STRING) {
                        sql = estrdup(Z_STRVAL_P(query_string_prop));
                        debug_log("[execute_ex] Captured SQL from PDOStatement::execute: %s", sql);
                    }
                    if (Z_TYPE(query_string) != IS_UNDEF) {
                        zval_dtor(&query_string);
                    }
                }
            }
        }
    }
    
    // Check for curl_exec in BEFORE section (capture timing before execution)
    int curl_func_before = 0;
    double curl_start_time = 0.0;
    size_t curl_bytes_sent_before = 0;
    size_t curl_bytes_received_before = 0;
    
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
    
    // Call original handler
    original_zend_execute_ex(execute_data);
    
    // Capture SQL query AFTER execution if it's a PDO method
    if (pdo_method && sql && call_id) {
        double query_end_time = get_time_seconds();
        double query_duration = query_end_time - query_start_time;
        const char *query_type = function_name;
        int rows_affected = -1;
        
        // Try to get rows_affected from return value if available
        // Note: In zend_execute_ex, we don't have direct access to return_value
        // This is a limitation - we'll record 0 for now
        
        // Record SQL query using the current call from collector
        record_sql_query(sql, query_duration, NULL, query_type, rows_affected);
        debug_log("[execute_ex] Recorded SQL query: %s, duration=%.6f", sql, query_duration);
        efree(sql);
    } else if (pdo_method && sql) {
        // Free SQL if we captured it but don't have a call_id
        efree(sql);
    }
    
    // Re-check curl function after execution using argument-based detection
    // Only check if function has at least 1 argument
    zval *curl_handle_after = NULL;
    int curl_func_after = 0;
    int curl_func_type_after = 0;
    
    // Detect curl_exec calls - try is_curl_call first (works when function_name is NULL)
    // Then fallback to function name detection
    if (execute_data && execute_data->func) {
        uint32_t num_args = ZEND_CALL_NUM_ARGS(execute_data);
        if (num_args > 0) {
            curl_func_after = is_curl_call(execute_data, &curl_handle_after);
            if (curl_func_after) {
                // Check if it's curl_exec by argument count (curl_exec has 1 arg)
                if (num_args == 1) {
                    curl_func_type_after = 1; // curl_exec
                } else {
                    curl_func_type_after = get_curl_function_type(execute_data);
                }
            }
        }
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
                            
                            // Get response size
                            zval *size_val = zend_hash_str_find(Z_ARRVAL(curl_getinfo_ret), "size_download", sizeof("size_download") - 1);
                            if (size_val && Z_TYPE_P(size_val) == IS_DOUBLE) {
                                response_size = (size_t)Z_DVAL_P(size_val);
                            } else if (size_val && Z_TYPE_P(size_val) == IS_LONG) {
                                response_size = (size_t)Z_LVAL_P(size_val);
                            }
                            
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
            response_size, dns_time, connect_time, total_time);
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
}

/* SQL Profiling Hooks */

// Store original function handlers
static zend_function *orig_mysqli_query_func = NULL;
static zif_handler orig_mysqli_query_handler = NULL;
static zend_function *orig_pdo_query_func = NULL;
static zif_handler orig_pdo_query_handler = NULL;
static zend_function *orig_pdo_stmt_execute_func = NULL;
static zif_handler orig_pdo_stmt_execute_handler = NULL;

// Helper: Get current time in microseconds
static double get_microtime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

// MySQLi query hook
PHP_FUNCTION(opaphp_mysqli_query) {
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
        php_printf("[OPA SQL Profiling] MySQLi Query: %s | Time: %.3fms | Rows: %ld\n", 
                   ZSTR_VAL(query), elapsed, rows_affected);
        
        // Send SQL query data to agent via record_sql_query
        // Duration is in milliseconds, convert to seconds for record_sql_query
        double duration_seconds = elapsed / 1000.0;
        record_sql_query(ZSTR_VAL(query), duration_seconds, NULL, "mysqli_query", rows_affected);
    }
}

// PDO::query hook
PHP_METHOD(PDO, query) {
    zend_string *sql;
    double start = get_microtime();
    
    // Parse parameters (query takes at least 1 parameter: SQL string)
    // Additional parameters are optional and handled by original function
    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_STR(sql)
    ZEND_PARSE_PARAMETERS_END();
    
    // Call original PDO::query
    if (orig_pdo_query_handler) {
        orig_pdo_query_handler(execute_data, return_value);
    } else if (orig_pdo_query_func && orig_pdo_query_func->internal_function.handler) {
        orig_pdo_query_func->internal_function.handler(execute_data, return_value);
    }
    
    double elapsed = (get_microtime() - start) * 1000.0;
    
    // Get row count if available
    long row_count = -1;
    if (return_value && Z_TYPE_P(return_value) == IS_OBJECT) {
        zend_class_entry *ce = Z_OBJCE_P(return_value);
        const char *class_name = ZSTR_VAL(ce->name);
        
        // For PDOStatement, get rowCount() (works for INSERT/UPDATE/DELETE)
        // For SELECT queries, rowCount() returns 0, so we try to fetch and count
        zval row_count_zv;
        zval method_name;
        zval return_zv;
        ZVAL_OBJ(&return_zv, Z_OBJ_P(return_value));
        ZVAL_STRING(&method_name, "rowCount");
        if (call_user_function(EG(function_table), &return_zv, &method_name, &row_count_zv, 0, NULL) == SUCCESS) {
            if (Z_TYPE(row_count_zv) == IS_LONG) {
                row_count = Z_LVAL(row_count_zv);
                
                // If rowCount is 0, it might be a SELECT query
                // Try to fetch all rows and count them
                if (row_count == 0 && sql && (strncasecmp(ZSTR_VAL(sql), "SELECT", 6) == 0)) {
                    zval fetch_all_zv;
                    zval fetch_all_method;
                    ZVAL_STRING(&fetch_all_method, "fetchAll");
                    if (call_user_function(EG(function_table), &return_zv, &fetch_all_method, &fetch_all_zv, 0, NULL) == SUCCESS) {
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
    }
    
    // Log SQL query
    if (sql) {
        php_printf("[OPA SQL Profiling] PDO Query: %s | Time: %.3fms | Rows: %ld\n", 
                   ZSTR_VAL(sql), elapsed, row_count);
        
        // Send SQL query data to agent via record_sql_query
        double duration_seconds = elapsed / 1000.0;
        record_sql_query(ZSTR_VAL(sql), duration_seconds, NULL, "PDO::query", row_count);
    }
}

// PDOStatement::execute hook
PHP_METHOD(PDOStatement, execute) {
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
        php_printf("[OPA SQL Profiling] PDOStatement Execute: %s | Time: %.3fms | Rows: %ld\n", 
                   sql, elapsed, row_count);
        
        // Send SQL query data to agent via record_sql_query
        double duration_seconds = elapsed / 1000.0;
        record_sql_query(sql, duration_seconds, NULL, "PDOStatement::execute", row_count);
    }
}

// Module initialization - registers INI settings and hooks zend_execute_ex for function call interception
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
    
    // Hook zend_execute_ex to intercept all function calls
    // TEMPORARILY: Always hook (hardcode enabled) to avoid OPA_G access crash
        if (zend_execute_ex && !original_zend_execute_ex) {
            original_zend_execute_ex = zend_execute_ex;
            zend_execute_ex = opa_execute_ex;
    }
    
    // Register SQL profiling hooks (lazy registration - try in RINIT as well)
    // MySQLi hook
    orig_mysqli_query_func = zend_hash_str_find_ptr(CG(function_table), "mysqli_query", sizeof("mysqli_query")-1);
    if (orig_mysqli_query_func && orig_mysqli_query_func->type == ZEND_INTERNAL_FUNCTION) {
        // Store original handler
        orig_mysqli_query_handler = orig_mysqli_query_func->internal_function.handler;
        // Replace with our handler
        orig_mysqli_query_func->internal_function.handler = zif_opaphp_mysqli_query;
        php_printf("[OPA] MySQLi query hook registered\n");
    } else {
        php_printf("[OPA] MySQLi query function not found at MINIT\n");
    }
    
    // PDO::query hook
    zend_class_entry *pdo_ce = zend_hash_str_find_ptr(CG(class_table), "PDO", sizeof("PDO")-1);
    if (pdo_ce) {
        orig_pdo_query_func = zend_hash_str_find_ptr(&pdo_ce->function_table, "query", sizeof("query")-1);
        if (orig_pdo_query_func && orig_pdo_query_func->type == ZEND_INTERNAL_FUNCTION) {
            orig_pdo_query_handler = orig_pdo_query_func->internal_function.handler;
            orig_pdo_query_func->internal_function.handler = zim_PDO_query;
            php_printf("[OPA] PDO::query hook registered\n");
        } else {
            php_printf("[OPA] PDO::query method not found at MINIT\n");
        }
    } else {
        php_printf("[OPA] PDO class not found at MINIT\n");
    }
    
    // PDOStatement::execute hook
    zend_class_entry *pdo_stmt_ce = zend_hash_str_find_ptr(CG(class_table), "PDOStatement", sizeof("PDOStatement")-1);
    if (pdo_stmt_ce) {
        orig_pdo_stmt_execute_func = zend_hash_str_find_ptr(&pdo_stmt_ce->function_table, "execute", sizeof("execute")-1);
        if (orig_pdo_stmt_execute_func && orig_pdo_stmt_execute_func->type == ZEND_INTERNAL_FUNCTION) {
            orig_pdo_stmt_execute_handler = orig_pdo_stmt_execute_func->internal_function.handler;
            orig_pdo_stmt_execute_func->internal_function.handler = zim_PDOStatement_execute;
            php_printf("[OPA] PDOStatement::execute hook registered\n");
        } else {
            php_printf("[OPA] PDOStatement::execute method not found at MINIT\n");
        }
    } else {
        php_printf("[OPA] PDOStatement class not found at MINIT\n");
    }
    
    // PHP 8.4: Get CurlHandle class entry pointers for reliable detection
    // NOTE: EG(class_table) is NULL at MINIT time in PHP 8.4, so we defer lookup to RINIT
    // Initialize pointers to NULL - will be set in RINIT when class_table is available
    curl_ce = NULL;
    curl_multi_ce = NULL;
    curl_share_ce = NULL;
    php_printf("[OPA] CurlHandle class lookup deferred to RINIT (class_table not available at MINIT)\n");
    
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

PHP_RINIT_FUNCTION(opa) {
    // RINIT: Only reset per-request state, do NOT install hooks or access globals
    // Following best practices: RINIT should only reset counters and lightweight state
    
    // Try to register SQL hooks lazily if they weren't found at MINIT
    if (!orig_mysqli_query_func) {
        orig_mysqli_query_func = zend_hash_str_find_ptr(CG(function_table), "mysqli_query", sizeof("mysqli_query")-1);
        if (orig_mysqli_query_func && orig_mysqli_query_func->type == ZEND_INTERNAL_FUNCTION) {
            orig_mysqli_query_handler = orig_mysqli_query_func->internal_function.handler;
            orig_mysqli_query_func->internal_function.handler = zif_opaphp_mysqli_query;
        }
    }
    
    if (!orig_pdo_query_func) {
        zend_class_entry *pdo_ce = zend_hash_str_find_ptr(CG(class_table), "PDO", sizeof("PDO")-1);
        if (pdo_ce) {
            orig_pdo_query_func = zend_hash_str_find_ptr(&pdo_ce->function_table, "query", sizeof("query")-1);
            if (orig_pdo_query_func && orig_pdo_query_func->type == ZEND_INTERNAL_FUNCTION) {
                orig_pdo_query_handler = orig_pdo_query_func->internal_function.handler;
                orig_pdo_query_func->internal_function.handler = zim_PDO_query;
            }
        }
    }
    
    if (!orig_pdo_stmt_execute_func) {
        zend_class_entry *pdo_stmt_ce = zend_hash_str_find_ptr(CG(class_table), "PDOStatement", sizeof("PDOStatement")-1);
        if (pdo_stmt_ce) {
            orig_pdo_stmt_execute_func = zend_hash_str_find_ptr(&pdo_stmt_ce->function_table, "execute", sizeof("execute")-1);
            if (orig_pdo_stmt_execute_func && orig_pdo_stmt_execute_func->type == ZEND_INTERNAL_FUNCTION) {
                orig_pdo_stmt_execute_handler = orig_pdo_stmt_execute_func->internal_function.handler;
                orig_pdo_stmt_execute_func->internal_function.handler = zim_PDOStatement_execute;
            }
        }
    }
    
    // Look up curl class entries at RINIT time when class_table is available
    if (!curl_ce && EG(class_table)) {
        curl_ce = zend_hash_str_find_ptr(EG(class_table), "CurlHandle", sizeof("CurlHandle")-1);
        if (curl_ce) {
            php_printf("[OPA] CurlHandle class found at RINIT\n");
        }
    }

    if (!curl_multi_ce && EG(class_table)) {
        curl_multi_ce = zend_hash_str_find_ptr(EG(class_table), "CurlMultiHandle", sizeof("CurlMultiHandle")-1);
        if (curl_multi_ce) {
            php_printf("[OPA] CurlMultiHandle class found at RINIT\n");
        }
    }

    if (!curl_share_ce && EG(class_table)) {
        curl_share_ce = zend_hash_str_find_ptr(EG(class_table), "CurlShareHandle", sizeof("CurlShareHandle")-1);
        if (curl_share_ce) {
            php_printf("[OPA] CurlShareHandle class found at RINIT\n");
        }
    }
    
    // SIMPLE TEST - Write to file to verify code path is executing
    FILE *test_file = fopen("/tmp/opa_rinit_test.log", "a");
    if (test_file) {
        fprintf(test_file, "[OPA RINIT] EXECUTING - timestamp=%ld\n", (long)time(NULL));
        fprintf(test_file, "[OPA RINIT] method=%s | uri=%s | query=%s | content=%d\n",
            SG(request_info).request_method ? SG(request_info).request_method : "NULL",
            SG(request_info).request_uri ? SG(request_info).request_uri : "NULL", 
            SG(request_info).query_string ? SG(request_info).query_string : "NULL",
            SG(request_info).content_length);
        fclose(test_file);
    }
    
    // IMMEDIATE DEBUG - write(2) bypasses PHP buffers and survives shutdown
    char debug_buf[512];
    int len = snprintf(debug_buf, sizeof(debug_buf),
        "[OPA RINIT] method=%s | uri=%s | query=%s | content=%d\n",
        SG(request_info).request_method ? SG(request_info).request_method : "NULL",
        SG(request_info).request_uri ? SG(request_info).request_uri : "NULL", 
        SG(request_info).query_string ? SG(request_info).query_string : "NULL",
        SG(request_info).content_length);
    fprintf(stderr, "%s", debug_buf);  /* stderr - survives shutdown */
    
    // FPM-Specific SAPI Check - PG(http_globals) populated EARLIER than SG() in FPM
    if (sapi_module.name && strstr(sapi_module.name, "fpm")) {
        fprintf(stderr, "[OPA] FPM detected\n");
        
        // Check PG(http_globals) - populated EARLIER than SG() in FPM
        zval *server = &PG(http_globals)[TRACK_VARS_SERVER];
        len = snprintf(debug_buf, sizeof(debug_buf), "[OPA] PG(http_globals) server=%p, type=%d, is_array=%d\n", 
            server, server ? Z_TYPE_P(server) : -1, server && Z_TYPE_P(server) == IS_ARRAY);
        fprintf(stderr, "%s", debug_buf);
        
        if (server && Z_TYPE_P(server) == IS_ARRAY) {
            zval *method = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
            zval *uri = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
            len = snprintf(debug_buf, sizeof(debug_buf), "[OPA] PG method=%p, uri=%p\n", method, uri);
            fprintf(stderr, "%s", debug_buf);
            
            if (method && Z_TYPE_P(method) == IS_STRING) {
                len = snprintf(debug_buf, sizeof(debug_buf), "[OPA] PG method=%s\n", Z_STRVAL_P(method));
                fprintf(stderr, "%s", debug_buf);
            }
            if (uri && Z_TYPE_P(uri) == IS_STRING) {
                len = snprintf(debug_buf, sizeof(debug_buf), "[OPA] PG uri=%s\n", Z_STRVAL_P(uri));
                fprintf(stderr, "%s", debug_buf);
            }
        } else {
            fprintf(stderr, "[OPA] PG(http_globals) NOT available (server=%p)\n", server);
        }
    }
    
    // Reset per-request state
    profiling_active = 1; // Hardcode enabled for now (avoid OPA_G access)
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
            /* TARGETED DEBUG: Verify we're entering HTTP capture block */
            fprintf(stderr, "[OPA RINIT] Entering HTTP capture block (is_cli=%d)\n", is_cli);
            
            // HTTP request - use universal serializer (tries PG first for FPM, then SG for Apache)
            char *req_info = serialize_http_request_json_universal();
            
            /* TARGETED DEBUG: Verify serializer returned something */
            fprintf(stderr, "[OPA RINIT] serialize_http_request_json_universal() returned: %p\n", req_info);
            
            debug_log("[RINIT] serialize_http_request_json_universal() returned: %p, content=%.200s", req_info, req_info ? req_info : "NULL");
            
            // DEBUG - Log the result
            if (req_info) {
                len = snprintf(debug_buf, sizeof(debug_buf), "[OPA RINIT] JSON len=%zu, content=%.200s\n", strlen(req_info), req_info);
                fprintf(stderr, "%s", debug_buf);
            } else {
                fprintf(stderr, "[OPA RINIT] JSON is NULL\n");
            }
            
            if (req_info && strlen(req_info) > 2) {
                /* TARGETED DEBUG: Verify we're storing the data */
                fprintf(stderr, "[OPA RINIT] Storing http_request_json (len=%zu)\n", strlen(req_info));
                
                // Free old value if exists
                if (root_span_http_request_json) {
                    fprintf(stderr, "[OPA RINIT] Freeing old http_request_json\n");
                    free(root_span_http_request_json);
                }
                root_span_http_request_json = req_info;
                fprintf(stderr, "[OPA RINIT] http_request_json stored: %p, content=%.200s\n", 
                        root_span_http_request_json, root_span_http_request_json);
                debug_log("[RINIT] root_span_http_request_json set to: %p, content=%.200s", root_span_http_request_json, root_span_http_request_json ? root_span_http_request_json : "NULL");
                
                // Try to extract method and URI from the JSON or use SG(request_info) as fallback
                const char *method = SG(request_info).request_method ? SG(request_info).request_method : "GET";
                const char *uri = SG(request_info).request_uri ? SG(request_info).request_uri : "/";
                
                // Also try PG(http_globals) for method/URI if SG doesn't have them
                zval *server = &PG(http_globals)[TRACK_VARS_SERVER];
                if (server && Z_TYPE_P(server) == IS_ARRAY) {
                    zval *method_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_METHOD", sizeof("REQUEST_METHOD")-1);
                    zval *uri_zv = zend_hash_str_find(Z_ARRVAL_P(server), "REQUEST_URI", sizeof("REQUEST_URI")-1);
                    if (method_zv && Z_TYPE_P(method_zv) == IS_STRING) {
                        method = Z_STRVAL_P(method_zv);
                    }
                    if (uri_zv && Z_TYPE_P(uri_zv) == IS_STRING) {
                        uri = Z_STRVAL_P(uri_zv);
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
    if (!global_collector) {
        global_collector = opa_collector_init();
    }
    if (global_collector) {
        opa_collector_start(global_collector);
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

PHP_RSHUTDOWN_FUNCTION(opa) {
    // Skip logging in CLI mode
    int is_cli = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0);
    
    // SIMPLE TEST - Verify RSHUTDOWN is executing
    FILE *test_file = fopen("/tmp/opa_rinit_test.log", "a");
    if (test_file) {
        fprintf(test_file, "[OPA RSHUTDOWN] EXECUTING - timestamp=%ld\n", (long)time(NULL));
        fclose(test_file);
    }
    
    debug_log("[RSHUTDOWN] START - is_cli=%d, collector=%p", is_cli, global_collector);
    
    // Disable profiling first to stop hook processing ()
    profiling_active = 0;
    
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
        int status = 1; // Finalize status
        
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
            root_span_cli_args_json, root_span_http_request_json, root_span_http_response_json
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
        // Convert malloc'd string to emalloc'd for send_message_direct
        // (send_message_direct expects emalloc'd and will efree it)
        char *msg_copy = estrdup(json_str);
        if (msg_copy) {
        free(json_str); // Free malloc'd version
        send_message_direct(msg_copy, 1);
        } else {
            char fields[128];
            snprintf(fields, sizeof(fields), "{\"message_size\":%zu}", strlen(json_str));
            log_error("Failed to allocate memory for span message", "estrdup failed", fields);
            free(json_str);
        }
    } else if (json_str) {
        free(json_str); // Free if not sent
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
