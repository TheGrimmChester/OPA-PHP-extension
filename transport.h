#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "opa.h"

// Transport functions
void opa_finish_request(void);
void send_message_direct(char *msg, int compress);

#endif /* TRANSPORT_H */

