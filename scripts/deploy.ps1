# Build in WSL, upload to the Singapore host, restart jh-server.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1
param(
    [string]$HostName = "8.219.65.240",
    [string]$User = "admin",
    [string]$RemoteDir = "/opt/jh-server"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

Write-Host "WSL static build..."
wsl -e bash /mnt/d/server/scripts/build-linux.sh
if ($LASTEXITCODE -ne 0) { throw "WSL build failed" }

$exe = Join-Path $root "build-linux\jh_server"
if (-not (Test-Path $exe)) { throw "binary not found: $exe" }

$remote = "${User}@${HostName}"
Write-Host "Upload to ${remote}:${RemoteDir}"

$mkdirCmd = "sudo mkdir -p ${RemoteDir}/build ${RemoteDir}/web ${RemoteDir}/data ${RemoteDir}/scripts ${RemoteDir}/deploy; sudo chown -R ${User}:${User} ${RemoteDir}"
ssh $remote $mkdirCmd
if ($LASTEXITCODE -ne 0) { throw "SSH failed. Check key in ~/.ssh/authorized_keys on the server." }

scp $exe "${remote}:${RemoteDir}/build/jh_server"
if ($LASTEXITCODE -ne 0) { throw "scp binary failed" }
scp (Join-Path $root "config.prod.json") "${remote}:${RemoteDir}/config.json"
scp (Join-Path $root "web\admin.html") "${remote}:${RemoteDir}/web/admin.html"
scp (Join-Path $root "scripts\init_db.sql") "${remote}:${RemoteDir}/scripts/init_db.sql"
scp (Join-Path $root "deploy\jh-server.service") "${remote}:${RemoteDir}/deploy/jh-server.service"
if (Test-Path (Join-Path $root "data\save_keys.json")) {
    scp (Join-Path $root "data\save_keys.json") "${remote}:${RemoteDir}/data/save_keys.json"
}
if (Test-Path (Join-Path $root "data\updates")) {
    scp -r (Join-Path $root "data\updates") "${remote}:${RemoteDir}/data/"
}

Write-Host "Restart remote service..."
$remoteCmd = "chmod +x ${RemoteDir}/build/jh_server; sudo cp ${RemoteDir}/deploy/jh-server.service /etc/systemd/system/jh-server.service; sudo systemctl daemon-reload; sudo systemctl enable jh-server; sudo systemctl restart jh-server; sleep 1; sudo systemctl --no-pager --full status jh-server; curl -sS http://127.0.0.1:18080/health; echo"
ssh $remote $remoteCmd
if ($LASTEXITCODE -ne 0) { throw "remote restart failed" }

Write-Host ""
Write-Host "Done."
Write-Host "  health: http://${HostName}:18080/health"
Write-Host "  admin:  http://gaoy.fun:18080/admin"
Write-Host "  g_url:  http://gaoy.fun:18080/"
Write-Host "If MySQL error, create db in BT panel then run init_db.sql on the server."
