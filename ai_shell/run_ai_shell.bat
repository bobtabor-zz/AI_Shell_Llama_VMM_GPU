@echo off
setlocal

rem === Enable VMM builder mode ===
set LLAMA_NO_MMAP=1
set LLAMA_VMM_BUILD=1

rem === Optional: show what mode we're in ===
echo LLAMA_NO_MMAP=%LLAMA_NO_MMAP%
echo LLAMA_VMM_BUILD=%LLAMA_VMM_BUILD%

rem === Launch ai_shell.exe from the same folder as this .bat ===
"%~dp0ai_shell.exe"

endlocal
