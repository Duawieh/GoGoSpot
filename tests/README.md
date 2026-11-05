# 发布前冒烟

先把隔离副本准备好。最省事的是留在项目根目录另起一个：

```
GoGoSpot.exe --port 8770 --data build\test\smoke\data\tasks.json --no-browser
```

`--data` 收的是 tasks.json 的文件路径，不是目录；settings.json 和 backups 会跟着它走。
**不要用 8765**，那是正式实例的端口。启动后先看 `/api/info` 的 `dataFile` 确实落在
`build\test\` 下，再跑脚本。

```
# 接口：正常路径、畸形输入、并发、备份、重复派生
python tests/verify-fixes.py     # 逐条对应修复报告里的 P0 / P1
python tests/api_test.py
python tests/api_test2.py
python tests/boundary_test.py    # 边界值、锚点、主题三档，写死 8770

# 控制台（需要 playwright，装在托管 node 工作区）
set NODE_PATH=%USERPROFILE%\.workbuddy-ai\binaries\node\workspace\node_modules
node tests/console1.js
node tests/console2.js
node tests/console3.js
node tests/est-unit.js          # 预估单位切换
node tests/shots-readme.js      # 重新生成 docs/ 里的 README 截图
```

端口默认 8770：四个 Python 脚本读 `GOGOSPOT_PORT`，控制台脚本读 `GOGOSPOT_BASE`
（默认 `http://127.0.0.1:8770/`）。要换端口改这两个变量，别改回 8765。
`boundary_test.py` 例外：端口写死 8770，且要求 `dataFile` 含 `build/test/boundary`，
启动时用 `--data build\test\boundary\data\tasks.json`。

`est-unit.js` 会先 PUT 一份全新的任务列表，**只能打隔离副本**。

`verify-fixes.py` 里有两段各等 22 秒的逾期扫描，整套跑完约 1 分钟。控制台脚本会把截图写进
`tests/shots/`，那个目录和 `tests/*.json` 都不入库。

## 界面工具

三个小工具分别单独编译，产物放临时目录即可，不要提交：

```
# 命中区自检：两个按钮的圆心与 hitPin / hitProgress 是否一致，有失败返回非零
g++ -std=c++17 -O2 -static -municode tests/hit_test.cpp -o %TEMP%\hit_test.exe ^
    -lwinhttp -lws2_32 -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -limm32 -lcomdlg32

# 截图：按窗口标题抓悬浮清单，可选先投一条消息再等几毫秒
g++ -std=c++17 -O2 -static -municode tests/cap.cpp -o %TEMP%\cap.exe ^
    -lgdiplus -lgdi32 -luser32

%TEMP%\cap.exe "GoGoSpot 悬浮清单" shot.png 0 300

# 窗口探针：按进程号列窗口、读矩形、投点击、发关闭
g++ -std=c++17 -O2 -static -municode tests/widget_probe.cpp -o %TEMP%\widget_probe.exe ^
    -luser32
```

`hit_test.cpp` 包含 `src/widget.cpp`，自带几个 `app*` 空实现补上外部接口依赖；
`cap.cpp` 和 `widget_probe.cpp` 不依赖项目源码。

`widget_probe.exe <pid> windows|rect|menu` 用来确认悬浮清单在哪、菜单有没有弹出来；
`click <x> <y>` / `rclick <x> <y>` 按客户区坐标投消息，`close` / `quit` 分别关悬浮清单和托盘。
