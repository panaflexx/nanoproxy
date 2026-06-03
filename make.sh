echo "Super basic make, for openssl static builds"
gcc -o npserver -g3 -DHAVE_OPENSSL -I openssl-3.5.2/include/ -I include server.c openssl-3.5.2/libcrypto.dylib openssl-3.5.2/libssl.dylib
