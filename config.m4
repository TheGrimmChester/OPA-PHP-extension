PHP_ARG_ENABLE(opa, whether to enable opa support,
[ --enable-opa   Enable opa support])

if test "$PHP_OPA" != "no"; then
  # Check for LZ4 library
  AC_CHECK_HEADER([lz4.h], [
    AC_CHECK_LIB([lz4], [LZ4_compress], [
      PHP_ADD_LIBRARY_WITH_PATH(lz4, , OPA_SHARED_LIBADD)
      AC_DEFINE(HAVE_LZ4, 1, [Have LZ4 library])
    ])
  ])
  
  PHP_NEW_EXTENSION(opa, opa.c span.c call_node.c transport.c serialize.c opa_api.c error_tracking.c, $ext_shared, , -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -DCOMPILE_DL_OPA=1)
  PHP_SUBST(OPA_SHARED_LIBADD)
fi
