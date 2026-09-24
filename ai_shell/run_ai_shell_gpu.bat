@echo off
setlocal

rem === GPU acceleration ===
set LLAMA_NO_MMAP=1
set LLAMA_VMM_USE=1
set LLAMA_GPU_LAYERS=999
set LLAMA_KV_OFFLOAD=1

echo [GPU MODE]
echo LLAMA_NO_MMAP=%LLAMA_NO_MMAP%
echo LLAMA_VMM_USE=%LLAMA_VMM_USE%
echo LLAMA_GPU_LAYERS=%LLAMA_GPU_LAYERS%
echo LLAMA_KV_OFFLOAD=%LLAMA_KV_OFFLOAD%

"%~dp0ai_shell.exe"

endlocal

