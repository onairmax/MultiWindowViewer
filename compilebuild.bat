@echo off
echo Compiling resources...
windres resource.rc -O coff -o resource.o
if %errorlevel% neq 0 (
    echo Resource compilation failed!
    goto :eof
)

echo Compiling and linking main application...
g++ -std=c++11 -Wall -Wextra -DUNICODE -D_UNICODE winview.cpp resource.o -o MultiWindowViewer.exe -mwindows -luser32 -lgdi32 -lcomctl32 -ldwmapi -luxtheme
if %errorlevel% neq 0 (
    echo Application compilation failed!
    goto :eof
)

echo Build successful!