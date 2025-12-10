# Debugging Guide

This guide covers debugging the PHP extension, including core dump analysis and runtime debugging.

## Table of Contents

- [Core Dump Analysis](#core-dump-analysis)
- [Debug Build Setup](#debug-build-setup)
- [Runtime Debugging](#runtime-debugging)
- [Useful GDB Commands](#useful-gdb-commands)

## Core Dump Analysis

### Prerequisites

Core dumps are automatically enabled in the Docker containers. When a SIGSEGV occurs, PHP-FPM will:
- Generate a core dump file
- Log a warning message in PHP-FPM logs
- Write the core dump to `/var/log/core-dumps/` inside the container (mounted to `./logs/core-dumps/` on host)

### Finding Core Dumps

**On the Host Machine:**
```bash
# List core dumps on host
ls -lh ./logs/core-dumps/

# List PHP-FPM logs on host
ls -lh ./logs/php-fpm/
```

**Inside the Container:**
```bash
# List core dumps inside container
docker exec -it php-fpm-opa ls -lh /var/log/core-dumps/
```

### Analyzing Core Dumps with GDB

1. **Locate PHP-FPM Binary:**
   ```bash
   docker exec -it php-fpm-opa which php-fpm
   # Output: /usr/local/sbin/php-fpm
   ```

2. **Start GDB:**
   ```bash
   docker exec -it php-fpm-opa bash
   gdb /usr/local/sbin/php-fpm /var/log/core-dumps/core.php-fpm.<pid>.<timestamp>
   ```

3. **Get Backtrace:**
   ```
   (gdb) bt
   ```

4. **Get PHP Context:**
   ```gdb
   (gdb) print (char *)executor_globals.active_op_array->function_name
   (gdb) print (char *)executor_globals.active_op_array->filename
   ```

## Debug Build Setup

### Quick Start

1. **Build the debug container:**
   ```bash
   docker-compose -f docker-compose.debug.yaml build --no-cache
   ```

2. **Start the debug container:**
   ```bash
   docker-compose -f docker-compose.debug.yaml up -d
   ```

3. **Run a test:**
   ```bash
   docker exec -it opa-php-debug php /app/tests/curl_profiling_test.php
   ```

4. **Analyze the core dump:**
   ```bash
   ./debug_core.sh
   ```

### Debug Tools

The container automatically saves core dumps to `/var/log/core-dumps/` with the pattern:
`core.<executable>.<pid>.<timestamp>`

To analyze a core dump:
```bash
docker exec opa-php-debug /usr/local/bin/debug_core.sh
```

Or from the host:
```bash
./debug_core.sh
```

### Interactive Debugging

To debug interactively with gdb:
```bash
docker exec -it opa-php-debug /usr/local/bin/debug_interactive.sh /app/tests/curl_profiling_test.php
```

## Runtime Debugging

### Enable Debug Logging

Set the `OPA_DEBUG_LOG` environment variable:
```yaml
environment:
  - OPA_DEBUG_LOG=1
```

Debug logs are written to:
- `/tmp/opa_debug.log` (primary)
- `/app/logs/opa_debug.log` (fallback)

### Debug Log Format

Debug logs include:
- Timestamp
- File name and line number
- Function name
- Debug message

Example:
```
[2024-01-01 12:00:00] [opa.c:1234:function_name] Debug message here
```

## Useful GDB Commands

### Basic Commands

- `bt` - Show backtrace
- `frame N` - Switch to frame N
- `print var` - Print variable
- `info locals` - Show local variables
- `info args` - Show function arguments
- `continue` - Continue execution
- `step` - Step into function
- `next` - Step over function

### Memory Inspection

- `x/10x $rsp` - View 10 hex words from stack pointer
- `x/10i $pc` - View 10 instructions from program counter
- `info proc mappings` - Show memory mappings

### Breakpoints

```
(gdb) break opa_execute_ex
(gdb) break is_curl_call
(gdb) run /app/tests/curl_profiling_test.php
```

### Custom GDB Commands

The `.gdbinit` file defines custom commands:
- `execute_data_print <ptr>` - Pretty print a `zend_execute_data` structure
- `zval_print <ptr>` - Pretty print a `zval` structure

## Troubleshooting

### "No debugging symbols found"
This is normal for production builds. You'll still get function names and addresses, but not line numbers or local variables.

### "Cannot access memory"
The core dump may be corrupted or incomplete. Try a different core dump if available.

### Core dumps not being generated
- Verify `rlimit_core = unlimited` is in `/usr/local/etc/php-fpm.d/www.conf`
- Check that `/var/log/core-dumps` is writable
- Check system core dump settings: `ulimit -c` (should show `unlimited`)
- Verify core pattern: `cat /proc/sys/kernel/core_pattern`

## Production Considerations

- **Disk Space**: Core dumps can be very large (hundreds of MB to GB). Monitor disk usage.
- **Security**: Core dumps may contain sensitive data. Handle them carefully.
- **Performance**: Generating core dumps adds overhead. Consider disabling in production after debugging.
- **Retention**: Set up automatic cleanup of old core dumps to prevent disk fill.

## Additional Resources

- [GDB Documentation](https://www.gnu.org/software/gdb/documentation/)
- [PHP Internals Documentation](https://www.php.net/manual/en/internals2.php)
- [PHP-FPM Configuration](https://www.php.net/manual/en/install.fpm.configuration.php)
