#ifndef SERIALIZE_H
#define SERIALIZE_H

#include "opa.h"

// JSON serialization functions
void json_escape_string(smart_string *buf, const char *str, size_t len);
void serialize_zval_json(smart_string *buf, zval *zv);
void serialize_zval_text(smart_string *buf, zval *zv);
void serialize_call_node_json(smart_string *buf, call_node_t *call);
void serialize_call_node_json_recursive(smart_string *buf, call_node_t *call);
void serialize_call_stack_json(smart_string *buf, zval *stack);
void serialize_call_stack_from_root(smart_string *buf);
void serialize_all_sql_queries(smart_string *buf);

#endif /* SERIALIZE_H */

