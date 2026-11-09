@echo off
rem Atalho para compilar/gravar o firmware ESP32-DIV com um clique.
rem Passa todos os argumentos para o build.ps1 (ex.: "build.cmd -CompileOnly").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
