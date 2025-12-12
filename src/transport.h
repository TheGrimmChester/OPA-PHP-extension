#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "opa.h"

// Transport functions
void opa_finish_request(void);
void send_message_direct(char *msg, int compress);
void pre_resolve_agent_address(void); // Pre-resolve agent address in RINIT to avoid DNS calls from unsafe contexts

#endif /* TRANSPORT_H */

