@echo off
set "GRAPH_DIR=E:\code\UE-AI-CLI"
set "UNDERSTAND_ACCESS_TOKEN=ecc6437d272a4edcaf8ea6b1f888b083"
cd /d "C:\Users\aimidi\.understand-anything\repo\understand-anything-plugin\packages\dashboard"
"D:\Program Files\nodejs\npx.cmd" vite --host 127.0.0.1 --port 5173 --strictPort 1>"E:\code\UE-AI-CLI\.ua\dashboard.log" 2>&1
