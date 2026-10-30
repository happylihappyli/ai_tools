@echo off
setlocal
set "AC_DIR=%~dp0"
python "%AC_DIR%ac" %*
endlocal