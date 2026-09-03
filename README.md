# JH Game Server (C++)

与 `libcocos2dcpp.so` 客户端协议对齐的 HTTP 服务端，默认端口 **8888**。

## 依赖

- CMake 3.16+
- C++17 编译器（MSVC / GCC / Clang）
- 网络（首次构建会通过 FetchContent 下载 `cpp-httplib` 与 `nlohmann/json`）

## 构建

```bash
cmake -S . -B build
cmake --build build --config Release
```

Windows Release 可执行文件：`build/Release/jh_server.exe`

复制 `config.example.json` 为 `config.json`，填写 MySQL 和管理员密码后再启动。

## 运行

```bash
./build/jh_server
# 或指定参数
./build/jh_server --port 8888 --version 478 --config config.json
```

## 客户端接入

1. 将客户端 `JhData::g_url` 指向本机，例如 `http://127.0.0.1:8888/`
2. 确保 URL 参数 `ver` 与 `config.json` 中 `game_version` 一致（密钥 = `MD5(str(ver))` 前 16 位 hex）
3. `plat=ANDR`，`channel` 默认 `none`

请求格式：

```
POST /{action}?plat=ANDR&ver=478&channel=none
Body: Base64(XXTEA(JSON))   # smsCode 例外：acc=手机号
```

## 已实现接口

| 端点 | 说明 |
|------|------|
| `getInitData` | 初始化，**响应加密** |
| `login` | 手机+密码登录（MySQL 校验，新号自动注册） |
| `register` / `regist` | 同 `login` |
| `smsCode` | 兼容客户端 UI，直接返回成功（不发送短信） |
| `mail` | 邮箱登录/注册（MySQL 校验密码） |
| `uploadSave` / `downloadSave` | 云存档 |
| 其他 | 返回 `{"code":0}` 占位 |

## 调试

- `GET /health` — 健康检查
- `GET /debug/key?ver=478` — 查看当前版本加解密密钥

## Web 管理后台

浏览器打开：**http://127.0.0.1:8888/admin**（无需登录）

功能：服务概览、账号列表、云存档查看/删除、配置信息

API：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/admin/api/stats` | 统计与配置 |
| GET | `/admin/api/accounts` | 账号列表 |
| GET | `/admin/api/saves` | 存档列表 |
| GET | `/admin/api/save?acc=&area=` | 存档预览 |
| DELETE | `/admin/api/save?acc=&area=` | 删除存档 |
| DELETE | `/admin/api/account?acc=` | 删除账号及存档 |

## 登录鉴权（短信验证码）

与原版 APK 一致：手机号登录走短信 OTP，`psw` 字段传验证码（不是密码）。

`config.json` 示例：

```json
{
  "admin_acc": "19848015669",
  "dev_sms_code": "888888",
  "sms_ttl_sec": 300,
  "sms_cooldown_sec": 60,
  "min_password_len": 6
}
```

流程：

1. `smsCode`：POST 明文 `acc=手机号`，返回 `{"code":0,"msg":""}`；验证码写入 `data/sms_codes.json`（开发模式固定为 `dev_sms_code`）
2. `login`（手机号）：校验 `psw` 是否为有效验证码，通过后自动注册/登录
3. `login`（邮箱）：`psw` 为密码，存于 `data/accounts.json`

后台 `POST /admin/api/account/password` 可为邮箱账号设置密码。

## 数据目录

- `data/accounts.json` — 账号与内部 `dataAccount` id
- `data/saves/{acc}_{area}.json` — 云存档

## 你需要提供

1. **客户端版本号 `ver`**（与 APK 一致，默认 478）
2. **服务器地址**（改客户端或 hosts）
3. 登录方式与活动 JSON（`huoDong` 等）按需扩展 `src/handlers.cpp`
