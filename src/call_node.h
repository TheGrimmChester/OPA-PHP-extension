#ifndef CALL_NODE_H
#define CALL_NODE_H

#include "opa.h"

// Call node management functions
void php_opa_begin_request(zend_execute_data *execute_data);
void php_opa_end_request(zend_execute_data *execute_data, zval *return_value);
void record_sql_query(const char *sql, double duration, zval *params, const char *query_type, int rows_affected);
void record_http_request(const char *url, const char *method, int status_code, size_t bytes_sent, size_t bytes_received, double duration, const char *error);
void record_http_request_enhanced(const char *url, const char *method, int status_code, size_t bytes_sent, size_t bytes_received, double duration, const char *error, const char *uri_path, const char *query_string, const char *request_headers, const char *response_headers, size_t response_size, size_t request_size, double dns_time, double connect_time, double total_time);
void record_cache_operation(const char *key, const char *operation, int hit, double duration, size_t data_size, const char *cache_type);
void record_redis_operation(const char *command, const char *key, int hit, double duration, const char *error);

#endif /* CALL_NODE_H */

