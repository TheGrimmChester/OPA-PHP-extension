#include "transport.h"

// Finish request to client BEFORE sending data
// This ensures the client receives the response immediately
void opa_finish_request(void) {
    // Skip in CLI mode
    int is_cli = (sapi_module.name && strcmp(sapi_module.name, "cli") == 0);
    if (is_cli) {
        return;
    }
    
    // Call fastcgi_finish_request() if available (for FastCGI/FPM)
    zval function, retval;
    ZVAL_UNDEF(&function);
    ZVAL_UNDEF(&retval);
    
    ZVAL_STRING(&function, "fastcgi_finish_request");
    
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;
    
    if (zend_fcall_info_init(&function, 0, &fci, &fcc, NULL, NULL) == SUCCESS) {
        fci.size = sizeof(fci);
        ZVAL_UNDEF(&fci.function_name);
        fci.object = NULL;
        fci.param_count = 0;
        fci.params = NULL;
        fci.retval = &retval;
        
        int call_result = zend_call_function(&fci, &fcc);
        if (call_result == SUCCESS) {
            zval_dtor(&retval);
        }
    }
    
    zval_dtor(&function);
}

// Send message directly to socket (synchronous, no threads)
void send_message_direct(char *msg, int compress) {
    if (!OPA_G(enabled)) {
        debug_log("[SEND] Extension disabled, not sending");
        if (msg) efree(msg);
        return;
    }
    if (!msg) {
        debug_log("[SEND] Message is NULL, not sending");
        return;
    }
    
    // Apply sampling rate
    double rate = OPA_G(sampling_rate);
    if (rate < 1.0 && ((double)rand() / RAND_MAX) > rate) {
        efree(msg);
        return;
    }
    
    size_t msg_len = strlen(msg);
    char *final_msg = msg;
    size_t final_len = msg_len;
    
#if LZ4_ENABLED
    // Compress if enabled and message is large enough
    if (compress && msg_len > 1024) {
        int max_compressed = LZ4_compressBound(msg_len);
        char *compressed = emalloc(max_compressed + strlen(COMPRESSION_HEADER) + sizeof(size_t) + 1);
        memcpy(compressed, COMPRESSION_HEADER, strlen(COMPRESSION_HEADER));
        memcpy(compressed + strlen(COMPRESSION_HEADER), &msg_len, sizeof(size_t));
        
        int compressed_size = LZ4_compress_HC(msg, compressed + strlen(COMPRESSION_HEADER) + sizeof(size_t), msg_len, max_compressed, LZ4HC_CLEVEL_DEFAULT);
        if (compressed_size > 0) {
            efree(msg);
            final_msg = compressed;
            final_len = strlen(COMPRESSION_HEADER) + sizeof(size_t) + compressed_size;
        } else {
            efree(compressed);
        }
    }
#endif
    
    // Detect transport type: Unix socket if path starts with '/', otherwise TCP/IP
    const char *sock_path = OPA_G(socket_path) ? OPA_G(socket_path) : "/var/run/opa.sock";
    int is_unix_socket = (sock_path[0] == '/');
    
    int sock = -1;
    int conn_result = -1;
    
    if (is_unix_socket) {
        // Unix socket transport
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path)-1);
            conn_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        }
    } else {
        // TCP/IP transport (format: host:port)
        char *host = NULL;
        char *port_str = NULL;
        int port = 0;
        
        // Parse host:port
        char *path_copy = estrdup(sock_path);
        char *colon = strchr(path_copy, ':');
        if (colon != NULL) {
            *colon = '\0';
            host = path_copy;
            port_str = colon + 1;
            port = atoi(port_str);
        } else {
            // If no colon, assume it's just a port (default to localhost)
            port_str = path_copy;
            port = atoi(port_str);
            host = "127.0.0.1";
        }
        
        if (port > 0 && port <= 65535) {
            sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                
                // Parse host address
                if (inet_aton(host, &addr.sin_addr) == 0) {
                    // If inet_aton fails, try to resolve hostname
                    struct hostent *he = gethostbyname(host);
                    if (he != NULL && he->h_addr_list[0] != NULL) {
                        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
                    } else {
                        debug_log("[SEND] Failed to resolve host: %s", host);
                        close(sock);
                        sock = -1;
                    }
                }
                
                if (sock >= 0) {
                    conn_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
                }
            }
        } else {
            debug_log("[SEND] Invalid port number: %d", port);
        }
        
        if (path_copy) {
            efree(path_copy);
        }
    }
    
    if (sock >= 0 && conn_result == 0) {
        debug_log("[SEND] Connected to %s, sending %zu bytes", is_unix_socket ? "Unix socket" : "TCP", final_len);
        char fields[256];
        if (is_unix_socket) {
            snprintf(fields, sizeof(fields), "{\"socket_path\":\"%s\",\"bytes\":%zu}", sock_path, final_len);
        } else {
            snprintf(fields, sizeof(fields), "{\"tcp_address\":\"%s\",\"bytes\":%zu}", sock_path, final_len);
        }
        log_info("Connected to agent", fields);
        size_t sent = 0;
        while (sent < final_len) {
            ssize_t w = write(sock, final_msg + sent, final_len - sent);
            if (w <= 0) {
                debug_log("[SEND] Write failed or incomplete: w=%zd, sent=%zu/%zu", w, sent, final_len);
                char error_msg[256];
                char fields[512];
                snprintf(error_msg, sizeof(error_msg), "Socket write failed: w=%zd, sent=%zu/%zu", w, sent, final_len);
                if (is_unix_socket) {
                    snprintf(fields, sizeof(fields), "{\"bytes_written\":%zd,\"bytes_sent\":%zu,\"bytes_total\":%zu,\"errno\":%d,\"transport\":\"unix\"}", 
                            w, sent, final_len, errno);
                } else {
                    snprintf(fields, sizeof(fields), "{\"bytes_written\":%zd,\"bytes_sent\":%zu,\"bytes_total\":%zu,\"errno\":%d,\"transport\":\"tcp\"}", 
                            w, sent, final_len, errno);
                }
                log_error("Failed to write to agent", error_msg, fields);
                break;
            }
            sent += w;
        }
        debug_log("[SEND] Sent %zu/%zu bytes", sent, final_len);
    } else {
        if (sock >= 0) {
            debug_log("[SEND] Failed to connect to %s: %s (errno=%d)", is_unix_socket ? "Unix socket" : "TCP", sock_path, errno);
            char error_msg[256];
            char fields[512];
            snprintf(error_msg, sizeof(error_msg), "Failed to connect to %s: %s (errno=%d)", is_unix_socket ? "Unix socket" : "TCP", sock_path, errno);
            if (is_unix_socket) {
                snprintf(fields, sizeof(fields), "{\"socket_path\":\"%s\",\"errno\":%d,\"errno_str\":\"%s\",\"transport\":\"unix\"}", 
                        sock_path, errno, strerror(errno));
            } else {
                snprintf(fields, sizeof(fields), "{\"tcp_address\":\"%s\",\"errno\":%d,\"errno_str\":\"%s\",\"transport\":\"tcp\"}", 
                        sock_path, errno, strerror(errno));
            }
            log_error("Failed to connect to agent", error_msg, fields);
            close(sock);
        } else {
            debug_log("[SEND] Failed to create socket for %s: %s", is_unix_socket ? "Unix socket" : "TCP", sock_path);
        }
    }
    
    if (sock >= 0) {
        close(sock);
    }
    
    efree(final_msg);
}

