#include "transport.h"

// Cached agent address to avoid repeated DNS lookups (thread-safe with mutex)
static struct sockaddr_in cached_agent_addr = {0};
static char *cached_agent_host = NULL;
static int cached_agent_port = 0;
static pthread_mutex_t agent_addr_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int agent_addr_cached = 0;

// Pre-resolve agent address in RINIT (before observer callbacks) to avoid DNS calls from unsafe contexts
void pre_resolve_agent_address(void) {
    fprintf(stderr, "[OPA Pre-resolve] Starting pre-resolution\n");
    fflush(stderr);
    
    if (!OPA_G(enabled) || !OPA_G(socket_path)) {
        fprintf(stderr, "[OPA Pre-resolve] Early return: enabled=%d, socket_path=%p\n", OPA_G(enabled), OPA_G(socket_path));
        fflush(stderr);
        return;
    }
    
    const char *sock_path = OPA_G(socket_path);
    int is_unix_socket = (sock_path[0] == '/');
    
    fprintf(stderr, "[OPA Pre-resolve] socket_path=%s, is_unix=%d\n", sock_path, is_unix_socket);
    fflush(stderr);
    
    if (is_unix_socket) {
        // Unix socket - no DNS resolution needed
        fprintf(stderr, "[OPA Pre-resolve] Unix socket, skipping DNS\n");
        fflush(stderr);
        return;
    }
    
    // Parse host:port
    char *path_copy = estrdup(sock_path);
    char *colon = strchr(path_copy, ':');
    if (!colon) {
        efree(path_copy);
        return;
    }
    
    *colon = '\0';
    char *host = path_copy;
    char *port_str = colon + 1;
    int port = atoi(port_str);
    
    if (port <= 0 || port > 65535) {
        efree(path_copy);
        return;
    }
    
    // Check if already cached
    pthread_mutex_lock(&agent_addr_cache_mutex);
    if (agent_addr_cached && cached_agent_host && 
        strcmp(cached_agent_host, host) == 0 && cached_agent_port == port) {
        pthread_mutex_unlock(&agent_addr_cache_mutex);
        efree(path_copy);
        return; // Already cached
    }
    pthread_mutex_unlock(&agent_addr_cache_mutex);
    
    // Try to parse as IP address first
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_aton(host, &addr.sin_addr) == 0) {
        // Not an IP address, resolve hostname using getaddrinfo (safe in RINIT context)
        struct addrinfo hints, *result, *rp;
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        char port_str_buf[16];
        snprintf(port_str_buf, sizeof(port_str_buf), "%d", port);
        
        int gai_result = getaddrinfo(host, port_str_buf, &hints, &result);
        if (gai_result == 0) {
            // Use first result
            for (rp = result; rp != NULL; rp = rp->ai_next) {
                if (rp->ai_family == AF_INET) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
                    memcpy(&addr.sin_addr, &sin->sin_addr, sizeof(addr.sin_addr));
                    break;
                }
            }
            freeaddrinfo(result);
            
            if (rp != NULL) {
                // Cache the resolved address
                pthread_mutex_lock(&agent_addr_cache_mutex);
                if (cached_agent_host) {
                    efree(cached_agent_host);
                }
                cached_agent_host = estrdup(host);
                cached_agent_port = port;
                memcpy(&cached_agent_addr.sin_addr, &addr.sin_addr, sizeof(cached_agent_addr.sin_addr));
                agent_addr_cached = 1;
                pthread_mutex_unlock(&agent_addr_cache_mutex);
            }
        }
    } else {
        // IP address parsed successfully, cache it
        pthread_mutex_lock(&agent_addr_cache_mutex);
        if (cached_agent_host) {
            efree(cached_agent_host);
        }
        cached_agent_host = estrdup(host);
        cached_agent_port = port;
        memcpy(&cached_agent_addr.sin_addr, &addr.sin_addr, sizeof(cached_agent_addr.sin_addr));
        agent_addr_cached = 1;
        pthread_mutex_unlock(&agent_addr_cache_mutex);
    }
    
    efree(path_copy);
}

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
                // First check cache to avoid repeated DNS lookups (which can crash in observer callbacks)
                int use_cached = 0;
                pthread_mutex_lock(&agent_addr_cache_mutex);
                if (agent_addr_cached && cached_agent_host && 
                    strcmp(cached_agent_host, host) == 0 && cached_agent_port == port) {
                    // Use cached address
                    memcpy(&addr.sin_addr, &cached_agent_addr.sin_addr, sizeof(addr.sin_addr));
                    use_cached = 1;
                }
                pthread_mutex_unlock(&agent_addr_cache_mutex);
                
                if (!use_cached) {
                    // Try to parse as IP address first
                    if (inet_aton(host, &addr.sin_addr) == 0) {
                        // If inet_aton fails, check if we have a cached address (pre-resolved in RINIT)
                        // If no cache, fail gracefully instead of calling getaddrinfo from unsafe context
                        pthread_mutex_lock(&agent_addr_cache_mutex);
                        int cache_available = agent_addr_cached && cached_agent_host;
                        pthread_mutex_unlock(&agent_addr_cache_mutex);
                        
                        if (!cache_available) {
                            // No cache available - this might be called from unsafe context
                            // Fail gracefully instead of calling getaddrinfo
                            debug_log("[SEND] Cannot resolve host (no cache, unsafe context): %s", host);
                            close(sock);
                            efree(path_copy);
                            if (msg) efree(msg);
                            return;
                        }
                        
                        // Cache exists - use it (should be for same host/port if pre-resolved correctly)
                        pthread_mutex_lock(&agent_addr_cache_mutex);
                        if (cached_agent_host) {
                            memcpy(&addr.sin_addr, &cached_agent_addr.sin_addr, sizeof(addr.sin_addr));
                        } else {
                            pthread_mutex_unlock(&agent_addr_cache_mutex);
                            debug_log("[SEND] Cache corrupted - cannot resolve host: %s", host);
                            close(sock);
                            efree(path_copy);
                            if (msg) efree(msg);
                            return;
                        }
                        pthread_mutex_unlock(&agent_addr_cache_mutex);
                    } else {
                        // IP address parsed successfully, cache it
                        pthread_mutex_lock(&agent_addr_cache_mutex);
                        if (cached_agent_host) {
                            efree(cached_agent_host);
                        }
                        cached_agent_host = estrdup(host);
                        cached_agent_port = port;
                        memcpy(&cached_agent_addr.sin_addr, &addr.sin_addr, sizeof(cached_agent_addr.sin_addr));
                        agent_addr_cached = 1;
                        pthread_mutex_unlock(&agent_addr_cache_mutex);
                    }
                }
                
                // Check if address resolution succeeded
                if (addr.sin_addr.s_addr == 0) {
                    debug_log("[SEND] Failed to resolve host address: %s", host);
                    close(sock);
                    sock = -1;
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
        // NOTE: Do NOT call log_info() here - it would cause infinite recursion since log_info calls send_message_direct
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
                // NOTE: Do NOT call log_error() here - it would cause infinite recursion since log_error calls send_message_direct
                debug_log("[SEND] Error: %s", error_msg);
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
            // NOTE: Do NOT call log_error() here - it would cause infinite recursion since log_error calls send_message_direct
            debug_log("[SEND] Error: %s", error_msg);
            close(sock);
        } else {
            debug_log("[SEND] Failed to create socket for %s: %s", is_unix_socket ? "Unix socket" : "TCP", sock_path);
        }
    }
    
    if (sock >= 0) {
        close(sock);
    }
    
    // Free the message
    // NOTE: During shutdown (RSHUTDOWN/MSHUTDOWN), PHP's memory manager may be shutting down
    // In that case, efree() can cause heap corruption. However, since we're using emalloc/efree
    // consistently, and the message was allocated with emalloc, we should use efree.
    // If heap corruption occurs, it's likely due to other issues (like accessing destroyed zvals).
    // For now, we'll free it - if issues persist, we may need to track shutdown state.
    if (final_msg) {
        efree(final_msg);
    }
}

