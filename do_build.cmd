@echo off
call "D:\DevProgramsSDK\ESP-IDF\export.bat"
set IDF_COMPONENT_MANAGER_CACHE_PATH=C:\idfcache
set PYTHONUTF8=1
cd /d N:\
echo ===== BUILD START %DATE% %TIME% ===== >> N:\build_v8.log
python "%IDF_PATH%\tools\idf.py" build >> N:\build_v8.log 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL% >> N:\build_v8.log
echo ===== BUILD END %DATE% %TIME% ===== >> N:\build_v8.log
