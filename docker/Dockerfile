ARG PHP_VERSION=8.4
FROM php:${PHP_VERSION}-fpm

RUN apt-get update && apt-get install -y \
    git \
    autoconf \
    gcc \
    make \
    libtool \
    pkg-config \
    liblz4-dev \
    libpthread-stubs0-dev \
    gdb \
    default-mysql-client \
    libmariadb-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Composer BEFORE installing opa extension to avoid segfaults
RUN curl -sS https://getcomposer.org/installer | php -- --install-dir=/usr/local/bin --filename=composer && \
    chmod +x /usr/local/bin/composer

# Install sockets extension for PHP BEFORE opa
RUN docker-php-ext-install sockets

# Install MySQLi and PDO MySQL extensions
RUN docker-php-ext-install mysqli pdo pdo_mysql

# Install APCu extension
RUN pecl install apcu && \
    docker-php-ext-enable apcu

# Install Redis extension
RUN pecl install redis || echo "Warning: Redis extension installation failed (network issue), continuing..." && \
    (docker-php-ext-enable redis || echo "Warning: Redis extension not available")

WORKDIR /usr/src/opa

COPY config.m4 configure.ac /usr/src/opa/
COPY src/ /usr/src/opa/src/

RUN phpize && \
    ./configure --enable-opa && \
    make && \
    EXT_DIR=$(php-config --extension-dir) && \
    cp modules/opa.so $EXT_DIR/opa.so

RUN echo "extension=opa.so" > /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.enabled=1" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.socket_path=/var/run/opa.sock" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.sampling_rate=1.0" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.debug_log=1" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.organization_id=test-org" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.project_id=default-project" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.service=php-app" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.language=php" >> /usr/local/etc/php/conf.d/opa.ini && \
    echo "opa.language_version=8.4" >> /usr/local/etc/php/conf.d/opa.ini

# Configure APCu
RUN echo "apc.enabled=1" >> /usr/local/etc/php/conf.d/apcu.ini && \
    echo "apc.shm_size=64M" >> /usr/local/etc/php/conf.d/apcu.ini && \
    echo "apc.ttl=3600" >> /usr/local/etc/php/conf.d/apcu.ini

# Configure PHP-FPM pool to enable core dumps on SIGSEGV and set log paths
RUN echo "rlimit_core = unlimited" >> /usr/local/etc/php-fpm.d/www.conf && \
    echo "php_admin_value[error_log] = /var/log/php-fpm/error.log" >> /usr/local/etc/php-fpm.d/www.conf && \
    echo "php_admin_flag[log_errors] = on" >> /usr/local/etc/php-fpm.d/www.conf && \
    echo "catch_workers_output = yes" >> /usr/local/etc/php-fpm.d/www.conf && \
    echo "access.log = /var/log/php-fpm/access.log" >> /usr/local/etc/php-fpm.d/www.conf && \
    echo "slowlog = /var/log/php-fpm/slow.log" >> /usr/local/etc/php-fpm.d/www.conf

# Copy entrypoint script for core dump configuration
COPY docker/entrypoint.sh /usr/local/bin/docker-entrypoint-custom.sh
RUN chmod +x /usr/local/bin/docker-entrypoint-custom.sh

# Create log directories and set permissions
RUN mkdir -p /var/run && chown www-data:www-data /var/run && \
    mkdir -p /app/logs && chown www-data:www-data /app/logs && chmod 755 /app/logs && \
    mkdir -p /var/log/php-fpm && chown www-data:www-data /var/log/php-fpm && chmod 755 /var/log/php-fpm && \
    mkdir -p /var/log/core-dumps && chown www-data:www-data /var/log/core-dumps && chmod 755 /var/log/core-dumps

WORKDIR /var/www/html
