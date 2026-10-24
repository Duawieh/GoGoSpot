# 发布前冒烟

先把隔离副本准备好：把 `GoGoSpot.exe` 和 `web/` 复制到一个临时目录（例如 `build/test/run/`），
准备一份空的 `data/`，用 `--port 8765 --no-browser` 起起来。脚本只打 `127.0.0.1:8765`，
不碰真实 `data/`。

```
# 接口：正常路径、畸形输入、并发、备份、重复派生
python tests/verify-fixes.py     # 逐条对应修复报告里的 P0 / P1
python tests/api_test.py
python tests/api_test2.py

# 控制台（需要 playwright，装在托管 node 工作区）
set NODE_PATH=%USERPROFILE%\.workbuddy-ai\binaries\node\workspace\node_modules
node tests/console1.js
node tests/console2.js
node tests/console3.js
```

`verify-fixes.py` 里有两段各等 22 秒的逾期扫描，整套跑完约 1 分钟。控制台脚本会把截图写进
`tests/shots/`，那个目录和 `tests/*.json` 都不入库。
