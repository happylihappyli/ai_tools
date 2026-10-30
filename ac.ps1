# ac.ps1 — Windows PowerShell 包装器
# 用法: . E:\GitHub3\cpp\ai_tools\ac.ps1   (加载到当前 shell)
#      然后: ac tts "你好"
#
# 或在 PowerShell profile ($PROFILE) 里加一行:
#   . 'E:\GitHub3\cpp\ai_tools\ac.ps1'
#
$global:AC_HOME = 'E:\GitHub3\cpp\ai_tools'
function ac {
    & python "$AC_HOME\ac" @args
}

Set-Alias -Name ab -Value ac -Force -Scope Global
Set-Alias -Name tts -Value ac -Force -Scope Global 2>$null   # 不强制覆盖

Write-Host "ac loaded from $AC_HOME"