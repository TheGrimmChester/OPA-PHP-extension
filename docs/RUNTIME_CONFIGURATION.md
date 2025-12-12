# Runtime Configuration Guide

OpenProfilingAgent supports on-the-fly configuration through environment variables and PHP functions, allowing you to enable, disable, and configure profiling without rebuilding containers or restarting services.

## Table of Contents

- [Environment Variable Configuration](#environment-variable-configuration)
- [Runtime PHP Functions](#runtime-php-functions)
- [Docker Compose Examples](#docker-compose-examples)
- [Use Cases](#use-cases)
- [Best Practices](#best-practices)

## Environment Variable Configuration

Environment variables provide the easiest way to configure OpenProfilingAgent at runtime. They override any INI settings configured during the Docker build.

### Available Environment Variables

All environment variables follow the pattern `OPA_*` and map directly to INI settings:

| Environment Variable | INI Setting | Default | Description |
|---------------------|-------------|---------|-------------|
| `OPA_ENABLED` | `opa.enabled` | `1` | Enable/disable profiling (0 or 1) |
| `OPA_SAMPLING_RATE` | `opa.sampling_rate` | `1.0` | Sampling rate (0.0 to 1.0) |
| `OPA_SOCKET_PATH` | `opa.socket_path` | `/var/run/opa.sock` | Unix socket path or TCP address (format: `host:port`). Auto-detected: paths starting with `/` are Unix sockets, otherwise TCP/IP |
| `OPA_FULL_CAPTURE_THRESHOLD_MS` | `opa.full_capture_threshold_ms` | `100` | Threshold for full capture (ms) |
| `OPA_STACK_DEPTH` | `opa.stack_depth` | `20` | Maximum stack depth |
| `OPA_BUFFER_SIZE` | `opa.buffer_size` | `65536` | Buffer size in bytes |
| `OPA_COLLECT_INTERNAL_FUNCTIONS` | `opa.collect_internal_functions` | `1` | Collect internal PHP functions (0 or 1) |
| `OPA_DEBUG_LOG` | `opa.debug_log` | `0` | Enable debug logging (0 or 1) |
| `OPA_ORGANIZATION_ID` | `opa.organization_id` | `default-org` | Organization identifier |
| `OPA_PROJECT_ID` | `opa.project_id` | `default-project` | Project identifier |
| `OPA_SERVICE` | `opa.service` | `php-fpm` | Service name |
| `OPA_LANGUAGE` | `opa.language` | `php` | Language identifier |
| `OPA_LANGUAGE_VERSION` | `opa.language_version` | (auto) | Language version |
| `OPA_FRAMEWORK` | `opa.framework` | (empty) | Framework name (e.g., `symfony`) |
| `OPA_FRAMEWORK_VERSION` | `opa.framework_version` | (empty) | Framework version |

### Agent Environment Variables

The agent also supports environment variables for transport configuration:

| Environment Variable | Flag | Default | Description |
|---------------------|------|---------|-------------|
| `SOCKET_PATH` | `-socket` | `/var/run/opa.sock` | Unix socket path |
| `TRANSPORT_TCP` | `-tcp` | (empty) | TCP address to listen on (e.g., `:9090` or `0.0.0.0:9090`). If empty, TCP listener is disabled |

**Note**: The agent can listen on both Unix socket and TCP/IP simultaneously. At least one transport must be configured.

### How It Works

1. **Container Startup**: The entrypoint script reads environment variables
2. **INI File Update**: All existing `opa.*` settings are removed from the INI file
3. **Environment Override**: Settings from environment variables are written to the INI file
4. **PHP Loads Configuration**: PHP reads the updated INI file when the extension loads

**Important**: Environment variables always override INI settings, even if they were set during the Docker build.

### Docker Compose Example

```yaml
services:
  php-fpm:
    build: ./php-extension
    environment:
      # Enable profiling with 10% sampling
      - OPA_ENABLED=1
      - OPA_SAMPLING_RATE=0.1
      # Transport: Unix socket or TCP/IP
      - OPA_SOCKET_PATH=/var/run/opa.sock  # Unix socket
      # Or use TCP/IP:
      # - OPA_SOCKET_PATH=agent:9090  # TCP format: host:port
      
      # Service identification
      - OPA_ORGANIZATION_ID=my-org
      - OPA_PROJECT_ID=my-project
      - OPA_SERVICE=api-service
      
      # Framework information
      - OPA_FRAMEWORK=symfony
      - OPA_FRAMEWORK_VERSION=7.0
      
      # Debugging (enable only when needed)
      - OPA_DEBUG_LOG=0
```

### Docker Run Example

```bash
docker run -d \
  -e OPA_ENABLED=1 \
  -e OPA_SAMPLING_RATE=0.5 \
  -e OPA_SERVICE=my-service \
  -e OPA_ORGANIZATION_ID=prod-org \
  -e OPA_PROJECT_ID=my-project \
  my-php-app
```

### Kubernetes Example

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: php-app
spec:
  template:
    spec:
      containers:
      - name: php-fpm
        image: my-php-app
        env:
        - name: OPA_ENABLED
          value: "1"
        - name: OPA_SAMPLING_RATE
          value: "0.1"
        - name: OPA_SERVICE
          value: "api-service"
        - name: OPA_ORGANIZATION_ID
          valueFrom:
            secretKeyRef:
              name: opa-config
              key: organization-id
```

### Disabling Profiling

To disable profiling completely:

```yaml
environment:
  - OPA_ENABLED=0
```

When disabled, the extension is loaded but does not collect or send any trace data.

## Runtime PHP Functions

For programmatic control within your PHP application, use the runtime functions to enable/disable profiling during request execution.

### opa_enable()

Enables profiling for the current request. Can be called even if profiling was disabled at request start.

```php
<?php
// Enable profiling at runtime
opa_enable();

// Now all function calls will be profiled
some_expensive_operation();
```

**Use Cases**:
- Enable profiling conditionally based on request parameters
- Enable profiling for specific user segments
- Enable profiling for debugging specific issues

**Example**: Enable profiling only for admin users

```php
<?php
if (is_admin_user()) {
    opa_enable();
}
// Profiling is now active for this request
handle_request();
```

### opa_disable()

Disables profiling for the current request. Can be called even if profiling was enabled at request start.

```php
<?php
// Disable profiling at runtime
opa_disable();

// Function calls after this point will not be profiled
some_operation();
```

**Use Cases**:
- Disable profiling for health checks or monitoring endpoints
- Disable profiling for high-frequency, low-value requests
- Reduce overhead for specific code paths

**Example**: Disable profiling for health check endpoints

```php
<?php
if ($_SERVER['REQUEST_URI'] === '/health') {
    opa_disable();
    echo json_encode(['status' => 'ok']);
    exit;
}
// Normal request processing continues with profiling
```

### opa_is_enabled()

Returns whether profiling is currently enabled for the current request.

```php
<?php
if (opa_is_enabled()) {
    echo "Profiling is active\n";
} else {
    echo "Profiling is disabled\n";
}
```

**Use Cases**:
- Conditional logic based on profiling state
- Debugging and troubleshooting
- Logging profiling status

**Example**: Conditional behavior based on profiling state

```php
<?php
if (opa_is_enabled()) {
    // Add additional context when profiling
    opa_add_tag($spanId, 'debug_mode', 'true');
    opa_dump($debugData);
} else {
    // Use lightweight logging when not profiling
    error_log("Operation completed");
}
```

### Complete Example: Conditional Profiling

```php
<?php
// Enable profiling only for requests with specific header
if (isset($_SERVER['HTTP_X_ENABLE_PROFILING']) && 
    $_SERVER['HTTP_X_ENABLE_PROFILING'] === 'true') {
    opa_enable();
    echo "Profiling enabled for this request\n";
}

// Check if profiling is active
if (opa_is_enabled()) {
    // Create a manual span for important operations
    $spanId = opa_start_span('important_operation', [
        'user_id' => get_current_user_id(),
        'request_id' => $_SERVER['REQUEST_ID'] ?? 'unknown'
    ]);
    
    try {
        perform_important_operation();
        opa_add_tag($spanId, 'status', 'success');
    } catch (Exception $e) {
        opa_add_tag($spanId, 'status', 'error');
        opa_add_tag($spanId, 'error', $e->getMessage());
        throw $e;
    } finally {
        opa_end_span($spanId);
    }
} else {
    // Fallback when profiling is disabled
    perform_important_operation();
}

// Disable profiling for cleanup operations
opa_disable();
perform_cleanup();
```

## Docker Compose Examples

### Recommended Configuration

```yaml
services:
  php-fpm:
    build: ./php-extension
    environment:
      # Recommended: 10% sampling to reduce overhead
      - OPA_ENABLED=1
      - OPA_SAMPLING_RATE=0.1
      - OPA_SOCKET_PATH=/var/run/opa.sock
      - OPA_FULL_CAPTURE_THRESHOLD_MS=200
      
      # Service identifiers
      - OPA_ORGANIZATION_ID=my-org
      - OPA_PROJECT_ID=main-app
      - OPA_SERVICE=api-service
      
      # Disable debug logging
      - OPA_DEBUG_LOG=0
```

### Development Configuration

```yaml
services:
  php-fpm:
    build: ./php-extension
    environment:
      # Development: 100% sampling for complete visibility
      - OPA_ENABLED=1
      - OPA_SAMPLING_RATE=1.0
      - OPA_SOCKET_PATH=/var/run/opa.sock
      
      # Development identifiers
      - OPA_ORGANIZATION_ID=development
      - OPA_PROJECT_ID=dev-app
      - OPA_SERVICE=api-dev
      
      # Enable debug logging in development
      - OPA_DEBUG_LOG=1
```

### Staging Configuration

```yaml
services:
  php-fpm:
    build: ./php-extension
    environment:
      # Staging: 50% sampling
      - OPA_ENABLED=1
      - OPA_SAMPLING_RATE=0.5
      - OPA_SOCKET_PATH=/var/run/opa.sock
      
      # Staging identifiers
      - OPA_ORGANIZATION_ID=staging
      - OPA_PROJECT_ID=staging-app
      - OPA_SERVICE=api-staging
      
      - OPA_DEBUG_LOG=0
```

### Disabled Configuration

```yaml
services:
  php-fpm:
    build: ./php-extension
    environment:
      # Completely disable profiling
      - OPA_ENABLED=0
```

## Use Cases

### 1. A/B Testing Profiling Impact

Enable profiling for a subset of requests to measure performance impact:

```php
<?php
// Enable profiling for 10% of requests
if (rand(1, 100) <= 10) {
    opa_enable();
}
```

### 2. Debug Mode

Enable profiling when a debug flag is set:

```php
<?php
if (getenv('DEBUG_MODE') === 'true') {
    opa_enable();
    // Profiling will capture all operations
}
```

### 3. User-Based Profiling

Enable profiling for specific users (e.g., beta testers):

```php
<?php
$user = get_current_user();
if (in_array($user->id, $beta_tester_ids)) {
    opa_enable();
}
```

### 4. Error-Based Profiling

Enable profiling when errors occur:

```php
<?php
try {
    process_request();
} catch (Exception $e) {
    // Enable profiling to capture error context
    opa_enable();
    handle_error($e);
}
```

### 5. Performance Monitoring

Enable profiling for slow requests:

```php
<?php
$start_time = microtime(true);

// ... process request ...

$duration = microtime(true) - $start_time;
if ($duration > 1.0) { // Slower than 1 second
    opa_enable();
    // Re-process with profiling to capture details
}
```

### 6. Feature Flag Integration

```php
<?php
if (feature_flag_enabled('enable_profiling')) {
    opa_enable();
}
```

## Best Practices

### 1. Use Environment Variables for Static Configuration

For configuration that doesn't change during runtime, use environment variables:

```yaml
# Good: Set once at container startup
environment:
  - OPA_SAMPLING_RATE=0.1
  - OPA_SERVICE=api-service
```

### 2. Use PHP Functions for Dynamic Control

For conditional profiling based on request characteristics, use PHP functions:

```php
// Good: Dynamic control based on request
if (should_profile_request()) {
    opa_enable();
}
```

### 3. Sampling Rate Guidelines

- **High-traffic environments**: 0.1 (10%) to 0.5 (50%) depending on traffic
- **Staging**: 0.5 (50%) to 1.0 (100%)
- **Development**: 1.0 (100%) for complete visibility

### 4. Disable for Health Checks

Always disable profiling for health check endpoints:

```php
<?php
if ($_SERVER['REQUEST_URI'] === '/health' || 
    $_SERVER['REQUEST_URI'] === '/ready') {
    opa_disable();
    // Return health status
}
```

### 5. Monitor Overhead

Use `opa_is_enabled()` to track when profiling is active:

```php
<?php
$profiling_active = opa_is_enabled();
// Log or track this metric
```

### 6. Combine Environment Variables and PHP Functions

Use environment variables for base configuration and PHP functions for fine-grained control:

```yaml
# Base configuration via environment
environment:
  - OPA_ENABLED=1
  - OPA_SAMPLING_RATE=0.1
```

```php
// Fine-grained control via PHP
if (is_important_request()) {
    opa_enable(); // Override to 100% for important requests
}
```

### 7. Testing Configuration

Test your configuration changes:

```php
<?php
// Verify configuration
assert(opa_is_enabled() === true, "Profiling should be enabled");

// Test enable/disable
opa_disable();
assert(opa_is_enabled() === false, "Profiling should be disabled");

opa_enable();
assert(opa_is_enabled() === true, "Profiling should be enabled again");
```

## Troubleshooting

### Environment Variables Not Applied

**Problem**: Environment variables are set but not taking effect.

**Solution**: 
1. Verify the entrypoint script is running: Check container logs
2. Verify INI file is updated: `docker exec container cat /usr/local/etc/php/conf.d/opa.ini`
3. Check PHP configuration: `docker exec container php -i | grep opa`

### PHP Functions Not Working

**Problem**: `opa_enable()`, `opa_disable()`, or `opa_is_enabled()` are not available.

**Solution**:
1. Verify extension is loaded: `php -m | grep opa`
2. Check PHP version compatibility (requires PHP 8.0+)
3. Rebuild the extension if needed

### Profiling Still Active After opa_disable()

**Problem**: Called `opa_disable()` but profiling continues.

**Solution**:
- `opa_disable()` only affects the current request
- Function calls that started before `opa_disable()` may still complete
- Check that `opa_is_enabled()` returns `false` after calling `opa_disable()`

## Related Documentation

- [Installation Guide](INSTALLATION.md) - How to install OpenProfilingAgent
- [User Guide](GUIDE.md) - Complete usage guide and API reference
- [Integration Guide](INTEGRATION.md) - Integration with frameworks and applications

