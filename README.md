# OpenProfilingAgent PHP Extension

A high-performance PHP extension for Application Performance Monitoring (APM) that provides comprehensive profiling, tracing, and observability capabilities for PHP applications.

## Overview

OpenProfilingAgent PHP Extension is a native C extension for PHP 8.0-8.5 that enables automatic and manual instrumentation of PHP applications. It collects detailed performance metrics, traces function calls, profiles SQL queries and HTTP requests, tracks errors, and sends data to the OpenProfilingAgent collector via Unix socket or TCP/IP.

## Features

### Core Capabilities

- **Automatic Function Profiling**: Tracks all function calls with precise timing, CPU usage, and memory metrics
- **Call Stack Tracking**: Unlimited depth call stack with parent-child relationships
- **SQL Profiling**: Automatic instrumentation of PDO and MySQLi queries with query text, timing, and row counts
- **HTTP/cURL Profiling**: Tracks outgoing HTTP requests with URL, method, status codes, and response times
- **Error Tracking**: Captures PHP errors, exceptions, and fatal errors with stack traces
- **Log Tracking**: Monitors error_log() calls with configurable log levels
- **Span Management**: Automatic root spans for web requests and CLI commands, plus manual span creation
- **Network Metrics**: Tracks bytes sent/received across the request lifecycle
- **LZ4 Compression**: Optional compression for efficient data transmission

### Advanced Features

- **Runtime Configuration**: Enable/disable profiling and adjust settings via environment variables or PHP functions
- **Sampling Rate**: Configurable sampling to reduce overhead in high-traffic environments
- **Full Capture Threshold**: Automatically enable full profiling for slow requests
- **Manual Spans**: Programmatic span creation for custom instrumentation
- **Tags and Metadata**: Add custom tags and context to spans
- **Dump Support**: Capture variable dumps (var_dump-like) within spans
- **Multi-Transport**: Support for both Unix socket and TCP/IP communication
- **Thread-Safe**: Built with thread safety for PHP-FPM environments

## Requirements

- **PHP**: 8.0, 8.1, 8.2, 8.3, 8.4, or 8.5
- **Build Tools**: `phpize`, `autoconf`, `gcc`, `make`, `libtool`, `pkg-config`
- **Libraries**: `liblz4-dev` (optional, for compression)
- **Extensions**: `sockets` (for network communication)
- **Runtime**: PHP-FPM, CLI, or Apache with mod_php

## Installation

### Docker (Recommended)

The easiest way to use the extension is via Docker:

```bash
# Build the Docker image
docker-compose build

# Or build with a specific PHP version
docker build --build-arg PHP_VERSION=8.4 -t my-php-app .
```

### Manual Build

1. **Install dependencies**:

```bash
# Debian/Ubuntu
sudo apt-get install php-dev autoconf gcc make libtool pkg-config liblz4-dev

# RHEL/CentOS
sudo yum install php-devel autoconf gcc make libtool pkgconfig lz4-devel
```

2. **Build the extension**:

```bash
# Clone or navigate to the extension directory
cd php-extension

# Build
phpize
./configure --enable-opa
make
sudo make install
```

3. **Enable the extension**:

Add to your `php.ini`:
```ini
extension=opa.so
```

4. **Verify installation**:

```bash
php -m | grep opa
php -i | grep opa
```

## Configuration

### Environment Variables

The extension can be configured via environment variables (recommended for Docker/Kubernetes):

```bash
# Enable/disable profiling
OPA_ENABLED=1

# Sampling rate (0.0 to 1.0)
OPA_SAMPLING_RATE=0.1

# Agent connection (Unix socket or TCP/IP)
OPA_SOCKET_PATH=/var/run/opa.sock          # Unix socket
# OPA_SOCKET_PATH=agent:9090               # TCP/IP format: host:port

# Service identification
OPA_ORGANIZATION_ID=my-org
OPA_PROJECT_ID=my-project
OPA_SERVICE=api-service

# Framework information
OPA_FRAMEWORK=symfony
OPA_FRAMEWORK_VERSION=7.0

# Profiling settings
OPA_FULL_CAPTURE_THRESHOLD_MS=100
OPA_STACK_DEPTH=20
OPA_COLLECT_INTERNAL_FUNCTIONS=1

# Error and log tracking
OPA_TRACK_ERRORS=1
OPA_TRACK_LOGS=1
OPA_LOG_LEVELS=critical,error,warning

# Debug logging
OPA_DEBUG_LOG=0
```

### PHP INI Configuration

Alternatively, configure via PHP INI files:

```ini
; Enable/disable profiling
opa.enabled=1

; Sampling rate (0.0 to 1.0)
opa.sampling_rate=0.1

; Agent connection
opa.socket_path=/var/run/opa.sock

; Service identification
opa.organization_id=my-org
opa.project_id=my-project
opa.service=api-service

; Framework
opa.framework=symfony
opa.framework_version=7.0

; Profiling settings
opa.full_capture_threshold_ms=100
opa.stack_depth=20
opa.collect_internal_functions=1

; Error and log tracking
opa.track_errors=1
opa.track_logs=1
opa.log_levels=critical,error,warning

; Debug logging
opa.debug_log=0
```

See [RUNTIME_CONFIGURATION.md](RUNTIME_CONFIGURATION.md) for detailed configuration options and examples.

## Usage

### Automatic Profiling

Once installed and configured, the extension automatically profiles:

- **Web Requests**: Creates root spans for HTTP requests with URL, method, headers, and response status
- **CLI Commands**: Creates root spans for command-line scripts with arguments
- **Function Calls**: Tracks all function calls with timing and stack information
- **SQL Queries**: Automatically instruments PDO and MySQLi queries
- **HTTP Requests**: Tracks cURL and other HTTP client calls
- **Errors**: Captures PHP errors, exceptions, and fatal errors

### Manual Spans

Create custom spans for important operations:

```php
<?php
// Start a manual span
$spanId = opa_start_span('payment_processing', [
    'user_id' => $userId,
    'order_id' => $orderId
]);

try {
    // Your business logic
    process_payment($orderId);
    
    opa_add_tag($spanId, 'status', 'success');
    opa_add_tag($spanId, 'amount', $amount);
} catch (Exception $e) {
    opa_add_tag($spanId, 'status', 'error');
    opa_add_tag($spanId, 'error', $e->getMessage());
    throw $e;
} finally {
    // End the span
    opa_end_span($spanId);
}
```

### Runtime Control

Enable or disable profiling programmatically:

```php
<?php
// Enable profiling for this request
opa_enable();

// Check if profiling is enabled
if (opa_is_enabled()) {
    // Profiling is active
}

// Disable profiling (e.g., for health checks)
if ($_SERVER['REQUEST_URI'] === '/health') {
    opa_disable();
}
```

### Error Tracking

Errors are automatically tracked when `opa.track_errors=1`. You can also manually track errors:

```php
<?php
try {
    risky_operation();
} catch (Exception $e) {
    opa_track_error($e);
    // Error is sent to the agent with full context
}
```

### Variable Dumps

Capture variable dumps within spans:

```php
<?php
$spanId = opa_start_span('debug_operation');

// Dump variables (similar to var_dump)
opa_dump($variable1, $variable2, $variable3);

opa_end_span($spanId);
```

## API Reference

### Functions

#### `opa_start_span(string $name, array $tags = []): string`

Creates a new manual span and returns its span ID.

**Parameters:**
- `$name` (string): Span name
- `$tags` (array, optional): Key-value pairs for span metadata

**Returns:** Span ID (string)

**Example:**
```php
$spanId = opa_start_span('database_operation', ['table' => 'users']);
```

#### `opa_end_span(string $spanId): bool`

Finalizes a manual span and sends it to the agent.

**Parameters:**
- `$spanId` (string): Span ID returned by `opa_start_span()`

**Returns:** `true` on success, `false` if span not found

**Example:**
```php
opa_end_span($spanId);
```

#### `opa_add_tag(string $spanId, string $key, string $value): bool`

Adds a tag to an existing span.

**Parameters:**
- `$spanId` (string): Span ID
- `$key` (string): Tag key
- `$value` (string): Tag value

**Returns:** `true` on success, `false` if span not found

**Example:**
```php
opa_add_tag($spanId, 'user_id', '12345');
```

#### `opa_set_parent(string $spanId, string $parentId): bool`

Sets the parent span for a manual span.

**Parameters:**
- `$spanId` (string): Child span ID
- `$parentId` (string): Parent span ID

**Returns:** `true` on success, `false` if span not found

#### `opa_dump(mixed ...$vars): void`

Captures variable dumps (similar to `var_dump`) within the current span context.

**Parameters:**
- `...$vars`: Variable number of arguments to dump

**Example:**
```php
opa_dump($user, $order, $payment);
```

#### `opa_enable(): void`

Enables profiling for the current request.

#### `opa_disable(): void`

Disables profiling for the current request.

#### `opa_is_enabled(): bool`

Returns whether profiling is currently enabled.

**Returns:** `true` if enabled, `false` otherwise

#### `opa_track_error(Throwable $exception): void`

Manually track an exception or error.

**Parameters:**
- `$exception` (Throwable): Exception or Error object

## Testing

The extension includes comprehensive end-to-end tests:

### Running Tests

```bash
# SQL profiling tests
./test_sql_ci_e2e.sh

# cURL profiling tests
./test_curl_ci_e2e.sh

# Error tracking tests
./test_errors_e2e.sh

# All tests
./test_errors_all.sh
```

### Test Requirements

- Docker and Docker Compose
- Access to `opa_network` Docker network
- Running OpenProfilingAgent and ClickHouse services
- MySQL 8.0+ (for SQL tests)

See [README_TESTS.md](README_TESTS.md) for detailed testing documentation.

## Development

### Building from Source

```bash
# Install build dependencies
sudo apt-get install php-dev autoconf gcc make libtool pkg-config liblz4-dev

# Build
./build.sh [PHP_VERSION]

# Or manually
phpize
./configure --enable-opa
make
```

### Docker Development

```bash
# Build debug container
docker-compose -f docker-compose.debug.yaml build

# Run with debugging
docker-compose -f docker-compose.debug.yaml up -d

# Run tests
docker exec -it opa-php-debug php /app/tests/test_file.php
```

### Debugging

The extension includes comprehensive debugging support:

- **Core Dump Analysis**: Automatic core dump generation on SIGSEGV
- **Debug Logging**: Enable with `OPA_DEBUG_LOG=1`
- **GDB Support**: Debug symbols and custom GDB commands

See [DEBUGGING.md](DEBUGGING.md) for detailed debugging instructions.

## Architecture

### Components

- **opa.c**: Main extension code, function hooking, and request lifecycle
- **span.c**: Span creation, management, and serialization
- **call_node.c**: Call stack tracking and function profiling
- **transport.c**: Communication with the agent (Unix socket/TCP)
- **serialize.c**: JSON serialization and LZ4 compression
- **error_tracking.c**: Error and log capture
- **opa_api.c**: PHP function implementations

### Data Flow

1. **Request Start**: Extension creates root span and initializes collector
2. **Function Calls**: `zend_execute_ex` hook tracks all function entries/exits
3. **SQL/HTTP**: Specialized hooks capture PDO, MySQLi, and cURL operations
4. **Request End**: Collector serializes data and sends to agent via transport
5. **Agent**: Receives data, processes, and stores in ClickHouse

## Performance Considerations

### Overhead

- **Sampling**: Use `OPA_SAMPLING_RATE` to reduce overhead (recommended: 0.1-0.5 for production)
- **Stack Depth**: Limit `OPA_STACK_DEPTH` to reduce memory usage
- **Internal Functions**: Disable `OPA_COLLECT_INTERNAL_FUNCTIONS` to reduce noise
- **Full Capture**: Use `OPA_FULL_CAPTURE_THRESHOLD_MS` to only fully profile slow requests

### Best Practices

- **Production**: 10-50% sampling rate
- **Staging**: 50-100% sampling rate
- **Development**: 100% sampling rate
- **Health Checks**: Disable profiling with `opa_disable()`
- **High-Frequency Endpoints**: Use conditional profiling

## Troubleshooting

### Extension Not Loading

```bash
# Check if extension is installed
php -m | grep opa

# Check PHP configuration
php -i | grep opa

# Check for errors
php -r "echo 'OK';"
```

### No Data in Agent

1. **Verify connection**: Check `OPA_SOCKET_PATH` matches agent configuration
2. **Check agent logs**: Ensure agent is running and receiving connections
3. **Enable debug logging**: Set `OPA_DEBUG_LOG=1` and check `/tmp/opa_debug.log`
4. **Verify sampling**: Ensure `OPA_SAMPLING_RATE > 0` and `OPA_ENABLED=1`

### Performance Issues

1. **Reduce sampling rate**: Lower `OPA_SAMPLING_RATE`
2. **Limit stack depth**: Reduce `OPA_STACK_DEPTH`
3. **Disable internal functions**: Set `OPA_COLLECT_INTERNAL_FUNCTIONS=0`
4. **Use full capture threshold**: Set `OPA_FULL_CAPTURE_THRESHOLD_MS` to only profile slow requests

## Related Documentation

- [RUNTIME_CONFIGURATION.md](RUNTIME_CONFIGURATION.md) - Detailed configuration guide
- [DEBUGGING.md](DEBUGGING.md) - Debugging and core dump analysis
- [README_TESTS.md](README_TESTS.md) - Testing documentation

## License

See [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please ensure:

1. Code follows existing style and patterns
2. Tests are added/updated for new features
3. Documentation is updated
4. All tests pass

## Support

For issues, questions, or contributions, please open an issue on the project repository.
