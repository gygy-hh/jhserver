#include "jh/gateway.hpp"

#include "jh/admin.hpp"
#include "jh/handlers.hpp"
#include "jh/update.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

namespace jh::gateway {

namespace {

constexpr uint64_t kMaxPayloadBytes = 64ULL * 1024 * 1024;
constexpr const char* kStreamUploadPath = "/admin/api/update/apk";

bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool is_reserved_path(const std::string& path) {
  return starts_with(path, "/admin") || starts_with(path, "/update");
}

Backend classify_request(const std::string& path) {
  if (path == "/health") {
    return Backend::Health;
  }
  if (starts_with(path, "/admin")) {
    return Backend::Admin;
  }
  if (starts_with(path, "/update")) {
    return Backend::Update;
  }
  return Backend::Game;
}

struct Trace {
  Backend backend = Backend::Game;
  std::chrono::steady_clock::time_point start{};
  bool active = false;
};

thread_local Trace g_trace;
thread_local bool g_on_slow_pool = false;

void begin_trace(const httplib::Request& req) {
  g_trace.backend = classify_request(req.path);
  g_trace.start = std::chrono::steady_clock::now();
  g_trace.active = true;
}

void finish_trace(const httplib::Request& req, const httplib::Response& res) {
  if (!g_trace.active) {
    return;
  }
  g_trace.active = false;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g_trace.start)
                           .count();
  // 合成一行再写。cerr 每次插入都会刷新，拆开写会堵在系统日志上，响应发不出去。
  std::string line;
  line.reserve(160);
  line += "[gateway] ";
  line += req.method;
  line += ' ';
  line += req.path;
  line += " -> ";
  line += backend_name(g_trace.backend);
  line += ' ';
  line += std::to_string(res.status);
  line += " body=";
  line += std::to_string(req.body.size());
  line += ' ';
  line += std::to_string(elapsed);
  line += "ms\n";
  std::cerr << line;
}

void reject(httplib::Response& res, int status, const std::string& body, const char* content_type) {
  res.status = status;
  res.set_content(body, content_type);
}

bool authorize_admin(const httplib::Request& req, httplib::Response& res, const ServerConfig& config) {
  if (!starts_with(req.path, "/admin")) {
    return true;
  }
  const auto expected =
      httplib::make_basic_authentication_header(config.admin_acc, config.admin_psw).second;
  if (!req.has_header("Authorization") || req.get_header_value("Authorization") != expected) {
    res.set_header("WWW-Authenticate", R"(Basic realm="jh-admin", charset="UTF-8")");
    reject(res, 401, R"({"error":"admin authentication required"})", "application/json; charset=utf-8");
    return false;
  }
  return true;
}

bool accept_payload(const httplib::Request& req, httplib::Response& res) {
  if (req.path == kStreamUploadPath || !req.has_header("Content-Length")) {
    return true;
  }
  try {
    if (std::stoull(req.get_header_value("Content-Length")) > kMaxPayloadBytes) {
      reject(res, 413, R"({"error":"payload too large"})", "application/json; charset=utf-8");
      return false;
    }
  } catch (...) {
    reject(res, 400, R"({"error":"bad request"})", "application/json; charset=utf-8");
    return false;
  }
  return true;
}

void close_accepted_socket(::socket_t sock) {
#ifdef _WIN32
  shutdown(sock, SD_BOTH);
  closesocket(sock);
#else
  shutdown(sock, SHUT_RDWR);
  close(sock);
#endif
}

void set_socket_timeouts(::socket_t sock, int seconds) {
#ifdef _WIN32
  const DWORD timeout = static_cast<DWORD>(seconds) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval tv{};
  tv.tv_sec = seconds;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// 请求行已经在缓冲里才分类。不能在这里等待，否则每个新连接都会占住轻线程。
bool peek_request_line(::socket_t sock, std::string& line) {
#ifdef _WIN32
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(sock, &fds);
  timeval tv{};
  if (select(static_cast<int>(sock + 1), &fds, nullptr, nullptr, &tv) <= 0) {
    return false;
  }
  int n = 0;
#else
  pollfd pfd{};
  pfd.fd = sock;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 0) <= 0) {
    return false;
  }
  ssize_t n = 0;
#endif
  char buf[512];
  n = ::recv(sock, buf, sizeof(buf), MSG_PEEK);
  if (n <= 0) {
    return false;
  }
  const void* end = std::memchr(buf, '\n', static_cast<size_t>(n));
  if (end == nullptr) {
    return false;
  }
  line.assign(buf, static_cast<size_t>(static_cast<const char*>(end) - buf));
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return !line.empty();
}

bool is_slow_transfer(const std::string& request_line) {
  const auto method_end = request_line.find(' ');
  if (method_end == std::string::npos) {
    return false;
  }
  const auto path_end = request_line.find(' ', method_end + 1);
  if (path_end == std::string::npos) {
    return false;
  }
  std::string path = request_line.substr(method_end + 1, path_end - method_end - 1);
  const auto query = path.find('?');
  if (query != std::string::npos) {
    path.resize(query);
  }
  return path == "/uploadSave" || path == "/downloadSave" || path == "/admin/api/update/apk" ||
         path.rfind("/update/apk/", 0) == 0 || path.rfind("/update/assets/", 0) == 0;
}

// 同一来源同时占住的连接数。超过就直接关掉，避免一个地址把工作线程耗尽。
constexpr int kMaxConnsPerPeer = 24;

// 连接一进来就丢弃，不进工作队列。
const std::unordered_set<std::string> kBannedPeers = {
    "175.152.190.5",
};

std::string normalize_peer(std::string peer) {
  const std::string mapped = "::ffff:";
  if (peer.rfind(mapped, 0) == 0) {
    peer.erase(0, mapped.size());
  }
  return peer;
}

bool peer_banned(const std::string& peer) {
  return kBannedPeers.find(normalize_peer(peer)) != kBannedPeers.end();
}

class PeerGate {
 public:
  bool admit(socket_t sock) {
    const std::string peer = peer_of(sock);
    if (peer_banned(peer)) {
      std::lock_guard<std::mutex> lock(mu_);
      const auto now = std::chrono::steady_clock::now();
      auto& logged = logged_[peer];
      if (now - logged >= std::chrono::seconds(30)) {
        logged = now;
        std::cerr << "[gateway] banned peer=" << peer << "\n";
      }
      return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    int& held = held_[peer];
    if (held >= kMaxConnsPerPeer) {
      const auto now = std::chrono::steady_clock::now();
      auto& logged = logged_[peer];
      if (now - logged >= std::chrono::seconds(5)) {
        logged = now;
        std::cerr << "[gateway] drop peer=" << peer << " held=" << held << "\n";
      }
      return false;
    }
    ++held;
    return true;
  }

  void release(socket_t sock) {
    const std::string peer = peer_of(sock);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = held_.find(peer);
    if (it == held_.end()) {
      return;
    }
    if (--it->second <= 0) {
      held_.erase(it);
    }
  }

 private:
  static std::string peer_of(socket_t sock) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      return "?";
    }
    char host[NI_MAXHOST];
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), nullptr, 0,
                    NI_NUMERICHOST) != 0) {
      return "?";
    }
    return host;
  }

  std::mutex mu_;
  std::unordered_map<std::string, int> held_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> logged_;
};

void configure_server(httplib::Server& server, const ServerConfig& config) {
  // 轻请求（getInitData、登录、检查更新）走这组。慢传输在 socket_handoff 里转走。
  // 线程大多堵在等客户端发包，不占 CPU，所以可以多于核数。
  server.new_task_queue = [] { return new httplib::ThreadPool(64, 256); };
  server.set_tcp_nodelay(true);
  // 连接上来却不发请求时，15 秒就放开线程。存档和安装包在认出路经后改成 5 分钟。
  server.set_read_timeout(15, 0);
  server.set_write_timeout(30, 0);
  server.set_keep_alive_max_count(1);
  server.set_keep_alive_timeout(5);
  server.set_payload_max_length(512ULL * 1024 * 1024);

  server.set_pre_routing_handler([&config](const httplib::Request& req, httplib::Response& res) {
    begin_trace(req);
    if (!authorize_admin(req, res, config) || !accept_payload(req, res)) {
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // 日志放在响应写完之后。写在回包前会被系统日志堵住，客户端会觉得每个请求都卡。
  server.set_logger([](const httplib::Request& req, const httplib::Response& res) { finish_trace(req, res); });

  httplib::Server::HandlerWithResponse on_error =
      [](const httplib::Request&, httplib::Response& res) {
        if (res.status == 404 && res.body.empty() && !res.content_provider_) {
          res.set_content(R"({"error":"not found"})", "application/json; charset=utf-8");
        }
        return httplib::Server::HandlerResponse::Unhandled;
      };
  server.set_error_handler(std::move(on_error));
}

}  // namespace

const char* backend_name(Backend backend) {
  switch (backend) {
    case Backend::Game:
      return "game";
    case Backend::Admin:
      return "admin";
    case Backend::Update:
      return "update";
    case Backend::Health:
      return "health";
  }
  return "unknown";
}

Backend Router::classify(const std::string& pattern) {
  if (pattern == "/health") {
    return Backend::Health;
  }
  if (pattern.find("/admin") != std::string::npos) {
    return Backend::Admin;
  }
  if (pattern.find("/update") != std::string::npos) {
    return Backend::Update;
  }
  return Backend::Game;
}

void Router::add(std::string method, std::string pattern, Handler handler) {
  Route route;
  route.method = std::move(method);
  route.pattern = std::move(pattern);
  route.backend = classify(route.pattern);
  route.handler = std::move(handler);
  routes_.push_back(std::move(route));
}

void Router::add_stream(std::string pattern, StreamHandler handler) {
  Route route;
  route.method = "POST";
  route.pattern = std::move(pattern);
  route.backend = classify(route.pattern);
  route.stream = true;
  route.stream_handler = std::move(handler);
  routes_.push_back(std::move(route));
}

void Router::Get(const std::string& pattern, Handler handler) {
  add("GET", pattern, std::move(handler));
}

void Router::Post(const std::string& pattern, Handler handler) {
  add("POST", pattern, std::move(handler));
}

void Router::Post(const std::string& pattern, StreamHandler handler) {
  add_stream(pattern, std::move(handler));
}

void Router::Delete(const std::string& pattern, Handler handler) {
  add("DELETE", pattern, std::move(handler));
}

void Router::mount(httplib::Server& server) const {
  for (const Route& route : routes_) {
    if (route.stream) {
      server.Post(route.pattern,
                  [route](const httplib::Request& req, httplib::Response& res,
                          const httplib::ContentReader& reader) {
                    route.stream_handler(req, res, reader);
                  });
      continue;
    }

    const auto wrapped = [route](const httplib::Request& req, httplib::Response& res) {
      if (route.backend == Backend::Game && is_reserved_path(req.path)) {
        res.status = 404;
        res.set_content(R"({"code":1,"msg":"not found"})", "application/json; charset=utf-8");
        return;
      }
      route.handler(req, res);
    };

    if (route.method == "GET") {
      server.Get(route.pattern, wrapped);
    } else if (route.method == "POST") {
      server.Post(route.pattern, wrapped);
    } else if (route.method == "DELETE") {
      server.Delete(route.pattern, wrapped);
    }
  }
}

int serve(const ServerConfig& config) {
  httplib::Server server;
  configure_server(server, config);
  PeerGate peers;
  server.socket_starting = [&peers](socket_t sock) { return peers.admit(sock); };
  server.socket_finished = [&peers](socket_t sock) { peers.release(sock); };

  // 存档和安装包传输。队列满了就留在轻线程上处理，不把这次上传直接断开。
  httplib::ThreadPool slow_pool(8, 32);
  server.keep_alive_adjust = [](size_t& count, time_t& sec) {
    if (!g_on_slow_pool) {
      return;
    }
    // 同一条连接上连续传多个存档。轻请求仍然一次就关，避免占住轮询线程。
    count = 32;
    sec = 15;
  };
  server.socket_handoff = [&server, &slow_pool](::socket_t sock) {
    if (g_on_slow_pool) {
      return false;
    }
    std::string request_line;
    if (!peek_request_line(sock, request_line) || !is_slow_transfer(request_line)) {
      return false;
    }
    set_socket_timeouts(sock, 300);
    const bool queued = slow_pool.enqueue([&server, sock]() {
      g_on_slow_pool = true;
      server.process_and_close_socket(sock);
    });
    if (!queued) {
      std::cerr << "[gateway] slow pool full, keep on light thread " << request_line << std::endl;
      return false;
    }
    return true;
  };

  Router router;
  admin::register_routes(router, config);
  update::register_routes(router, config);
  handlers::register_routes(router, config);
  router.mount(server);

  const int bind_flags = AI_PASSIVE | AI_NUMERICHOST;
  if (!server.bind_to_port(config.host, config.port, bind_flags)) {
    std::cerr << "Failed to bind " << config.host << ":" << config.port << std::endl;
    std::perror("bind");
    return 1;
  }

  std::cout << "JH game server listening on http://" << config.host << ":" << config.port << "/\n"
            << "  Gateway: /admin -> admin, /update -> update, POST /{action} -> game\n"
            << "  Admin panel: http://127.0.0.1:" << config.port << "/admin\n"
            << "  game_version(ver): " << config.game_version << "\n"
            << "  mysql: " << config.mysql.host << ":" << config.mysql.port << "/"
            << config.mysql.database << "\n"
            << "  redis: " << config.redis.host << ":" << config.redis.port << "/"
            << config.redis.database << "\n"
            << "  admin_acc: " << config.admin_acc << "\n"
            << std::flush;

  const bool listening = server.listen_after_bind();
  slow_pool.shutdown();
  if (!listening) {
    std::cerr << "Failed to listen on " << config.host << ":" << config.port << std::endl;
    return 1;
  }
  return 0;
}

}  // namespace jh::gateway
