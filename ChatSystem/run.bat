@echo off
chcp 65001 >nul
echo ========================================
echo   正在编译服务端和客户端...
echo ========================================

g++ -std=c++11 server.cpp -o server.exe -lws2_32 -pthread
if %errorlevel% neq 0 (
    echo 服务端编译失败！
    pause
    exit
)

g++ -std=c++11 client.cpp -o client.exe -lws2_32 -pthread
if %errorlevel% neq 0 (
    echo 客户端编译失败！
    pause
    exit
)

echo ========================================
echo   编译成功！
echo ========================================
echo   请打开两个命令行窗口：
echo ========================================
echo   1. 运行 server.exe 启动服务器
echo ----------------------------------------
echo   2. 运行 client.exe 启动客户端（可开多个）
echo ========================================
pause