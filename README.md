# JH Game Server (C++)

与 `libcocos2dcpp.so` 客户端协议对齐的 HTTP 服务端，默认端口 **8888**。

## 依赖

- CMake 3.16+
- C++17 编译器（MSVC / GCC / Clang）
- Redis 6+（邮件存储，默认 `127.0.0.1:6379`）
- 网络（首次构建会通过 FetchContent 下载 `cpp-httplib` 与 `nlohmann/json`）

## 构建

```bash
cmake -S . -B build
cmake --build build --config Release
```

Windows Release 可执行文件：`build/Release/jh_server.exe`

复制 `config.example.json` 为 `config.json`，填写 MySQL、Redis 和管理员密码后再启动。

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

浏览器打开：**http://127.0.0.1:8888/admin**（使用 `admin_acc` / `admin_psw` HTTP Basic 登录）

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
| GET | `/admin/api/mails?channel=` | 查询渠道内全部有效邮件 |
| GET | `/admin/api/mail?channel=&acc=&area=` | 查询邮件详情 |
| POST | `/admin/api/mail` | 覆盖发送个人邮件 |
| DELETE | `/admin/api/mail?channel=&acc=&area=` | 删除个人邮件 |
| GET | `/admin/api/global-mail?channel=` | 查询渠道全体邮件 |
| POST | `/admin/api/global-mail` | 发送渠道全体邮件（无需账号、区服） |
| DELETE | `/admin/api/global-mail?channel=` | 删除渠道全体邮件 |
| GET/POST | `/admin/api/global-mail/schedules` | 查询或创建全体邮件定时任务 |
| DELETE | `/admin/api/global-mail/schedules?id=` | 取消全体邮件定时任务 |

## Redis 邮件

邮件完全采用覆盖式活动下发：Redis key 为
`initData:{channel}:{acc}:{area}`，值是包含个人 `s_goldSum` 的完整
`huoDong` 数组，TTL 固定 7200 秒。相同渠道、账号和区服再次发送时会
覆盖旧邮件；服务端不保存历史、已读或领取状态。客户端仅通过
`getInitData` 获取邮件，Redis key 过期或被删除后自动回退到普通活动数据。

全体邮件使用 `initData:{channel}:global`，同样采用 2 小时 TTL 和覆盖语义，
对该渠道所有账号、区服生效。个人邮件和全体邮件都使用客户端邮箱协议
`s_goldSum`；两者同时存在时只下发 `beginAt` 较新的一封，保持参考项目
“后发覆盖前发”的单邮件语义，避免客户端同时处理两个同类型活动。

邮件在 Redis 的 2 小时有效期内会随每次 `getInitData` 请求重复下发，
由客户端根据活动 `md` 去重。服务端不在读取邮件时记录已投递状态，避免首次
响应丢失、初始化请求未被客户端处理等情况导致邮件被提前标记为已领取。

定时任务保存在 Redis 有序集合 `GLOBAL_MAIL_SCHEDULES` 中，服务器重启不会
丢失。到达计划时间后任务会自动转换为对应渠道的全体邮件。

邮件索引存储在有序集合 `MAIL_SET`，用于管理后台查询有效邮件。配置示例：

```json
{
  "redis": {
    "host": "127.0.0.1",
    "port": 6379,
    "password": "",
    "database": 0,
    "connect_timeout_ms": 3000
  }
}
```

Redis 在启动时不可连接、认证失败或数据库选择失败时，服务端会直接退出，
避免邮件请求在无持久化后端时静默成功。

可运行不依赖外部 Redis 的协议与邮件集成测试：

```bash
cmake --build build --config Release --target test_mail_redis
python scripts/test_mail_redis.py build/Release/test_mail_redis.exe
```

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

## 初始化数据增量同步

新版客户端调用 `getInitData` 时可在加密 JSON 中携带 `repairVer`：

- 版本一致：服务端仅返回 `{"code":0,"repairVer":当前版本}`。
- 版本不一致或未携带：返回完整初始化数据、邮件修复项及最新 `repairVer`。
- 客户端处理完整响应后应持久化顶层 `repairVer`，下次请求原样传回。

为兼容旧客户端，请求字段也接受 `repair_ver` 和 `repair`；未携带版本的旧客户端行为不变。

## 你需要提供

1. **客户端版本号 `ver`**（与 APK 一致，默认 478）
2. **服务器地址**（改客户端或 hosts）
3. 登录方式与活动 JSON（`huoDong` 等）按需扩展 `src/handlers.cpp`
