# Debugging Core Dumps

This directory contains debugging tools and configurations for debugging segmentation faults in the PHP extension.

## Quick Start

1. **Build the debug container:**
   ```bash
   docker-compose -f docker-compose.debug.yaml build
   ```

2. **Start the debug container:**
   ```bash
   docker-compose -f docker-compose.debug.yaml up -d
   ```

3. **Run a test that causes a segfault:**
   ```bash
   docker exec -it opa-php-debug php /app/tests/curl_profiling_test.php
   ```

4. **Analyze the core dump:**
   ```bash
   ./debug_core.sh
   ```

## Debugging Tools

### Core Dump Analysis

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

### Useful GDB Commands

Once in gdb, you can use:

- `bt` - Show backtrace
- `frame N` - Switch to frame N
- `print var` - Print variable
- `info locals` - Show local variables
- `info args` - Show function arguments
- `execute_data_print <ptr>` - Print execute_data structure (custom command)
- `zval_print <ptr>` - Print zval structure (custom command)
- `break opa_execute_ex` - Set breakpoint in opa_execute_ex
- `break is_curl_call` - Set breakpoint in is_curl_call
- `continue` - Continue execution
- `step` - Step into function
- `next` - Step over function

### Custom GDB Commands

The `.gdbinit` file defines custom commands:

- `execute_data_print <ptr>` - Pretty print a `zend_execute_data` structure
- `zval_print <ptr>` - Pretty print a `zval` structure

### Debugging Tips

1. **Set breakpoints before running:**
   ```
   (gdb) break opa_execute_ex
   (gdb) break is_curl_call
   (gdb) run /app/tests/curl_profiling_test.php
   ```

2. **Inspect execute_data when it crashes:**
   ```
   (gdb) frame 0
   (gdb) execute_data_print execute_data
   (gdb) print execute_data->func
   (gdb) print execute_data->func->type
   ```

3. **Check if function_name is NULL:**
   ```
   (gdb) print execute_data->func->common.function_name
   ```

4. **Inspect arguments:**
   ```
   (gdb) print ZEND_CALL_NUM_ARGS(execute_data)
   (gdb) print ZEND_CALL_ARG(execute_data, 1)
   ```

5. **Check memory validity:**
   ```
   (gdb) info proc mappings
   (gdb) x/10x execute_data
   ```

## Environment Variables

The debug container sets:
- `PHP_OPCACHE_ENABLE=0` - Disables opcache for easier debugging
- `OPA_DEBUG_LOG=1` - Enables debug logging

## Core Dump Location

Core dumps are saved to `/var/log/core-dumps/` in the container, which is mounted as a volume.

To access from host:
```bash
docker exec opa-php-debug ls -lah /var/log/core-dumps/
```

## Rebuilding with Debug Symbols

The debug Dockerfile builds with `-g -O0` flags to include debug symbols and disable optimizations.

To rebuild:
```bash
docker-compose -f docker-compose.debug.yaml build --no-cache
```

