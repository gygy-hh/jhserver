#!/usr/bin/env bash
# 在 WSL/Ubuntu 上静态编译，生成可在阿里云 Linux 上运行的 jh_server
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-linux"
mkdir -p "$OUT"

g++ -std=c++17 -O1 -g0 \
  -static -static-libgcc -static-libstdc++ -pthread \
  -DCPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=67108864 \
  -I"$ROOT/include" -I"$ROOT/third_party" -I/usr/include/mariadb \
  "$ROOT"/src/main.cpp \
  "$ROOT"/src/config.cpp \
  "$ROOT"/src/crypto.cpp \
  "$ROOT"/src/handlers.cpp \
  "$ROOT"/src/admin.cpp \
  "$ROOT"/src/auth.cpp \
  "$ROOT"/src/db.cpp \
  "$ROOT"/src/storage.cpp \
  "$ROOT"/src/mail.cpp \
  "$ROOT"/src/save_crypto.cpp \
  "$ROOT"/src/save_blob.cpp \
  "$ROOT"/src/update.cpp \
  "$ROOT"/src/static_nss.cpp \
  -L/usr/lib/x86_64-linux-gnu \
  -Wl,--wrap=getaddrinfo \
  -Wl,--wrap=freeaddrinfo \
  -Wl,--wrap=gai_strerror \
  -Wl,--wrap=getservbyname \
  -Wl,--wrap=gethostbyname \
  -Wl,--wrap=getpwuid \
  -Wl,--wrap=dlopen \
  -Wl,--wrap=dlsym \
  -Wl,--wrap=dlclose \
  -Wl,--wrap=dlerror \
  -lmariadb -lz -lssl -lcrypto -ldl -lm \
  -o "$OUT/jh_server"

ls -lh "$OUT/jh_server"
file "$OUT/jh_server"
