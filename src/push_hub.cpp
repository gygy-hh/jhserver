#include "jh/push_hub.hpp"

#include "jh/session.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace jh::push_hub {

namespace {

using json = nlohmann::json;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u;
constexpr int kIdleTimeoutSec = 60;
constexpr int kAcceptPollMs = 500;

void close_socket(Socket sock) {
  if (sock == kInvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(sock);
#else
  ::close(sock);
#endif
}

bool set_nonblocking(Socket sock, bool on) {
#ifdef _WIN32
  u_long mode = on ? 1 : 0;
  return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(sock, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string peer_of(Socket sock) {
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

bool send_all(Socket sock, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
#ifdef _WIN32
    const int n = ::send(sock, data + sent, static_cast<int>(len - sent), 0);
#else
    const ssize_t n = ::send(sock, data + sent, len - sent, MSG_NOSIGNAL);
#endif
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool send_frame(Socket sock, const std::string& body, std::mutex& write_mu) {
  if (body.size() > kMaxFrameBytes) {
    return false;
  }
  std::uint8_t header[4];
  const std::uint32_t n = static_cast<std::uint32_t>(body.size());
  header[0] = static_cast<std::uint8_t>((n >> 24) & 0xff);
  header[1] = static_cast<std::uint8_t>((n >> 16) & 0xff);
  header[2] = static_cast<std::uint8_t>((n >> 8) & 0xff);
  header[3] = static_cast<std::uint8_t>(n & 0xff);
  std::lock_guard<std::mutex> lock(write_mu);
  return send_all(sock, reinterpret_cast<const char*>(header), 4) &&
         send_all(sock, body.data(), body.size());
}

struct Conn : std::enable_shared_from_this<Conn> {
  Socket sock = kInvalidSocket;
  std::string acc;
  std::string peer;
  std::string map;
  std::string nick;
  int x = 0;
  int y = 0;
  int64_t connected_at = 0;
  std::atomic<int64_t> last_active{0};
  std::mutex write_mu;
  std::atomic<bool> closed{false};

  void close() {
    if (closed.exchange(true)) {
      return;
    }
    if (sock != kInvalidSocket) {
#ifdef _WIN32
      shutdown(sock, SD_BOTH);
#else
      shutdown(sock, SHUT_RDWR);
#endif
      close_socket(sock);
      sock = kInvalidSocket;
    }
  }
};

struct PendingWaveRoute {
  std::weak_ptr<Conn> sender;
  std::string target_acc;
  std::string to;
  int64_t created_at = 0;
};

struct Hub {
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<Conn>> by_acc;
  std::unordered_map<std::string, PendingWaveRoute> pending_waves;
  Socket listen_sock = kInvalidSocket;
  std::thread accept_thread;
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> push_seq{1};
  // 每条连接占一个线程，超过服务的 TasksMax 会让整个进程（含 HTTP 网关）崩溃，必须在这里截住。
  std::atomic<int> active{0};
  std::atomic<std::uint64_t> rejected{0};
  int max_conns = 0;
  std::string host;
  int port = 0;
};

Hub g_hub;

bool ensure_wsa() {
#ifdef _WIN32
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return ok;
#else
  return true;
#endif
}

Socket create_listen_socket(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  const char* node = host.empty() || host == "0.0.0.0" || host == "::" ? nullptr : host.c_str();
  if (getaddrinfo(node, port_str.c_str(), &hints, &res) != 0 || !res) {
    return kInvalidSocket;
  }

  Socket sock = kInvalidSocket;
  for (addrinfo* it = res; it; it = it->ai_next) {
    sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (sock == kInvalidSocket) {
      continue;
    }
    int yes = 1;
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    if (::bind(sock, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0 &&
        ::listen(sock, 128) == 0) {
      break;
    }
    close_socket(sock);
    sock = kInvalidSocket;
  }
  freeaddrinfo(res);
  return sock;
}

bool read_exact(Socket sock, char* buf, std::size_t len, Conn& conn) {
  std::size_t got = 0;
  while (got < len) {
    if (conn.closed.load()) {
      return false;
    }
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int pr = WSAPoll(&pfd, 1, 1000);
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, 1000);
#endif
    if (pr < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEINTR) {
        continue;
      }
#else
      if (errno == EINTR) {
        continue;
      }
#endif
      return false;
    }
    if (pr == 0) {
      const int64_t idle = now_unix() - conn.last_active.load();
      if (idle >= kIdleTimeoutSec) {
        return false;
      }
      continue;
    }
#ifdef _WIN32
    const int n = ::recv(sock, buf + got, static_cast<int>(len - got), 0);
#else
    const ssize_t n = ::recv(sock, buf + got, len - got, 0);
#endif
    if (n <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
    conn.last_active.store(now_unix());
  }
  return true;
}

bool read_frame(Conn& conn, std::string& out) {
  std::uint8_t header[4];
  if (!read_exact(conn.sock, reinterpret_cast<char*>(header), 4, conn)) {
    return false;
  }
  const std::uint32_t n = (static_cast<std::uint32_t>(header[0]) << 24) |
                          (static_cast<std::uint32_t>(header[1]) << 16) |
                          (static_cast<std::uint32_t>(header[2]) << 8) |
                          static_cast<std::uint32_t>(header[3]);
  if (n == 0 || n > kMaxFrameBytes) {
    return false;
  }
  out.resize(n);
  return read_exact(conn.sock, out.data(), n, conn);
}

void unregister_conn(const std::shared_ptr<Conn>& conn) {
  if (!conn || conn->acc.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_hub.mu);
  const auto it = g_hub.by_acc.find(conn->acc);
  if (it != g_hub.by_acc.end() && it->second.get() == conn.get()) {
    g_hub.by_acc.erase(it);
  }
}

void register_conn(const std::shared_ptr<Conn>& conn) {
  std::shared_ptr<Conn> old;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    auto& slot = g_hub.by_acc[conn->acc];
    if (slot && slot.get() != conn.get()) {
      old = slot;
    }
    slot = conn;
  }
  if (old) {
    old->close();
  }
}

json build_map_players_locked(const std::string& map) {
  json players = json::array();
  if (map.empty()) {
    return json{{"type", "map_players"}, {"map", map}, {"players", players}};
  }
  for (const auto& kv : g_hub.by_acc) {
    const auto& c = kv.second;
    if (!c || c->closed.load() || c->map != map) {
      continue;
    }
    players.push_back({{"acc", c->acc},
                       {"nick", c->nick.empty() ? c->acc : c->nick},
                       {"x", c->x},
                       {"y", c->y}});
  }
  return json{{"type", "map_players"}, {"map", map}, {"players", std::move(players)}};
}

void notify_map(const std::string& map) {
  if (map.empty()) {
    return;
  }
  std::vector<std::shared_ptr<Conn>> targets;
  json body;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    body = build_map_players_locked(map);
    targets.reserve(g_hub.by_acc.size());
    for (const auto& kv : g_hub.by_acc) {
      if (kv.second && !kv.second->closed.load() && kv.second->map == map) {
        targets.push_back(kv.second);
      }
    }
  }
  const std::string payload = body.dump();
  for (auto& c : targets) {
    if (c) {
      send_frame(c->sock, payload, c->write_mu);
    }
  }
}

void handle_presence(const std::shared_ptr<Conn>& conn, const json& msg) {
  std::string map = msg.value("map", "");
  std::string nick = msg.value("nick", "");
  const int x = msg.value("x", 0);
  const int y = msg.value("y", 0);
  if (map.size() > 64) {
    map.resize(64);
  }
  if (nick.size() > 32) {
    nick.resize(32);
  }
  std::string old_map;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    old_map = conn->map;
    conn->map = map;
    conn->x = x;
    conn->y = y;
    if (!nick.empty()) {
      conn->nick = nick;
    }
  }
  std::cerr << "[push] presence acc=" << conn->acc << " map=" << map << " nick="
            << (nick.empty() ? conn->nick : nick) << " xy=(" << x << "," << y << ")\n";
  if (old_map != map) {
    notify_map(old_map);
  }
  notify_map(map);
}

void handle_wave(const std::shared_ptr<Conn>& conn, const json& msg) {
  const std::string to = msg.value("to", "");
  std::string id = msg.value("id", "");
  if (to.empty() || to == conn->acc) {
    return;
  }
  if (id.size() > 64) {
    id.resize(64);
  }
  std::shared_ptr<Conn> target;
  std::string from_nick;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    from_nick = conn->nick.empty() ? conn->acc : conn->nick;
    const auto it = g_hub.by_acc.find(to);
    if (it != g_hub.by_acc.end()) {
      target = it->second;
    }
  }
  if (!target || target->closed.load()) {
    const json fail{{"type", "wave_fail"}, {"id", id}, {"to", to}, {"msg", "offline"}};
    send_frame(conn->sock, fail.dump(), conn->write_mu);
    std::cerr << "[push] wave_fail id=" << id << " from=" << conn->acc
              << " to=" << to << " reason=offline\n";
    return;
  }
  const json wave{
      {"type", "wave"}, {"id", id}, {"from", conn->acc}, {"nick", from_nick}};
  if (!id.empty()) {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    const int64_t cutoff = now_unix() - 60;
    for (auto it = g_hub.pending_waves.begin(); it != g_hub.pending_waves.end();) {
      if (it->second.created_at < cutoff) {
        it = g_hub.pending_waves.erase(it);
      } else {
        ++it;
      }
    }
    g_hub.pending_waves[id] =
        PendingWaveRoute{conn, target->acc, to, now_unix()};
  }
  if (!send_frame(target->sock, wave.dump(), target->write_mu)) {
    target->close();
    if (!id.empty()) {
      std::lock_guard<std::mutex> lock(g_hub.mu);
      g_hub.pending_waves.erase(id);
    }
    const json fail{
        {"type", "wave_fail"}, {"id", id}, {"to", to}, {"msg", "send failed"}};
    send_frame(conn->sock, fail.dump(), conn->write_mu);
    std::cerr << "[push] wave_fail id=" << id << " from=" << conn->acc
              << " to=" << to << " reason=send\n";
    return;
  }
  if (!id.empty()) {
    std::cerr << "[push] wave_forward id=" << id << " from=" << conn->acc
              << " to=" << to << "\n";
    return;
  }
  // 兼容旧客户端：旧协议没有 id，也没有接收端 wave_ack。
  const json ok{{"type", "wave_ok"}, {"id", id}, {"to", to}};
  if (send_frame(conn->sock, ok.dump(), conn->write_mu)) {
    std::cerr << "[push] wave_ok id=" << id << " from=" << conn->acc
              << " to=" << to << "\n";
  }
}

void handle_wave_ack(const std::shared_ptr<Conn>& conn, const json& msg) {
  const std::string id = msg.value("id", "");
  if (id.empty()) {
    return;
  }
  PendingWaveRoute route;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    const auto it = g_hub.pending_waves.find(id);
    if (it == g_hub.pending_waves.end() || it->second.target_acc != conn->acc) {
      return;
    }
    route = it->second;
    g_hub.pending_waves.erase(it);
  }
  const auto sender = route.sender.lock();
  if (!sender || sender->closed.load()) {
    return;
  }
  const json ok{{"type", "wave_ok"}, {"id", id}, {"to", route.to}};
  if (send_frame(sender->sock, ok.dump(), sender->write_mu)) {
    std::cerr << "[push] wave_ok id=" << id << " from=" << sender->acc
              << " to=" << route.to << " receiver_ack=1\n";
  }
}

void handle_client(std::shared_ptr<Conn> conn) {
  std::string frame;
  bool authed = false;
  std::string left_map;

  while (g_hub.running.load() && !conn->closed.load()) {
    if (!read_frame(*conn, frame)) {
      break;
    }

    json msg;
    try {
      msg = json::parse(frame);
    } catch (...) {
      break;
    }
    const std::string type = msg.value("type", "");
    if (!authed) {
      if (type != "auth") {
        const json fail{{"type", "auth_fail"}, {"msg", "auth required"}};
        send_frame(conn->sock, fail.dump(), conn->write_mu);
        break;
      }
      const std::string session = msg.value("session", "");
      const auto acc = session_validate(session);
      if (!acc) {
        const json fail{{"type", "auth_fail"}, {"msg", "invalid session"}};
        send_frame(conn->sock, fail.dump(), conn->write_mu);
        break;
      }
      conn->acc = *acc;
      register_conn(conn);
      authed = true;
      const json ok{{"type", "auth_ok"}, {"acc", conn->acc}};
      if (!send_frame(conn->sock, ok.dump(), conn->write_mu)) {
        break;
      }
      std::cerr << "[push] auth_ok acc=" << conn->acc << " peer=" << conn->peer << "\n";
      continue;
    }

    if (type == "ping") {
      const json pong{{"type", "pong"}};
      if (!send_frame(conn->sock, pong.dump(), conn->write_mu)) {
        break;
      }
      continue;
    }
    if (type == "ack") {
      continue;
    }
    if (type == "presence") {
      handle_presence(conn, msg);
      continue;
    }
    if (type == "wave") {
      handle_wave(conn, msg);
      continue;
    }
    if (type == "wave_ack") {
      handle_wave_ack(conn, msg);
      continue;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    left_map = conn->map;
  }
  unregister_conn(conn);
  conn->close();
  if (!left_map.empty()) {
    notify_map(left_map);
  }
  if (!conn->acc.empty()) {
    std::cerr << "[push] disconnect acc=" << conn->acc << " peer=" << conn->peer << "\n";
  }
}

void accept_loop() {
  while (g_hub.running.load()) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = g_hub.listen_sock;
    pfd.events = POLLIN;
    const int pr = WSAPoll(&pfd, 1, kAcceptPollMs);
#else
    pollfd pfd{};
    pfd.fd = g_hub.listen_sock;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, kAcceptPollMs);
#endif
    if (pr <= 0) {
      continue;
    }

    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    const Socket client =
        ::accept(g_hub.listen_sock, reinterpret_cast<sockaddr*>(&addr), &len);
    if (client == kInvalidSocket) {
      continue;
    }

    int yes = 1;
#ifdef _WIN32
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#endif
    set_nonblocking(client, false);

    if (g_hub.active.load() >= g_hub.max_conns) {
      close_socket(client);
      const std::uint64_t n = g_hub.rejected.fetch_add(1) + 1;
      if (n == 1 || n % 100 == 0) {
        std::cerr << "[push] at capacity " << g_hub.max_conns << ", rejected total=" << n << "\n";
      }
      continue;
    }

    auto conn = std::make_shared<Conn>();
    conn->sock = client;
    conn->peer = peer_of(client);
    conn->connected_at = now_unix();
    conn->last_active.store(conn->connected_at);
    g_hub.active.fetch_add(1);
    try {
      std::thread([conn] {
        handle_client(conn);
        g_hub.active.fetch_sub(1);
      }).detach();
    } catch (const std::exception& e) {
      g_hub.active.fetch_sub(1);
      conn->close();
      std::cerr << "[push] spawn thread failed: " << e.what() << "\n";
    }
  }
}

}  // namespace

bool start(const ServerConfig& config) {
  if (config.push_port <= 0) {
    std::cerr << "[push] disabled (push_port=" << config.push_port << ")\n";
    return true;
  }
  if (g_hub.running.load()) {
    return true;
  }
  if (!ensure_wsa()) {
    std::cerr << "[push] WSAStartup failed\n";
    return false;
  }

  const Socket listen_sock = create_listen_socket(config.host, config.push_port);
  if (listen_sock == kInvalidSocket) {
    std::cerr << "[push] failed to bind " << config.host << ":" << config.push_port << "\n";
    return false;
  }

  g_hub.listen_sock = listen_sock;
  g_hub.host = config.host;
  g_hub.port = config.push_port;
  g_hub.max_conns = config.push_max_conns > 0 ? config.push_max_conns : 1500;
  g_hub.running.store(true);
  g_hub.accept_thread = std::thread(accept_loop);
  std::cout << "[push] listening on tcp://" << config.host << ":" << config.push_port << "\n"
            << std::flush;
  return true;
}

void stop() {
  if (!g_hub.running.exchange(false)) {
    return;
  }
  if (g_hub.listen_sock != kInvalidSocket) {
    close_socket(g_hub.listen_sock);
    g_hub.listen_sock = kInvalidSocket;
  }
  if (g_hub.accept_thread.joinable()) {
    g_hub.accept_thread.join();
  }

  std::vector<std::shared_ptr<Conn>> conns;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    conns.reserve(g_hub.by_acc.size());
    for (auto& kv : g_hub.by_acc) {
      conns.push_back(kv.second);
    }
    g_hub.by_acc.clear();
  }
  for (auto& c : conns) {
    if (c) {
      c->close();
    }
  }
}

std::size_t online_count() {
  std::lock_guard<std::mutex> lock(g_hub.mu);
  return g_hub.by_acc.size();
}

std::vector<OnlineClient> list_online() {
  std::vector<OnlineClient> out;
  std::lock_guard<std::mutex> lock(g_hub.mu);
  out.reserve(g_hub.by_acc.size());
  for (const auto& kv : g_hub.by_acc) {
    OnlineClient item;
    item.acc = kv.first;
    if (kv.second) {
      item.peer = kv.second->peer;
      item.map = kv.second->map;
      item.nick = kv.second->nick;
      item.x = kv.second->x;
      item.y = kv.second->y;
      item.connected_at = kv.second->connected_at;
      item.last_active = kv.second->last_active.load();
    }
    out.push_back(std::move(item));
  }
  return out;
}

bool push_to_acc(const std::string& acc, const std::string& msg, std::string* error) {
  std::shared_ptr<Conn> conn;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    const auto it = g_hub.by_acc.find(acc);
    if (it == g_hub.by_acc.end() || !it->second) {
      if (error) {
        *error = "offline";
      }
      return false;
    }
    conn = it->second;
  }
  const std::uint64_t id = g_hub.push_seq.fetch_add(1);
  const json body{{"type", "push"}, {"id", std::to_string(id)}, {"msg", msg}};
  if (!send_frame(conn->sock, body.dump(), conn->write_mu)) {
    if (error) {
      *error = "send failed";
    }
    conn->close();
    return false;
  }
  return true;
}

std::size_t broadcast(const std::string& msg) {
  std::vector<std::shared_ptr<Conn>> conns;
  {
    std::lock_guard<std::mutex> lock(g_hub.mu);
    conns.reserve(g_hub.by_acc.size());
    for (auto& kv : g_hub.by_acc) {
      conns.push_back(kv.second);
    }
  }
  std::size_t ok = 0;
  for (auto& conn : conns) {
    if (!conn) {
      continue;
    }
    const std::uint64_t id = g_hub.push_seq.fetch_add(1);
    const json body{{"type", "push"}, {"id", std::to_string(id)}, {"msg", msg}};
    if (send_frame(conn->sock, body.dump(), conn->write_mu)) {
      ++ok;
    } else {
      conn->close();
    }
  }
  return ok;
}

}  // namespace jh::push_hub
