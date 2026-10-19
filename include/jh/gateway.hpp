#pragma once

#include "jh/config.hpp"

#include <httplib.h>

#include <string>
#include <vector>

namespace jh::gateway {

// 请求被转发到的后端。网关按路径归属选择，业务模块不再直接绑定端口。
enum class Backend {
  Game,
  Admin,
  Update,
  Health,
};

const char* backend_name(Backend backend);

// 路由表。模块只往这里登记处理函数，真正挂到 HTTP 服务上由网关完成。
class Router {
 public:
  using Handler = httplib::Server::Handler;
  using StreamHandler = httplib::Server::HandlerWithContentReader;

  void Get(const std::string& pattern, Handler handler);
  void Post(const std::string& pattern, Handler handler);
  void Post(const std::string& pattern, StreamHandler handler);
  void Delete(const std::string& pattern, Handler handler);

  void mount(httplib::Server& server) const;

 private:
  struct Route {
    std::string method;
    std::string pattern;
    Backend backend = Backend::Game;
    bool stream = false;
    Handler handler;
    StreamHandler stream_handler;
  };

  void add(std::string method, std::string pattern, Handler handler);
  void add_stream(std::string pattern, StreamHandler handler);
  static Backend classify(const std::string& pattern);

  std::vector<Route> routes_;
};

// 启动网关：统一鉴权、体积限制、访问日志，再按 admin → update → game 转发。
// 返回进程退出码。
int serve(const ServerConfig& config);

}  // namespace jh::gateway
