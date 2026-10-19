#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace jh::lunjian {

void init(const std::string& data_dir);

// 非论剑接口返回 nullopt，由其他模块继续处理。
std::optional<nlohmann::json> handle(const std::string& action, const nlohmann::json& req,
                                     const std::string& channel);

}  // namespace jh::lunjian
