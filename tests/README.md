# 发布前冒烟

先把隔离副本准备好：把 `GoGoSpot.exe` 和 `web/` 复制到一个临时目录，准备一份空的 `data/`，
用 `--port 8765 --no-browser` 起起来。脚本只打 `127.0.0.1:8765`，不碰真实 `data/`。

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
node tests/est-unit.js          # 预估单位切换
```

端口不写死：三个 Python 脚本读 `GOGOSPOT_PORT`（默认 8765），控制台脚本读 `GOGOSPOT_BASE`
（默认 `http://127.0.0.1:8765/`）。正式实例开着的时候，把这两个变量指到隔离副本再跑。

`est-unit.js` 会先 PUT 一份全新的任务列表，**只能打隔离副本**，默认端口是 8770。

`verify-fixes.py` 里有两段各等 22 秒的逾期扫描，整套跑完约 1 分钟。控制台脚本会把截图写进
`tests/shots/`，那个目录和 `tests/*.json` 都不入库。

## 界面工具

两个小工具分别单独编译，产物放临时目录即可，不要提交：

```
# 命中区自检：两个按钮的圆心与 hitPin / hitProgress 是否一致，有失败返回非零
g++ -std=c++17 -O2 -static -municode tests/hit_test.cpp -o %TEMP%\hit_test.exe ^
    -lwinhttp -lws2_32 -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -limm32 -lcomdlg32

# 截图：按窗口标题抓悬浮清单，可选先投一条消息再等几毫秒
g++ -std=c++17 -O2 -static -municode tests/cap.cpp -o %TEMP%\cap.exe ^
    -lgdiplus -lgdi32 -luser32

%TEMP%\cap.exe "GoGoSpot 悬浮清单" shot.png 0 300
```

`hit_test.cpp` 包含 `src/widget.cpp`，自带几个 `app*` 空实现补上外部接口依赖；
`cap.cpp` 不依赖项目源码。
