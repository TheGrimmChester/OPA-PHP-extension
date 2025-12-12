#!/bin/bash
set -e

# Configure core dump pattern to write to /var/log/core-dumps
# This must be done as root before dropping privileges
if [ -w /proc/sys/kernel/core_pattern ]; then
    echo "/var/log/core-dumps/core.%e.%p.%t" > /proc/sys/kernel/core_pattern
    echo "Core dump pattern configured: /var/log/core-dumps/core.%e.%p.%t"
fi

# Ensure log directories exist and have correct permissions
mkdir -p /var/log/php-fpm
mkdir -p /var/log/core-dumps
chown -R www-data:www-data /var/log/php-fpm /var/log/core-dumps || true
chmod 755 /var/log/php-fpm /var/log/core-dumps || true

# Copy extension .so file to shared directory with datetime
if [ -d "/build-output" ]; then
    # Find the extension directory
    EXT_DIR=$(php-config --extension-dir)
    if [ -f "$EXT_DIR/opa.so" ]; then
        DATETIME=$(date +%Y%m%d_%H%M%S)
        cp "$EXT_DIR/opa.so" "/build-output/opa_${DATETIME}.so"
        echo "Extension copied to /build-output/opa_${DATETIME}.so"
        # Also create a symlink to the latest version
        ln -sf "opa_${DATETIME}.so" "/build-output/opa_latest.so" || true
    fi
fi

# Configure OpenProfilingAgent from environment variables
# Environment variables override any existing INI configuration
OPA_INI_FILE="/usr/local/etc/php/conf.d/opa.ini"

# Function to update INI setting if environment variable is set
update_ini_setting() {
    local env_var=$1
    local ini_key=$2
    local value=$(printenv "$env_var")
    
    if [ -n "$value" ]; then
        # Remove existing setting if present (env vars always override)
        sed -i "/^${ini_key}=/d" "$OPA_INI_FILE" 2>/dev/null || true
        # Add new setting from environment variable
        echo "${ini_key}=${value}" >> "$OPA_INI_FILE"
    fi
}

# Ensure INI file exists, preserving extension line but removing all opa.* settings
# This ensures environment variables always override INI defaults
if [ -f "$OPA_INI_FILE" ]; then
    # Remove all existing opa.* settings (keep extension line and other settings)
    sed -i "/^opa\./d" "$OPA_INI_FILE" 2>/dev/null || true
    # Ensure extension line exists
    if ! grep -q "^extension=opa.so" "$OPA_INI_FILE" 2>/dev/null; then
        echo "extension=opa.so" >> "$OPA_INI_FILE"
    fi
else
    # Create new INI file with just extension line
    echo "extension=opa.so" > "$OPA_INI_FILE"
fi

# Update INI settings from environment variables (these override any defaults)
update_ini_setting "OPA_ENABLED" "opa.enabled"
update_ini_setting "OPA_SAMPLING_RATE" "opa.sampling_rate"
update_ini_setting "OPA_SOCKET_PATH" "opa.socket_path"
update_ini_setting "OPA_FULL_CAPTURE_THRESHOLD_MS" "opa.full_capture_threshold_ms"
update_ini_setting "OPA_STACK_DEPTH" "opa.stack_depth"
update_ini_setting "OPA_BUFFER_SIZE" "opa.buffer_size"
update_ini_setting "OPA_COLLECT_INTERNAL_FUNCTIONS" "opa.collect_internal_functions"
update_ini_setting "OPA_DEBUG_LOG" "opa.debug_log"
update_ini_setting "OPA_ORGANIZATION_ID" "opa.organization_id"
update_ini_setting "OPA_PROJECT_ID" "opa.project_id"
update_ini_setting "OPA_SERVICE" "opa.service"
update_ini_setting "OPA_LANGUAGE" "opa.language"
update_ini_setting "OPA_LANGUAGE_VERSION" "opa.language_version"
update_ini_setting "OPA_FRAMEWORK" "opa.framework"
update_ini_setting "OPA_FRAMEWORK_VERSION" "opa.framework_version"
update_ini_setting "OPA_EXPAND_SPANS" "opa.expand_spans"

# Execute the original docker-php-entrypoint (or the command passed)
if [ "$1" = "php-fpm" ]; then
    exec docker-php-entrypoint php-fpm
else
    exec docker-php-entrypoint "$@"
fi

