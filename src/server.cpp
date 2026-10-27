// GoGoSpot 后台：托管 web/ 静态文件，用 REST 接口读写 data/tasks.json，
// 覆盖写入前自动备份（最多 40 份）。纯 Winsock2 实现，不依赖第三方库。

#define WIN32_LEAN_AND_MEAN
// 使用 IFileOpenDialog 需要启用 Vista 及以上版本的 Windows SDK 定义
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <objbase.h>

#include "app.h"

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <utility>

using std::string;
using std::wstring;


#include "server/constants.inc"
#include "server/paths.inc"
#include "server/encoding.inc"
#include "server/time.inc"
#include "server/filesystem.inc"
#include "server/json_validation.inc"
#include "server/json.inc"
#include "server/settings.inc"
#include "server/http.inc"
#include "server/routes.inc"
#include "server/repeating_tasks.inc"
#include "server/client.inc"
#include "server/startup_helpers.inc"
#include "server/lifecycle.inc"
