<#
.SYNOPSIS
  cpp-daemon 托盘「退出」真实右键复核脚本。
.DESCRIPTION
  启动 build\Release\cpp-daemon.exe，提示你手动右键托盘图标 -> 点「退出」，
  然后自动轮询进程与 9801 端口是否释放（每 0.5s，最长 60s）。
  用后自动清理：若 60s 内未退出，则 Stop-Process 兜底，保证不留残留进程。
  退出耗时 = 从脚本提示完成到进程消失的秒数。
.PARAMETER ExeDir
  cpp-daemon 所在目录，默认脚本上一级的 build\Release。
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tray_quit_verify.ps1
#>
param(
  [string]$ExeDir = (Join-Path $PSScriptRoot "build\Release")
)

$ErrorActionPreference = 'Continue'
$exe = Join-Path $ExeDir "cpp-daemon.exe"
if (-not (Test-Path $exe)) { Write-Host "找不到 $exe" -ForegroundColor Red; exit 1 }

# 清掉可能残留的旧实例
Stop-Process -Name cpp-daemon -Force -ErrorAction SilentlyContinue
Start-Sleep 2

Write-Host "启动 cpp-daemon ..." -ForegroundColor Cyan
$p = Start-Process -FilePath $exe -PassThru
Write-Host "已启动 PID=$($p.Id)，等待 75s 让 iot_report 真实发布一次 ..." -ForegroundColor Cyan
Start-Sleep -Seconds 75

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Yellow
Write-Host "  请现在：右键托盘区 cpp-daemon 图标 -> 点菜单「退出」" -ForegroundColor Yellow
Write-Host "  （若图标在隐藏区，先点托盘箭头展开）" -ForegroundColor Yellow
Write-Host "==============================================================" -ForegroundColor Yellow
Write-Host "开始轮询（最长 60s）..." -ForegroundColor Cyan

$t0 = Get-Date
$gone = $false
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep -Milliseconds 500
  if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { $gone = $true; break }
}
$dt = (Get-Date) - $t0

if ($gone) {
  $ln = Get-NetTCPConnection -LocalPort 9801 -State Listen -ErrorAction SilentlyContinue
  Write-Host ("退出成功，耗时 {0}s" -f [int]$dt.TotalSeconds) -ForegroundColor Green
  if ($ln) { Write-Host "警告：9801 仍在监听" -ForegroundColor Red }
  else     { Write-Host "9801 端口已释放" -ForegroundColor Green }
} else {
  Write-Host "超时 60s 进程仍未退出（挂起），执行兜底 Stop-Process" -ForegroundColor Red
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}

Start-Sleep 1
$rp = Get-Process cpp-daemon -ErrorAction SilentlyContinue
if ($rp) { Write-Host "仍有残留进程 PID=$($rp.Id)" -ForegroundColor Red }
else     { Write-Host "无残留进程" -ForegroundColor Green }
