@echo off
chcp 65001 >nul
cd /d "%~dp0"
start "" "http://127.0.0.1:8000"
python app.py
if errorlevel 1 (
    echo.
    echo Python 启动失败，请确认已安装 Python 并加入 PATH。
    pause
)
