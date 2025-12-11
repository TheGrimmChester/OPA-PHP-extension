#ifndef OPA_H
#define OPA_H

#include "php.h"
#include "SAPI.h"
#include "zend.h"
#include "zend_observer.h"
#include "zend_interfaces.h"
#include "zend_smart_string.h"
#include "zend_API.h"
#include "zend_execute.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#define LZ4_ENABLED 1
#else
#define LZ4_ENABLED 0
#endif

// Constants
#define MSG_MAX 1048576
#define MAX_STACK_DEPTH 50
#define COMPRESSION_HEADER "LZ4"
#define OPA_CALL_NODE_MAGIC 0x4F504100  // "OPA\0"

// Module globals structure
ZEND_BEGIN_MODULE_GLOBALS(opa)
    zend_bool enabled;
    double sampling_rate;
    char *socket_path;
    zend_long full_capture_threshold_ms;
    zend_long stack_depth;
    zend_long buffer_size;
    zend_bool collect_internal_functions;
    zend_bool debug_log_enabled; // Enable/disable debug logging
    char *organization_id;
    char *project_id;
    char *service;
    char *language;
    char *language_version;
    char *framework;
    char *framework_version;
    zend_bool track_errors; // Enable/disable error tracking
    zend_bool track_logs; // Enable/disable log tracking
    char *log_levels; // Comma-separated list: critical,error,warning
    zend_bool expand_spans; // 1 = multiple spans (default), 0 = full span
ZEND_END_MODULE_GLOBALS(opa)

// Declare extern for other files - actual declaration is in opa.c
extern zend_opa_globals opa_globals;

// Macro helper for accessing globals
#define OPA_G(v) (opa_globals.v)

// Call node structure for call stack tracking
typedef struct call_node {
    unsigned int magic; // Magic number for validation
    char *call_id;
    char *function_name;
    char *class_name;
    char *file;
    int line;
    double start_time;
    double end_time;
    double start_cpu_time;
    double end_cpu_time;
    size_t start_memory;
    size_t end_memory;
    size_t start_bytes_sent;
    size_t end_bytes_sent;
    size_t start_bytes_received;
    size_t end_bytes_received;
    char *parent_id;
    zval *children;
    int depth;
    int function_type; // 0=user, 1=internal, 2=method
    zval *sql_queries; // Array of SQL queries executed in this call
    zval *http_requests; // Array of HTTP requests (cURL) executed in this call
    zval *cache_operations; // Array of cache operations (APCu, Symfony Cache) executed in this call
    zval *redis_operations; // Array of Redis operations executed in this call
    struct call_node *next; // Next in calls list
    struct call_node *stack_next; // Next in call stack (for unlimited depth)
} call_node_t;

// Span context structure
typedef struct {
    char *span_id;
    char *trace_id;
    char *parent_id;
    long start_ts;
    long end_ts;
    char *name;
    char *url_scheme;  // http or https
    char *url_host;    // hostname:port
    char *url_path;    // /path/to/resource
    zval *tags;
    zval *net;
    zval *sql;
    zval *http; // HTTP requests (cURL) for span-level aggregation
    zval *stack;
    zval *dumps; // Array of dump entries (var_dump-like data)
    int cpu_ms;
    int status;
    int is_manual;
} span_context_t;

// Collector structure
#define OPA_COLLECTOR_MAGIC 0x4F504100  // "OPA\0"

typedef struct _opa_collector_t {
    unsigned int magic; // Magic number for integrity checking
    call_node_t *calls; // Linked list of all calls
    call_node_t *call_stack_top; // Top of call stack (linked list, no depth limit)
    int call_stack_depth; // Current depth (for debugging, no limit enforced) // Current stack depth
    int call_depth; // Current call depth (for statistics)
    int call_count; // Total number of calls tracked
    zend_bool active; // Whether collector is active
    double start_time; // Request start time
    double end_time; // Request end time
    size_t start_memory; // Request start memory
    size_t end_memory; // Request end memory
    zval *global_sql_queries; // Global SQL queries array (independent of call nodes)
    pthread_mutex_t global_sql_mutex; // Mutex for thread-safe access to global_sql_queries
} opa_collector_t;

// Global state (declared in opa.c)
extern HashTable *active_spans;
extern pthread_mutex_t active_spans_mutex;
// Root span data stored in malloc'd memory to avoid emalloc segfaults
extern char *root_span_trace_id;
extern char *root_span_span_id;
extern char *root_span_parent_id;
extern char *root_span_name;
extern char *root_span_url_scheme;
extern char *root_span_url_host;
extern char *root_span_url_path;
extern char *root_span_cli_args_json;
extern char *root_span_http_request_json;
extern char *root_span_http_response_json;
extern long root_span_start_ts;
extern long root_span_end_ts;
extern int root_span_cpu_ms;
extern int root_span_status;
extern zval *root_span_dumps; // Root span dumps array
extern pthread_mutex_t root_span_data_mutex;
extern int profiling_active;
extern opa_collector_t *global_collector;
extern size_t network_bytes_sent_total;
extern size_t network_bytes_received_total;
extern pthread_mutex_t network_mutex;

// Helper functions (declared in opa.c)
char* generate_id(void);
long get_timestamp_ms(void);
double get_time_seconds(void);
size_t get_memory_usage(void);
double get_cpu_time(void);
size_t get_bytes_sent(void);
size_t get_bytes_received(void);
void add_bytes_sent(size_t bytes);
void add_bytes_received(size_t bytes);
void debug_log(const char *msg, ...);
void log_error(const char *message, const char *error, const char *fields_json);
void log_warn(const char *message, const char *fields_json);
void log_info(const char *message, const char *fields_json);
HashTable* get_active_spans(void);
int is_pdo_method(zend_execute_data *execute_data);
int is_curl_function(zend_execute_data *execute_data);
int is_apcu_function(zend_execute_data *execute_data);
int is_symfony_cache_method(zend_execute_data *execute_data);
int is_redis_method(zend_execute_data *execute_data);
void record_http_request(const char *url, const char *method, int status_code, size_t bytes_sent, size_t bytes_received, double duration, const char *error);
void record_cache_operation(const char *key, const char *operation, int hit, double duration, size_t data_size, const char *cache_type);
void record_redis_operation(const char *command, const char *key, int hit, double duration, const char *error);

// Error tracking functions
void opa_init_error_tracking(void);
void opa_cleanup_error_tracking(void);
void send_log_to_agent(const char *level, const char *message, const char *file, int line);
void send_error_to_agent(int error_type, const char *error_message, const char *file, int line, zval *stack_trace, int *exception_code);

// Collector functions
opa_collector_t* opa_collector_init(void);
void opa_collector_start(opa_collector_t *collector);
void opa_collector_stop(opa_collector_t *collector);
void opa_collector_free(opa_collector_t *collector);

// Call tracking functions
char* opa_enter_function(const char *function_name, const char *class_name, const char *file, int line, int function_type);
void opa_exit_function(const char *call_id);

// zend_execute_ex hook
extern void (*original_zend_execute_ex)(zend_execute_data *execute_data);
void opa_execute_ex(zend_execute_data *execute_data);

// Custom INI update handler
PHP_INI_MH(OnUpdateSamplingRate);

#endif /* OPA_H */

