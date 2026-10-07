#include "jh/redis.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace jh::redis {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

void close_socket(Socket socket) {
  if (socket == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

int socket_error() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool would_block(int error) {
#ifdef _WIN32
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

bool set_nonblocking(Socket socket, bool enabled) {
#ifdef _WIN32
  u_long mode = enabled ? 1 : 0;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

void set_io_timeout(Socket socket, int timeout_ms) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(std::max(timeout_ms, 1));
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = std::max(timeout_ms, 1) / 1000;
  timeout.tv_usec = (std::max(timeout_ms, 1) % 1000) * 1000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool connect_with_timeout(Socket socket, const sockaddr* address, socklen_t address_length,
                          int timeout_ms) {
  if (!set_nonblocking(socket, true)) {
    return false;
  }
  const int result = ::connect(socket, address, address_length);
  if (result != 0 && !would_block(socket_error())) {
    set_nonblocking(socket, false);
    return false;
  }
  if (result != 0) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket, &write_set);
    timeval timeout{};
    timeout.tv_sec = std::max(timeout_ms, 1) / 1000;
    timeout.tv_usec = (std::max(timeout_ms, 1) % 1000) * 1000;
#ifdef _WIN32
    const int selected = select(0, nullptr, &write_set, nullptr, &timeout);
#else
    const int selected = select(socket + 1, nullptr, &write_set, nullptr, &timeout);
#endif
    if (selected <= 0) {
      set_nonblocking(socket, false);
      return false;
    }
    int error = 0;
    socklen_t error_length = sizeof(error);
#ifdef _WIN32
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_length) !=
        0) {
#else
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0) {
#endif
      set_nonblocking(socket, false);
      return false;
    }
    if (error != 0) {
      set_nonblocking(socket, false);
      return false;
    }
  }
  return set_nonblocking(socket, false);
}

struct Reply {
  enum class Type { String, Error, Integer, Nil, Array };
  Type type = Type::Nil;
  std::string string;
  int64_t integer = 0;
  std::vector<Reply> array;
};

}  // namespace

struct Client::Impl {
  explicit Impl(RedisConfig config) : config(std::move(config)) {}

  RedisConfig config;
  Socket socket = kInvalidSocket;
  std::mutex mutex;
  std::string error;

  ~Impl() {
    close_unlocked();
  }

  void close_unlocked() {
    close_socket(socket);
    socket = kInvalidSocket;
  }

  void send_all_unlocked(const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
#ifdef _WIN32
      const int chunk = static_cast<int>(
          std::min<size_t>(data.size() - offset, static_cast<size_t>(std::numeric_limits<int>::max())));
      const int sent = ::send(socket, data.data() + offset, chunk, 0);
#else
      const ssize_t sent = ::send(socket, data.data() + offset, data.size() - offset, 0);
#endif
      if (sent <= 0) {
        throw std::runtime_error("Redis send failed");
      }
      offset += static_cast<size_t>(sent);
    }
  }

  char receive_char_unlocked() {
    char value = 0;
#ifdef _WIN32
    const int received = recv(socket, &value, 1, 0);
#else
    const ssize_t received = recv(socket, &value, 1, 0);
#endif
    if (received != 1) {
      throw std::runtime_error("Redis connection closed");
    }
    return value;
  }

  std::string receive_line_unlocked() {
    std::string line;
    line.reserve(64);
    while (line.size() < 1024 * 1024) {
      const char c = receive_char_unlocked();
      if (c == '\r') {
        if (receive_char_unlocked() != '\n') {
          throw std::runtime_error("Invalid Redis response terminator");
        }
        return line;
      }
      line.push_back(c);
    }
    throw std::runtime_error("Redis response line too large");
  }

  std::string receive_bytes_unlocked(size_t length) {
    std::string data(length, '\0');
    size_t offset = 0;
    while (offset < length) {
#ifdef _WIN32
      const int chunk = static_cast<int>(
          std::min<size_t>(length - offset, static_cast<size_t>(std::numeric_limits<int>::max())));
      const int received = recv(socket, data.data() + offset, chunk, 0);
#else
      const ssize_t received = recv(socket, data.data() + offset, length - offset, 0);
#endif
      if (received <= 0) {
        throw std::runtime_error("Redis connection closed");
      }
      offset += static_cast<size_t>(received);
    }
    return data;
  }

  Reply read_reply_unlocked() {
    const char prefix = receive_char_unlocked();
    if (prefix == '+' || prefix == '-') {
      Reply reply;
      reply.type = prefix == '+' ? Reply::Type::String : Reply::Type::Error;
      reply.string = receive_line_unlocked();
      return reply;
    }
    if (prefix == ':') {
      Reply reply;
      reply.type = Reply::Type::Integer;
      reply.integer = std::stoll(receive_line_unlocked());
      return reply;
    }
    if (prefix == '$') {
      const int64_t length = std::stoll(receive_line_unlocked());
      if (length < 0) {
        return Reply{};
      }
      if (length > 64 * 1024 * 1024) {
        throw std::runtime_error("Redis bulk response too large");
      }
      Reply reply;
      reply.type = Reply::Type::String;
      reply.string = receive_bytes_unlocked(static_cast<size_t>(length));
      if (receive_char_unlocked() != '\r' || receive_char_unlocked() != '\n') {
        throw std::runtime_error("Invalid Redis bulk response terminator");
      }
      return reply;
    }
    if (prefix == '*') {
      const int64_t count = std::stoll(receive_line_unlocked());
      if (count < 0) {
        return Reply{};
      }
      if (count > 1000000) {
        throw std::runtime_error("Redis array response too large");
      }
      Reply reply;
      reply.type = Reply::Type::Array;
      reply.array.reserve(static_cast<size_t>(count));
      for (int64_t i = 0; i < count; ++i) {
        reply.array.push_back(read_reply_unlocked());
      }
      return reply;
    }
    throw std::runtime_error("Unknown Redis response type");
  }

  Reply execute_raw_unlocked(const std::vector<std::string>& arguments) {
    std::string request = "*" + std::to_string(arguments.size()) + "\r\n";
    for (const auto& argument : arguments) {
      request += "$" + std::to_string(argument.size()) + "\r\n";
      request += argument;
      request += "\r\n";
    }
    send_all_unlocked(request);
    return read_reply_unlocked();
  }

  bool connect_unlocked() {
#ifdef _WIN32
    static std::once_flag winsock_once;
    static int winsock_result = WSASYSNOTREADY;
    std::call_once(winsock_once, [] {
      WSADATA data{};
      winsock_result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (winsock_result != 0) {
      error = "WSAStartup failed";
      return false;
    }
#endif
    close_unlocked();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(config.port);
    if (getaddrinfo(config.host.c_str(), port.c_str(), &hints, &addresses) != 0) {
      error = "Redis host resolution failed: " + config.host;
      return false;
    }
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
      Socket candidate =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate == kInvalidSocket) {
        continue;
      }
      if (connect_with_timeout(candidate, address->ai_addr,
                               static_cast<socklen_t>(address->ai_addrlen),
                               config.connect_timeout_ms)) {
        socket = candidate;
        break;
      }
      close_socket(candidate);
    }
    freeaddrinfo(addresses);
    if (socket == kInvalidSocket) {
      error = "Redis connection failed: " + config.host + ":" + port;
      return false;
    }
    set_io_timeout(socket, config.connect_timeout_ms);
    try {
      if (!config.password.empty()) {
        const Reply auth = execute_raw_unlocked({"AUTH", config.password});
        if (auth.type == Reply::Type::Error) {
          error = "Redis AUTH failed: " + auth.string;
          close_unlocked();
          return false;
        }
      }
      if (config.database != 0) {
        const Reply select = execute_raw_unlocked({"SELECT", std::to_string(config.database)});
        if (select.type == Reply::Type::Error) {
          error = "Redis SELECT failed: " + select.string;
          close_unlocked();
          return false;
        }
      }
    } catch (const std::exception& ex) {
      error = ex.what();
      close_unlocked();
      return false;
    }
    return true;
  }

  Reply command_unlocked(const std::vector<std::string>& arguments) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (socket == kInvalidSocket && !connect_unlocked()) {
        continue;
      }
      try {
        Reply reply = execute_raw_unlocked(arguments);
        if (reply.type == Reply::Type::Error) {
          error = reply.string;
        } else {
          error.clear();
        }
        return reply;
      } catch (const std::exception& ex) {
        error = ex.what();
        close_unlocked();
      }
    }
    Reply reply;
    reply.type = Reply::Type::Error;
    reply.string = error.empty() ? "Redis command failed" : error;
    return reply;
  }
};

Client::Client(RedisConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() = default;

bool Client::ping() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked({"PING"});
  return reply.type == Reply::Type::String && reply.string == "PONG";
}

std::optional<std::string> Client::get(const std::string& key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked({"GET", key});
  if (reply.type == Reply::Type::String) {
    return reply.string;
  }
  return std::nullopt;
}

bool Client::set_ex(const std::string& key, const std::string& value, int ttl_seconds) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply =
      impl_->command_unlocked({"SET", key, value, "EX", std::to_string(ttl_seconds)});
  return reply.type == Reply::Type::String && reply.string == "OK";
}

bool Client::set_nx_ex(const std::string& key, const std::string& value, int ttl_seconds) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply =
      impl_->command_unlocked({"SET", key, value, "NX", "EX", std::to_string(ttl_seconds)});
  return reply.type == Reply::Type::String && reply.string == "OK";
}

bool Client::del(const std::string& key) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked({"DEL", key});
  return reply.type == Reply::Type::Integer;
}

bool Client::zadd(const std::string& key, int64_t score, const std::string& member) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked({"ZADD", key, std::to_string(score), member});
  return reply.type == Reply::Type::Integer;
}

bool Client::zrem(const std::string& key, const std::string& member) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked({"ZREM", key, member});
  return reply.type == Reply::Type::Integer && reply.integer > 0;
}

bool Client::zremrangebyscore(const std::string& key, int64_t min_score, int64_t max_score) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked(
      {"ZREMRANGEBYSCORE", key, std::to_string(min_score), std::to_string(max_score)});
  return reply.type == Reply::Type::Integer;
}

std::vector<std::string> Client::zrangebyscore(const std::string& key, int64_t min_score,
                                               const std::string& max_score) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const Reply reply = impl_->command_unlocked(
      {"ZRANGEBYSCORE", key, std::to_string(min_score), max_score});
  std::vector<std::string> values;
  if (reply.type != Reply::Type::Array) {
    return values;
  }
  values.reserve(reply.array.size());
  for (const auto& item : reply.array) {
    if (item.type == Reply::Type::String) {
      values.push_back(item.string);
    }
  }
  return values;
}

const std::string& Client::last_error() const {
  return impl_->error;
}

}  // namespace jh::redis
