@echo off
rem Put cso_bench.exe and this file next to your ISOs and double-click.
rem Results go to bench_results\ (summary.txt + small .csv.gz files). ISOs are only read.
rem Options: --sample N (1 MiB of every N MiB, default 8), --threads N, --no-zopfli, --zopfli15
cd /d "%~dp0"
"%~dp0cso_bench.exe" %* "%~dp0."
pause
