# GoGoSpot

Windows 本地运行的**超轻量级**待办清单管理器，体积不足 1 MB。
不联网、不注册、不装运行库。

## 运行 GoGoSpot

将 `GoGoSpot.exe` 与 `web` 文件夹置于同一目录即完成部署。

双击 `GoGoSpot.exe` 启动，托盘图标右键退出。需要 64 位 Windows 7 或更高。

## 悬浮清单

<img src="docs/widget-card.png" alt="悬浮清单展开后" width="360">

桌面上的任务提示器，有**始终置顶**与**静默于桌面底层**两种状态。
常态下尽量保持低调，任务临近截止时才亮起提示。

### 正在进行

每个任务均可设置预计完成用时。

点击任务右侧的“正在进行”，悬浮清单即以进度条显示已进行的时间，
可用于考试一类的限时场景。

- 预计最晚开始时间点 = 截止时间点 − 预计完成用时
- 起始时间点 = min(点击“正在进行”按钮的时刻, 预计最晚开始时间点)
- 用时占比 = (当前时间点 − 起始时间点) ÷ 预计完成用时

可同时进行多项任务，勾选完成会自动结束计时。用时占比达到 100% 时发送系统通知，
窄条上的倒计时继续向负值累计。

未设定预计用时的不计算占比，以紫色呼吸环表示进行中。

### 添加与整理

底栏“添加任务”展开面板：输入框、截止（今天 / 明天 / 后天 / 自选 / 不设）、
预估（10 分至 8 小时）、从这里开始。自选截止展开月历，可选到分钟；
回车提交，Esc 取消。

**从这里开始**用于记录与任务相关的文件或网址，一条任务可挂多个，点击任务时一并打开。

右键任务可“置顶”“推迟一天”或“删除”。

## 控制台

浏览器中的管理界面，用于批量修改、搜索与备份恢复；
提供两套主题（[浅色](docs/screenshot-light.png) / [深色](docs/screenshot-dark.png)），
跟随系统应用模式。

**重复任务**在截止面板中选择：每天 / 每星期 / 每两星期 / 每月，且必须有截止时间。
完成或逾期时复制一条到下一周期，原任务的重复标记清空；
逾期较久的只补一条，下一周期直接推至当前时间之后。

## 设置与数据

三个开关位于控制台右上角的设置面板。开机自动启动写入
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\GoGoSpot`，
值为当前 `GoGoSpot.exe` 的完整路径，每次启动重写一遍，移动程序位置后仍能对应。

`data/tasks.json` 存任务，`data/backups/` 为每次覆盖前的快照（上限 40 份），
`data/settings.json` 存设置。写入采用临时文件加原子替换，
进程被终止也不会留下不完整的文件。与功能相关的字段：`links`（关联材料）、
`startedAt`（进行中，毫秒）、`pinned`（置顶）、
`repeat`（daily / weekly / biweekly / monthly）。
后台每 20 秒扫描一次逾期任务，每 5 秒检查一次计时是否结束。

## 目录与编译

```
src/main.cpp        程序外壳与托盘
src/server.cpp      后台编译入口，聚合 src/server/*.inc
src/server/         按职责命名的后台片段：文件系统、JSON、设置、HTTP、路由与任务
src/widget.cpp      悬浮清单编译入口，聚合 src/widget/*.inc
src/widget/         按职责命名的悬浮清单片段：布局、状态、绘制、命中检测与交互
web/                控制台，磁盘直读，改完不用重编
icons/              图标源文件
docs/               README 用的截图
tests/              发布前冒烟脚本
```

`server.cpp` 与 `widget.cpp` 按功能顺序包含对应 `.inc` 片段；片段保持在原编译单元中，以保留 `static` 符号的内部链接。

MinGW-w64（g++ 8.1 以上）或任何支持 C++17 的 Windows 编译器：

```
mkdir build
windres -I src -I icons -i src/app.rc -o build/app.res.o -O coff
g++ -std=c++17 -O2 -static -mwindows -o GoGoSpot.exe ^
    src/main.cpp src/server.cpp src/widget.cpp build/app.res.o ^
    -lws2_32 -lwinhttp -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -limm32 -lcomdlg32
```

`-static` 静态链接运行时，`-mwindows` 去掉命令行窗口。编译前需退出正在运行的程序。

## HTTP 接口

后台托管 `web/`、读写 JSON、管理备份，本身不解析任务字段。
仅 `/api/toggle`、`/api/start`、`/api/pin`、`/api/snooze`、`/api/remove`、`/api/add`
会修改 JSON——它们需要保留未知字段、只改其中一两个，因此不能整份覆盖。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health`、`/api/info` | 存活探测 / 数据路径、版本、运行时长 |
| GET / POST | `/api/settings` | 读写三个开关，body 形如 `{"topmost":true}` |
| GET / PUT | `/api/tasks` | 读任务；覆盖写入（写前备份），body 必须是带 `tasks` 数组的对象 |
| GET | `/api/widget` | 悬浮清单用，纯文本，一行一条未完成任务 |
| POST | `/api/toggle?id=`、`/api/start?id=&on=` | 翻转完成 / 标记或取消正在进行 |
| POST | `/api/pin?id=&on=`、`/api/topmost?on=` | 任务置顶 / 悬浮清单置顶 |
| POST | `/api/add?title=&due=&estimate=&links=` | 加一条任务，后三个可省，links 用竖线分隔 |
| POST | `/api/open?url=`、`/api/snooze?id=`、`/api/remove?id=` | 打开链接 / 推迟一天 / 删除 |
| GET | `/api/export`、`/api/backups` | 下载当前数据 / 列出历史备份 |
| POST | `/api/backup`、`/api/restore`、`/api/deletebackup` | 备份 / 恢复 / 删备份 |

## 限制

- 仅支持 Windows。基于 Win32 与 GDI+，跨平台需重写。
- 无同步机制，多台机器共用需自行拷贝 `data`。
- 截止时间到达时不会主动提醒，只在悬浮清单上标红标黄。
- 控制台中无法获取文件的绝对路径，只能手动填写、从剪贴板粘贴，或从资源管理器拖入。
- 仅监听 `127.0.0.1`；请求体只接受 `Content-Length`（不支持 `chunked`），单次上限 16 MB；
  写接口校验 `Origin` / `Sec-Fetch-Site` / `Host`，其他网页无法借浏览器修改任务。

## 许可

MIT，见 [LICENSE](LICENSE)。
