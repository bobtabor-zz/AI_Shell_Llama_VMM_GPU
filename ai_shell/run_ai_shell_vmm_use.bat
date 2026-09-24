@echo off
setlocal

rem === Use prebuilt VMM ===
set LLAMA_NO_MMAP=1
set LLAMA_VMM_USE=1

echo [VMM USE MODE]
echo LLAMA_NO_MMAP=%LLAMA_NO_MMAP%
echo LLAMA_VMM_USE=%LLAMA_VMM_USE%

"%~dp0ai_shell.exe"

endlocal
