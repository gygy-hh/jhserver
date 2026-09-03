# 启动 JH Game Server
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root "build\Release\jh_server.exe"

if (-not (Test-Path $exe)) {
    Write-Host "未找到 $exe，请先编译：" -ForegroundColor Yellow
    Write-Host "  cd $root"
    Write-Host "  cmake --build build --config Release"
    exit 1
}

Unblock-File -Path $exe -ErrorAction SilentlyContinue
Set-Location $root

try {
    & $exe
} catch {
    Write-Host ""
    Write-Host "无法运行 jh_server.exe：Windows 应用控制策略拦截了未签名的本地程序。" -ForegroundColor Red
    Write-Host ""
    Write-Host "请任选一种方式：" -ForegroundColor Yellow
    Write-Host "  1) 关闭「智能应用控制」："
    Write-Host "     设置 -> 隐私和安全性 -> Windows 安全中心 -> 应用和浏览器控制"
    Write-Host "     -> 智能应用控制设置 -> 关闭（关闭后通常无法再次轻易开启）"
    Write-Host ""
    Write-Host "  2) 用 Visual Studio 调试运行（有时可绕过限制）："
    Write-Host "     打开 build\jh_game_server.slnx -> 设为启动项目 -> 按 Ctrl+F5"
    Write-Host ""
    Write-Host "  3) 公司/学校电脑：联系管理员为 jh_server.exe 添加白名单"
    exit 1
}
