// Linux static builds: intercept glibc NSS/DNS so MariaDB/OpenSSL do not SEGV
// on older distros (Alibaba Cloud Linux) that lack matching nss modules.

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>

extern "C" {

int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                       struct addrinfo** res) {
  if (!res) {
    return EAI_FAIL;
  }
  *res = nullptr;

  int port = 0;
  if (service && service[0]) {
    if (service[0] >= '0' && service[0] <= '9') {
      port = std::atoi(service);
    } else if (std::strcmp(service, "mysql") == 0) {
      port = 3306;
    }
  }

  in_addr addr{};
  const bool passive = hints && (hints->ai_flags & AI_PASSIVE);
  if (!node || !node[0]) {
    addr.s_addr = htonl(passive ? INADDR_ANY : INADDR_LOOPBACK);
  } else if (std::strcmp(node, "localhost") == 0) {
    addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (inet_pton(AF_INET, node, &addr) != 1) {
    return EAI_NONAME;
  }

  auto* sin = static_cast<sockaddr_in*>(std::calloc(1, sizeof(sockaddr_in)));
  auto* ai = static_cast<addrinfo*>(std::calloc(1, sizeof(addrinfo)));
  if (!sin || !ai) {
    std::free(sin);
    std::free(ai);
    return EAI_MEMORY;
  }
  sin->sin_family = AF_INET;
  sin->sin_port = htons(static_cast<uint16_t>(port));
  sin->sin_addr = addr;
  ai->ai_family = AF_INET;
  ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
  ai->ai_protocol = (hints && hints->ai_protocol) ? hints->ai_protocol : IPPROTO_TCP;
  ai->ai_addrlen = sizeof(sockaddr_in);
  ai->ai_addr = reinterpret_cast<sockaddr*>(sin);
  *res = ai;
  return 0;
}

void __wrap_freeaddrinfo(struct addrinfo* res) {
  while (res) {
    addrinfo* next = res->ai_next;
    std::free(res->ai_addr);
    std::free(res);
    res = next;
  }
}

const char* __wrap_gai_strerror(int) { return "getaddrinfo"; }

struct servent* __wrap_getservbyname(const char*, const char*) { return nullptr; }

struct hostent* __wrap_gethostbyname(const char*) { return nullptr; }

struct passwd* __wrap_getpwuid(uid_t) { return nullptr; }

void* __wrap_dlopen(const char*, int) { return nullptr; }

void* __wrap_dlsym(void*, const char*) { return nullptr; }

int __wrap_dlclose(void*) { return 0; }

const char* __wrap_dlerror() { return "dlopen disabled"; }

}  // extern "C"
