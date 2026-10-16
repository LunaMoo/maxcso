@echo off
rem Put img_bench.exe, iPBP.dll (built from iPBP's feature/lzrc-max branch) and this file next to
rem your ISOs and double-click. Tests only EBOOT.PBP with LZRC max: whole ISOs, timed, converted
rem back and checked byte for byte. Results are added to bench_lib\.
cd /d "%~dp0"
"%~dp0img_bench.exe" --no-cso --no-chd --pbp-modes max %* "%~dp0."
pause
