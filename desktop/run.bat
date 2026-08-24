@echo off
chcp 65001 >nul
title VoiceStick
cd /d %~dp0
python main.py
pause