#!/bin/bash -ex

unset OPENSSL_CFLAGS
unset OPENSSL_LIBS
unset LDFLAGS
unset CPPFLAGS

make distclean || true
autoreconf -iv && OPENSSL_CFLAGS="${CFLAGS} -g -I${HOME}/openssl-1.1/include" OPENSSL_LIBS="-L${HOME}/openssl-1.1/lib -lssl -lcrypto" LDFLAGS="-g" CPPFLAGS="" ./configure --prefix=${HOME}/openssl-1.1 --with-pkcs11-module=/opt/local/lib/p11-kit-proxy.dylib --with-enginesdir=${HOME}/openssl-1.1/lib/engines-1.1/ && make clean && make -j 4  all && make check && make install

