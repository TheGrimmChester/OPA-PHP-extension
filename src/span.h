#ifndef SPAN_H
#define SPAN_H

#include "opa.h"

// Span management functions
span_context_t* create_span_context(const char *span_id, const char *trace_id, const char *name);
void free_span_context(span_context_t *span);
// Set shutdown flag to avoid zval access during MSHUTDOWN
void set_span_shutdown_flag(int flag);
// Produce span JSON from span pointer - safe BEFORE fastcgi_finish_request() only
char* produce_span_json(span_context_t *span); // Returns char* (malloc'd), caller must free
// Produce span JSON from individual values (not from span pointer) - safe after fastcgi_finish_request()
char* produce_span_json_from_values(
    const char *trace_id, const char *span_id, const char *parent_id, const char *name,
    const char *url_scheme, const char *url_host, const char *url_path,
    long start_ts, long end_ts, int cpu_ms, int status, const char *dumps_json,
    const char *cli_args_json, const char *http_request_json, const char *http_response_json
); // Returns char* (malloc'd), caller must free

// Produce child span JSON from call node - safe after fastcgi_finish_request()
// Returns NULL if call node is not significant (no SQL/HTTP/cache/Redis and duration <= 10ms)
char* produce_child_span_json_from_call_node(
    call_node_t *call, const char *trace_id, const char *parent_span_id, long root_start_ts
); // Returns char* (malloc'd), caller must free, or NULL if not significant

#endif /* SPAN_H */

