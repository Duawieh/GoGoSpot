// ---- 常量 ----

static const char*   APP_TAG     = "GoGoSpot";
static const size_t  MAX_BODY    = 16u * 1024 * 1024;   // 单次请求体上限 16MB
static const int     MAX_BACKUPS = 40;                  // 备份保留份数，超出后从最旧的开始删

