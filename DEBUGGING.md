# Debugging Core Dumps

This guide explains how to analyze core dumps generated when PHP-FPM encounters a SIGSEGV (segmentation fault).

## Prerequisites

Core dumps are automatically enabled in the Docker containers. When a SIGSEGV occurs, PHP-FPM will:
- Generate a core dump file
- Log a warning message in PHP-FPM logs
- Write the core dump to `/var/log/core-dumps/` inside the container (mounted to `./logs/core-dumps/` on host)

## Finding Core Dumps

### On the Host Machine

All logs and core dumps are automatically mounted to the host machine in the `./logs/` directory:

```bash
# List core dumps on host
ls -lh ./logs/core-dumps/

# List PHP-FPM logs on host
ls -lh ./logs/php-fpm/        # For php-fpm service
ls -lh ./logs/symfony-test/   # For symfony-test service
```

### Inside the Container

```bash
# List core dumps inside container
docker exec -it php-fpm-opa ls -lh /var/log/core-dumps/

# For Symfony test app
docker exec -it opa-symfony-test ls -lh /var/log/core-dumps/
```

### PHP-FPM Logs

Check PHP-FPM logs for warnings:

**On the host machine:**
```bash
# View PHP-FPM error logs
tail -f ./logs/php-fpm/error.log

# View access logs
tail -f ./logs/php-fpm/access.log

# View slow query logs
tail -f ./logs/php-fpm/slow.log
```

**Inside the container:**
```bash
# View PHP-FPM error logs
docker exec -it php-fpm-opa tail -f /var/log/php-fpm/error.log

# Or check stdout/stderr
docker logs php-fpm-opa | grep SIGSEGV
```

You should see messages like:
```
WARNING: [pool www] child <pid> exited on signal 11 (SIGSEGV - core dumped) after X.XXXXXX seconds from start
```

## Analyzing Core Dumps with GDB

### Step 1: Locate PHP-FPM Binary

In Docker containers, PHP-FPM is typically located at:
- `/usr/local/sbin/php-fpm` (most common in PHP Docker images)
- `/usr/sbin/php-fpm` (some distributions)

To find it:
```bash
docker exec -it php-fpm-opa which php-fpm
# or
docker exec -it php-fpm-opa find /usr -name php-fpm -type f 2>/dev/null
```

### Step 2: Start GDB

**Option 1: Inside the container (recommended)**
```bash
# Enter the container
docker exec -it php-fpm-opa bash

# Start GDB with the PHP-FPM binary and core dump
gdb /usr/local/sbin/php-fpm /var/log/core-dumps/core.php-fpm.<pid>.<timestamp>
```

**Option 2: On the host machine**
```bash
# Copy the core dump and PHP-FPM binary to host (if needed)
# Note: For accurate analysis, it's better to analyze inside the container

# Start GDB on host
gdb /path/to/php-fpm ./logs/core-dumps/core.php-fpm.<pid>.<timestamp>
```

### Step 3: Get Backtrace

Once in GDB, get the backtrace:
```
(gdb) bt
```

This will show the call stack at the moment of the crash. Example output:
```
#0  0x00007f8a8b6d7c37 in mmc_value_handler_multi () from /usr/lib64/php/modules/memcache.so
#1  0x00007f8a8b6db9ad in mmc_unpack_value () from /usr/lib64/php/modules/memcache.so
#2  0x00007f8a8b6e0637 in ?? () from /usr/lib64/php/modules/memcache.so
#3  0x00007f8a8b6dd55b in mmc_pool_select () from /usr/lib64/php/modules/memcache.so
...
```

### Step 4: Get PHP Context Information

From the GDB shell, you can extract PHP context information:

#### Current PHP Function
```gdb
(gdb) print (char *)executor_globals.active_op_array->function_name
```

#### Current PHP File
```gdb
(gdb) print (char *)executor_globals.active_op_array->filename
```

#### Current PHP Method (if in a class)
```gdb
(gdb) print (char *)executor_globals.active_op_array->class_name
```

**Note**: Not all information may be available depending on the crash context. Some symbols may show as `(no debugging symbols found)` if the PHP binary or extensions weren't compiled with debug symbols.

### Step 5: Exit GDB

```
(gdb) quit
```

## Useful GDB Commands

### View Local Variables
```
(gdb) info locals
```

### View Arguments
```
(gdb) info args
```

### View Registers
```
(gdb) info registers
```

### View Memory
```
(gdb) x/10x $rsp    # View 10 hex words from stack pointer
(gdb) x/10i $pc     # View 10 instructions from program counter
```

### Set Breakpoints (if debugging live)
```
(gdb) break function_name
(gdb) continue
```

## Logs and Core Dumps Location

All logs and core dumps are automatically available on the host machine:

- **PHP-FPM logs**: `./logs/php-fpm/` (error.log, access.log, slow.log)
- **Symfony logs**: `./logs/symfony-test/` (error.log, access.log, slow.log)
- **Core dumps**: `./logs/core-dumps/` (core.php-fpm.<pid>.<timestamp>)

These directories are mounted from the containers, so you can access them directly without copying files.

**Note**: For accurate GDB analysis, it's recommended to analyze core dumps inside the container where all libraries and extensions match exactly.

## Example: Complete Debugging Session

```bash
# 1. Find the core dump on host
ls -lh ./logs/core-dumps/

# 2. Enter the container
docker exec -it php-fpm-opa bash

# 3. Find PHP-FPM binary location
which php-fpm
# Output: /usr/local/sbin/php-fpm

# 4. List core dumps in container
ls -lh /var/log/core-dumps/

# 5. Start GDB (replace <pid> and <timestamp> with actual values)
gdb /usr/local/sbin/php-fpm /var/log/core-dumps/core.php-fpm.<pid>.<timestamp>

# 6. In GDB, get backtrace
(gdb) bt

# 7. Get PHP context
(gdb) print (char *)executor_globals.active_op_array->function_name
(gdb) print (char *)executor_globals.active_op_array->filename

# 8. Exit
(gdb) quit
```

## Troubleshooting

### "No debugging symbols found"
This is normal for production builds. You'll still get function names and addresses, but not line numbers or local variables.

### "Cannot access memory"
The core dump may be corrupted or incomplete. Try a different core dump if available.

### "No such file or directory"
- Verify the core dump path is correct
- Check that the core dump file exists: `ls -lh /tmp/coredump*`
- Ensure you're using the correct PHP-FPM binary path

### Core dumps not being generated
- Verify `rlimit_core = unlimited` is in `/usr/local/etc/php-fpm.d/www.conf`
- Check that `/var/log/core-dumps` is writable: `ls -ld /var/log/core-dumps`
- Check system core dump settings: `ulimit -c` (should show `unlimited`)
- Verify core pattern is configured: `cat /proc/sys/kernel/core_pattern` (should show `/var/log/core-dumps/core.%e.%p.%t`)
- Check container logs for core dump configuration messages

## Production Considerations

- **Disk Space**: Core dumps can be very large (hundreds of MB to GB). Monitor disk usage.
- **Security**: Core dumps may contain sensitive data. Handle them carefully.
- **Performance**: Generating core dumps adds overhead. Consider disabling in production after debugging.
- **Retention**: Set up automatic cleanup of old core dumps to prevent disk fill.

## Additional Resources

- [GDB Documentation](https://www.gnu.org/software/gdb/documentation/)
- [PHP Internals Documentation](https://www.php.net/manual/en/internals2.php)
- [PHP-FPM Configuration](https://www.php.net/manual/en/install.fpm.configuration.php)

