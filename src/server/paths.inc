// ---- 全局路径 ----

static wstring gRoot;        // exe 所在目录
static wstring gWebDir;      // <root>\web
static wstring gDataDir;     // <root>\data
static wstring gDataFile;    // <root>\data\tasks.json
static wstring gBackupDir;   // <root>\data\backups
static wstring gSettingsFile;   // <root>\data\settings.json
static DWORD   gStartTick = 0;
static SOCKET  g_listener = INVALID_SOCKET;

static std::mutex gDataMutex;   // 保护数据文件的所有读写

// 数据目录对外暴露一份：悬浮清单读 .port 要跟着 --data 走，
// 各读各的 g_root\data 时，便携副本会连上正式实例
const wchar_t* serverDataDir() { return gDataDir.c_str(); }

