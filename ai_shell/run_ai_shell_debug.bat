@echo off
setlocal

rem === Debug-friendly environment ===
set LLAMA_NO_MMAP=1
set LLAMA_VMM_BUILD=1
set LLAMA_DEBUG=1
set LLAMA_LOG_LEVEL=debug

echo [DEBUG MODE]
echo LLAMA_NO_MMAP=%LLAMA_NO_MMAP%
echo LLAMA_VMM_BUILD=%LLAMA_VMM_BUILD%
echo LLAMA_DEBUG=%LLAMA_DEBUG%
echo LLAMA_LOG_LEVEL=%LLAMA_LOG_LEVEL%

"%~dp0ai_shell.exe"

endlocal
