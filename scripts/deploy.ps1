# Build in WSL, upload to the production host, restart jh-server.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
#        powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -SkipBuild
param(
    [string]$HostName = "39.107.52.206",
    [string]$User = "root",
    [string]$RemoteDir = "/opt/jh-server",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

if (-not $SkipBuild) {
    Write-Host "WSL static build..."
    wsl -e bash /mnt/d/server/scripts/build-linux.sh
    if ($LASTEXITCODE -ne 0) { throw "WSL build failed" }
}

$exe = Join-Path $root "build-linux\jh_server"
if (-not (Test-Path $exe)) { throw "binary not found: $exe" }

$remote = "${User}@${HostName}"
Write-Host "Upload to ${remote}:${RemoteDir}"

$prepCmd = "sudo systemctl stop jh-server; sudo install -d -o root -g jh-server -m 755 ${RemoteDir}/build ${RemoteDir}/web ${RemoteDir}/scripts ${RemoteDir}/deploy; sudo install -d -o jh-server -g jh-server -m 750 ${RemoteDir}/data"
ssh $remote $prepCmd
if ($LASTEXITCODE -ne 0) { throw "SSH failed. Check key in ~/.ssh/authorized_keys on the server." }

scp $exe "${remote}:/tmp/jh_server"
if ($LASTEXITCODE -ne 0) { throw "scp binary failed" }
ssh $remote "sudo mv /tmp/jh_server ${RemoteDir}/build/jh_server; sudo chmod 755 ${RemoteDir}/build/jh_server; sudo chown root:jh-server ${RemoteDir}/build/jh_server"
if ($LASTEXITCODE -ne 0) { throw "install binary failed" }

scp (Join-Path $root "config.prod.json") "${remote}:${RemoteDir}/config.json"
scp (Join-Path $root "web\admin.html") "${remote}:${RemoteDir}/web/admin.html"
scp (Join-Path $root "scripts\init_db.sql") "${remote}:${RemoteDir}/scripts/init_db.sql"
scp (Join-Path $root "deploy\jh-server.service") "${remote}:${RemoteDir}/deploy/jh-server.service"
if (Test-Path (Join-Path $root "data\save_keys.json")) {
    scp (Join-Path $root "data\save_keys.json") "${remote}:${RemoteDir}/data/save_keys.json"
}

Write-Host "Restart remote service..."
$remoteCmd = "sudo chown root:jh-server ${RemoteDir}/config.json ${RemoteDir}/web/admin.html; sudo chmod 640 ${RemoteDir}/config.json; sudo chmod 644 ${RemoteDir}/web/admin.html; sudo chown -R jh-server:jh-server ${RemoteDir}/data; sudo cp ${RemoteDir}/deploy/jh-server.service /etc/systemd/system/jh-server.service; sudo systemctl daemon-reload; sudo systemctl enable jh-server; sudo systemctl restart jh-server; sleep 1; sudo systemctl --no-pager --full status jh-server; curl -sS http://127.0.0.1:18080/health; echo"
ssh $remote $remoteCmd
if ($LASTEXITCODE -ne 0) { throw "remote restart failed" }

Write-Host ""
Write-Host "Done."
Write-Host "  health: http://${HostName}:18080/health"
Write-Host "  admin:  http://${HostName}:18080/admin"
Write-Host "  g_url:  http://${HostName}:18080/"
Write-Host "If MySQL error, create db in BT panel then run init_db.sql on the server."
