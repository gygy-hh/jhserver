# 生产环境部署说明

面向当前线上机（默认 `root@39.107.52.206`，目录 `/opt/jh-server`）。  
本地仓库根目录：`D:\server`（WSL 下为 `/mnt/d/server`）。

---

## 1. 部署会动到什么

| 本地文件 | 线上路径 | `deploy.ps1` 是否上传 |
|----------|----------|----------------------|
| `build-linux/jh_server` | `/opt/jh-server/build/jh_server` | **是**（主产物） |
| `config.prod.json` | `/opt/jh-server/config.json` | 是 |
| `web/admin.html` | `/opt/jh-server/web/admin.html` | 是 |
| `scripts/init_db.sql` | `/opt/jh-server/scripts/init_db.sql` | 是 |
| `deploy/jh-server.service` | 安装到 systemd | 是 |
| `data/save_keys.json`（若存在） | `/opt/jh-server/data/save_keys.json` | 是 |
| `data/bl_force.json`（若存在） | `/opt/jh-server/data/bl_force.json` | 是 |
| **`data/doc.json`** | `/opt/jh-server/data/doc.json` | **否（禁止）** |

服务进程由 systemd 拉起：

```text
ExecStart=/opt/jh-server/build/jh_server --config /opt/jh-server/config.json
WorkingDirectory=/opt/jh-server
```

健康检查：`http://127.0.0.1:18080/health`（对外同端口）。

---

## 2. 两份 doc.json（必读）

线上同时存在两份用途不同的 `doc.json`，**不要混用、不要用本地残缺文件覆盖**：

| 路径 | 用途 | 正确更新方式 |
|------|------|----------------|
| `/opt/jh-server/data/updates/conf/doc.json` | 客户端远程拉取（管理后台上传后 XOR 保存） | 管理后台 **「上传 doc.json」** |
| `/opt/jh-server/data/doc.json` | 服务端论剑池 / 本地目录读取（明文 JSON） | 见下文「同步服务端 data/doc.json」 |

历史事故：

1. 用本地 `D:\server\data\doc.json`（约 3.5MB、缺大量 id）覆盖线上 → 客户端/论剑「找不到 id」。
2. 用 root 批量改写 `data/saves/*.json` 后属主变成 root → 上传存档 `.sav` 成功但 meta 写失败，客户端报 `save failed`。

**原则：**

- 日常发版：**不要** `scp` 本地 `data/doc.json`。
- 改游戏配置：走管理后台上传；需要论剑也用新表时，再把完整版同步到 `data/doc.json`。
- 改存档 meta / 清 CD 等脚本：写完后务必 `chown jh-server:jh-server`。

---

## 3. 环境准备

### 本机

- Windows + WSL（Ubuntu），能执行 `wsl -e bash ...`
- WSL 内：`g++`、MariaDB 客户端开发库（`libmariadb-dev` 等，与 `scripts/build-linux.sh` 一致）
- OpenSSH 客户端；到生产机免密登录已配好

### 生产机

- 已安装并运行：`jh-server` systemd 服务、MySQL/MariaDB、Redis
- 运行用户相关目录属主：`data/` 及下属文件应为 **`jh-server:jh-server`**
- 二进制：`/opt/jh-server/build/jh_server`（属主可为 `root:jh-server`，权限 `755`）

首次装机若还没有服务，参考 `scripts/setup-linux.sh` 与 `deploy/jh-server.service`，并在宝塔/面板中建好库后执行 `scripts/init_db.sql`。

---

## 4. 标准发版（推荐）

在 **PowerShell** 仓库根目录执行：

```powershell
# 完整：WSL 静态编译 + 上传 + 重启
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1

# 已编好二进制、只上传重启
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -SkipBuild
```

可选参数：

```powershell
.\scripts\deploy.ps1 -HostName 39.107.52.206 -User root -RemoteDir /opt/jh-server
```

脚本流程简述：

1. （默认）WSL 调用 `scripts/build-linux.sh`，产出 `build-linux/jh_server`（静态链接 ELF）
2. `systemctl stop jh-server`
3. 上传二进制到 `/tmp/jh_server`，再安装到 `/opt/jh-server/build/jh_server`
4. 上传 config / admin / service 等（**不含** `data/doc.json`）
5. `chown` 配置与 `data/`，`daemon-reload`，`enable` + `restart`
6. 打印 `systemctl status` 与 `/health`

编译也可单独跑：

```bash
# 在 WSL 内
bash /mnt/d/server/scripts/build-linux.sh
# 产物：/mnt/d/server/build-linux/jh_server
```

---

## 5. 只更新二进制（不碰 config / admin）

适合「只改了 `lunjian.cpp` / `handlers.cpp` 等逻辑、配置不变」：

```powershell
wsl -e bash /mnt/d/server/scripts/build-linux.sh
scp D:\server\build-linux\jh_server root@39.107.52.206:/tmp/jh_server
ssh root@39.107.52.206 @"
systemctl stop jh-server
install -m 755 -o root -g jh-server /tmp/jh_server /opt/jh-server/build/jh_server
rm -f /tmp/jh_server
# 发版前确认论剑用的 doc 没被盖成残缺版（完整版约 4MB+）
python3 -c "import os; s=os.path.getsize('/opt/jh-server/data/doc.json'); assert s>4000000, s"
systemctl start jh-server
sleep 1
systemctl is-active jh-server
curl -sS http://127.0.0.1:18080/health; echo
"@
```

---

## 6. 更新 doc.json（正确姿势）

### 6.1 客户端用的远程 doc（必须）

1. 浏览器打开：`http://<主机>:18080/admin`（Basic 认证，账号密码见 `config.prod.json` / 线上 `config.json`）
2. 找到 **「服务端 doc.json」** → 选择完整 `conf/doc.json` → **上传 doc.json**
3. 确认页面显示新的 `doc_version` / `doc_md5` / 大小（完整包大约 **4.0MB+**）
4. 文件落在：`/opt/jh-server/data/updates/conf/doc.json`（XOR 存储）  
   清单：`/opt/jh-server/data/updates/manifest.json`

### 6.2 服务端论剑用的 `data/doc.json`（需要时再同步）

管理后台上传**不会**自动改 `data/doc.json`。论剑池读的是这份明文。  
在确认 `updates/conf/doc.json` 已是完整新版后，在服务器上同步：

```bash
python3 <<'PY'
import hashlib, json, os, shutil, time
src = "/opt/jh-server/data/updates/conf/doc.json"
dst = "/opt/jh-server/data/doc.json"
bak = dst + ".bak-%d" % int(time.time())
raw = open(src, "rb").read()
# 与 update.cpp 中 doc_xor_transform 等价：逐字节按位取反
plain = bytes((~b) & 0xFF for b in raw)
assert plain[:1] in b"{[", plain[:20]
doc = json.loads(plain.decode("utf-8"))
shutil.copy2(dst, bak)
tmp = dst + ".tmp"
open(tmp, "wb").write(plain)
import pwd
pw = pwd.getpwnam("jh-server")
os.chown(tmp, pw.pw_uid, pw.pw_gid)
os.chmod(tmp, 0o640)
os.replace(tmp, dst)
print("ok", len(plain), "person", len(doc.get("person", {})), "backup", bak)
PY
systemctl restart jh-server
```

自检：

```bash
wc -c /opt/jh-server/data/doc.json /opt/jh-server/data/updates/conf/doc.json
# data/doc.json 应约为 4MB+；若只有约 3.5MB，多半又被残缺文件覆盖
```

---

## 7. 发版后检查清单

```bash
# 服务
systemctl is-active jh-server
curl -sS http://127.0.0.1:18080/health

# 二进制与启动时间
ls -la --time-style=full-iso /opt/jh-server/build/jh_server
systemctl show jh-server -p ActiveEnterTimestamp --no-pager

# doc 是否完整（论剑）
wc -c /opt/jh-server/data/doc.json
# 建议 > 4000000

# 存档目录属主（上传失败 save failed 时先查这个）
find /opt/jh-server/data/saves -name '*.json' -printf '%u\n' | sort | uniq -c
# 应几乎全是 jh-server；若大量 root，执行：
# chown -R jh-server:jh-server /opt/jh-server/data/saves

# 最近错误
journalctl -u jh-server --since '10 min ago' --no-pager | tail -50
```

管理后台可再确认：账号/存档列表、远程 doc 版本号。

---

## 8. 常见问题

### 上传存档提示失败，但云端其实有了

- 现象：`.sav` 已更新，`.json` meta 未更新，客户端显示 `(save failed)`。
- 原因：`*.json` 属主是 `root`，进程用户 `jh-server` 写不了。
- 处理：`chown -R jh-server:jh-server /opt/jh-server/data/saves`

### 找不到人物/武功/道具 id

- 先看 `data/doc.json` 是否又被缩成约 3.5MB。
- 用第 6 节从 `updates/conf/doc.json` 还原，再 `systemctl restart jh-server`。

### 论剑人机名变回「人机01」或逻辑不对

- 确认部署的是**当前仓库编出的** `build-linux/jh_server`，不是旧备份。
- 重启后请求会按代码刷新展示名；必要时再查 MySQL `lunjian_fighter` 本周 `is_npc=1` 行。

### 部署脚本改了 config，线上连库失败

- `deploy.ps1` 会覆盖 `config.json`。发版前确认本地 `config.prod.json` 与线上一致（主机、库名、密码）。
- 不要把含真实密码的配置提交进公开仓库。

### SSH / WSL 编译失败

- SSH：检查本机密钥是否在服务器 `authorized_keys`。
- WSL：确认 `g++`、`-lmariadb` 等依赖齐全；完整命令以 `scripts/build-linux.sh` 为准。

---

## 9. 回滚

1. **二进制**：保留上一次可用的 `jh_server` 备份，停服务后覆盖 `/opt/jh-server/build/jh_server` 再启动。  
2. **doc**：若刚同步坏了，用同步脚本自动生成的 `data/doc.json.bak-*`，或再次从 `updates/conf/doc.json` 解密还原。  
3. **配置**：从已知好的 `config.json` 备份恢复（不要随意用仓库里过期的 prod 配置覆盖）。

建议发版前在服务器执行：

```bash
cp -a /opt/jh-server/build/jh_server /opt/jh-server/build/jh_server.bak-$(date +%Y%m%d-%H%M%S)
```

---

## 10. 相关路径速查

```text
/opt/jh-server/
  build/jh_server          # 主程序
  config.json              # 运行配置（来自本地 config.prod.json）
  web/admin.html           # 管理后台
  data/
    doc.json               # 论剑等服务端目录（明文，勿用残缺本地文件覆盖）
    saves/                 # 云存档 *.sav + *.json meta
    updates/
      manifest.json
      conf/doc.json        # 客户端远程 doc（管理后台上传）
      apk/ json/ ...
  deploy/jh-server.service
```

本地常用：

```text
D:\server\
  scripts\deploy.ps1       # 一键发版
  scripts\build-linux.sh   # WSL 静态编译
  build-linux\jh_server    # 编译产物
  config.prod.json         # 将覆盖线上 config.json
  data\doc.json            # 本地副本，可能过期/残缺 —— 禁止直接当线上源
  DEPLOY.md                # 本文档
```

---

## 11. 一句话规范

**发版用 `deploy.ps1`（或不碰 doc 的二进制热更）；改表用管理后台上传；论剑要新表再解密同步 `data/doc.json`；动 saves 后保持 `jh-server` 属主。**
