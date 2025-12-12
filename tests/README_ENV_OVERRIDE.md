# Environment Variable Override Testing

## Status

The PHP extension has been compiled with environment variable override support. The code checks for `OPA_*` environment variables in the `RINIT` function and attempts to override INI settings using `zend_alter_ini_entry()`.

## Current Implementation

The extension now:
1. Checks all `OPA_*` environment variables in `RINIT` (before request processing)
2. Attempts to override INI settings using `zend_alter_ini_entry()`
3. Checks `OPA_ENABLE` specifically for on-the-fly profiling control

## Testing

### Build Status
✅ Extension compiles successfully

### Test Scripts
- `test_env_override.php` - PHP test script that validates overrides
- `test_env_override.sh` - Shell script to run multiple test scenarios
- `test_env_override_docker.sh` - Docker-based test runner

### Running Tests

```bash
# Using Docker (recommended)
cd /home/xorix/myapm
bash php-extension/tests/test_env_override_docker.sh

# Or manually
docker run --rm \
  -v "$(pwd)/php-extension/tests:/tests:ro" \
  -w /tests \
  opa-php-test \
  bash -c "OPA_ENABLED=0 OPA_SAMPLING_RATE=0.5 php test_env_override.php"
```

## Known Issues

The `zend_alter_ini_entry()` function may not work as expected in `RINIT` for all INI settings. The environment variables are detected, but the INI values may not be updated immediately.

### Workaround

For Docker deployments, the entrypoint script (`docker/entrypoint.sh`) already handles environment variable overrides by writing to the INI file at container startup. This is the recommended approach for production use.

For runtime overrides (on-the-fly profiling), use:
- `OPA_ENABLE=1` environment variable (works for CLI commands)
- `opa_enable()` PHP function (works in code)

## Next Steps

If runtime INI overrides are critical, consider:
1. Directly updating globals from environment variables (bypassing INI system)
2. Using a different PHP API for runtime INI updates
3. Relying on the entrypoint script for container-based deployments

## Files Modified

- `php-extension/src/opa.c` - Added `update_ini_from_env()` function and calls in `RINIT`
- `php-extension/tests/test_env_override.php` - Test script
- `php-extension/tests/test_env_override.sh` - Test runner
- `php-extension/tests/test_env_override_docker.sh` - Docker test runner

