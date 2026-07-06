/*
 * server.c v8 — 参数校验 + 转义处理 + 最终整理
 * 新增：validate_pipename() / errors 命令
 *       转义字符处理 (\\n \\t 等)
 *       log_error() / log_info() 时间戳
 * 用法: server.exe [管道名]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>

#include "vector_clock.h"
#include "indexer.h"

#define MAX_BUF   8192
#define PIPE_BUF  4096

static InvertedIndex g_idx;
static CausalBuffer  g_buf;
static volatile LONG g_running = 1;
static int g_error_count = 0;

/* ========== 时间戳日志 ========== */

static void log_info(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    printf("[%02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    vprintf(fmt, args); printf("\n"); fflush(stdout);
    va_end(args);
}

static void log_error(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    printf("[ERR %02d:%02d:%02d] ", tm->tm_hour, tm->tm_min, tm->tm_sec);
    vprintf(fmt, args); printf("\n"); fflush(stdout);
    g_error_count++;
    va_end(args);
}

/* ========== Ctrl+C ========== */

static BOOL WINAPI signal_handler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT) {
        log_info("收到关闭信号");
        InterlockedExchange(&g_running, 0);
        return TRUE;
    }
    return FALSE;
}

/* ========== 参数校验 ========== */

static int validate_pipename(const char *name) {
    if (strncmp(name, "\\\\.\\pipe\\", 9) != 0) {
        log_error("管道名格式错误");
        return 0;
    }
    if (strlen(name) > 256) {
        log_error("管道名过长");
        return 0;
    }
    return 1;
}

/* ========== JSON 解析 ========== */

static int json_str(const char *s, const char *key, char *out, int n) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    s = strstr(s, pat);
    if (!s) return 0;
    s += strlen(pat);
    int i = 0;
    while (*s && *s != '"' && i < n - 1) {
        /* v8: 处理转义字符 */
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                default:  out[i++] = *s;   break;
            }
        } else {
            out[i++] = *s;
        }
        s++;
    }
    out[i] = 0;
    return i;
}

static int json_vc(const char *s, VectorClock *v) {
    s = strstr(s, "\"vector_clock\":");
    if (!s) return 0;
    return vc_from_json(v, s + 15) ? 1 : 0;
}

static void handle_line(const char *line) {
    LogEntry e;
    memset(&e, 0, sizeof(e));

    /* v8: 字段缺失时记录错误 */
    if (!json_str(line, "node_id", e.node_id, sizeof(e.node_id))) {
        log_error("缺少 node_id");
        return;
    }
    if (!json_str(line, "message", e.message, sizeof(e.message))) {
        log_error("缺少 message");
        return;
    }
    if (!json_vc(line, &e.vector_clock)) {
        log_error("缺少 vector_clock");
        return;
    }

    log_info("收到 [%s] %s", e.node_id, e.message);
    printf("       vc: "); vc_print(&e.vector_clock); printf("\n");

    LogEntry d[MAX_ENTRIES];
    int dc = buf_add(&g_buf, &e, d);
    for (int i = 0; i < dc; i++) {
        idx_add(&g_idx, &d[i]);
        log_info("交付 [%s] %s", d[i].node_id, d[i].message);
    }
    if (dc == 0) {
        log_info("缓存 [%s] %s", e.node_id, e.message);
    }

    if (g_buf.buf_count >= MAX_ENTRIES - 5) {
        log_info("缓冲接近上限: %d/%d", g_buf.buf_count, MAX_ENTRIES);
    }

    printf("[server] > "); fflush(stdout);
}

typedef struct { HANDLE pipe; } ClientCtx;

static DWORD WINAPI pipe_thread(LPVOID arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    char line[PIPE_BUF]; int lp = 0;
    while (g_running) {
        DWORD n; char chunk[1024];
        if (!ReadFile(ctx->pipe, chunk, sizeof(chunk), &n, NULL) || n == 0) {
            log_error("Agent 断开");
            break;
        }
        for (DWORD i = 0; i < n && lp < (int)sizeof(line) - 1; i++) {
            if (chunk[i] == '\n') {
                if (lp > 0) { line[lp] = 0; handle_line(line); lp = 0; }
            } else if (chunk[i] != '\r') {
                line[lp++] = chunk[i];
            }
        }
    }
    DisconnectNamedPipe(ctx->pipe);
    CloseHandle(ctx->pipe);
    free(ctx);
    return 0;
}

/* ========== 命令处理 ========== */

static void list_node(const char *node_id) {
    int cnt = 0;
    for (int i = 0; i < g_idx.entry_count; i++) {
        if (strcmp(g_idx.entries[i].node_id, node_id) == 0) {
            printf("  %s\n", g_idx.entries[i].message);
            cnt++;
        }
    }
    if (cnt == 0) printf("  (无节点 %s 的日志)\n", node_id);
}

static void cmd(const char *c) {
    if (strcmp(c, "exit") == 0) {
        log_info("exit 命令");
        InterlockedExchange(&g_running, 0);
        return;
    }
    if (strcmp(c, "help") == 0) {
        printf("  search <词>  — 全文检索\n");
        printf("  list         — 列出全部\n");
        printf("  list <节点>  — 按节点过滤\n");
        printf("  count        — 查看统计\n");
        printf("  errors       — 错误计数\n");
        printf("  exit         — 关闭\n");
        return;
    }
    /* v8: errors 命令 */
    if (strcmp(c, "errors") == 0) {
        printf("  错误次数: %d\n", g_error_count);
        return;
    }
    if (strcmp(c, "count") == 0) {
        printf("  已交付: %d  缓冲: %d/%d  错误: %d\n",
               g_idx.entry_count, g_buf.buf_count,
               MAX_ENTRIES, g_error_count);
        return;
    }
    if (strcmp(c, "list") == 0) {
        if (g_idx.entry_count == 0) { printf("  (暂无)\n"); return; }
        for (int i = 0; i < g_idx.entry_count; i++)
            printf("  [%s] %s\n", g_idx.entries[i].node_id, g_idx.entries[i].message);
        return;
    }
    if (strncmp(c, "list ", 5) == 0) { list_node(c + 5); return; }
    if (strncmp(c, "search ", 7) == 0) {
        LogEntry r[MAX_ENTRIES];
        int n = idx_search(&g_idx, c + 7, r);
        printf("  \"%s\" = %d 条:\n", c + 7, n);
        for (int i = 0; i < n; i++)
            printf("    [%s] %s\n", r[i].node_id, r[i].message);
        return;
    }
    printf("  未知命令 (help 查看)\n");
}

static int read_console_line(char *buf, int maxlen) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    int pos = 0;
    DWORD mode, oldMode;
    GetConsoleMode(hIn, &oldMode);
    mode = oldMode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    mode |= ENABLE_PROCESSED_INPUT;
    SetConsoleMode(hIn, mode);
    while (pos < maxlen - 1) {
        INPUT_RECORD rec; DWORD n;
        if (!ReadConsoleInputA(hIn, &rec, 1, &n) || n == 0) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;
        if (ch == '\r' || ch == '\n') { printf("\n"); break; }
        if (ch == '\b') { if (pos > 0) { pos--; printf("\b \b"); } continue; }
        if (ch >= ' ') { buf[pos++] = ch; printf("%c", ch); }
    }
    buf[pos] = 0;
    SetConsoleMode(hIn, oldMode);
    return pos;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    SetConsoleOutputCP(65001);
    SetConsoleCtrlHandler(signal_handler, TRUE);

    const char *pn = argc > 1 ? argv[1] : "\\\\.\\pipe\\vc_log_agg";

    /* v8: 参数校验 */
    if (argc > 1 && !validate_pipename(argv[1]))
        return 1;

    idx_init(&g_idx);
    buf_init(&g_buf);
    log_info("索引初始化完成");

    printf("+------------------------------------------+\n");
    printf("| 向量时钟分布式日志聚合服务器 v8           |\n");
    printf("| 校验 + 转义 + errors 命令                 |\n");
    printf("+------------------------------------------+\n");
    log_info("管道: %s", pn);
    printf("等待 Agent 连接...\n\n[server] > "); fflush(stdout);

    HANDLE hPipe = CreateNamedPipeA(pn, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, PIPE_BUF, PIPE_BUF, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) { log_error("创建管道失败"); return 1; }

    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) { log_error("创建事件失败"); return 1; }

    BOOL cp = ConnectNamedPipe(hPipe, &ov);
    if (cp || GetLastError() == ERROR_PIPE_CONNECTED) SetEvent(ov.hEvent);
    else if (GetLastError() != ERROR_IO_PENDING) { log_error("连接失败"); return 1; }

    while (g_running) {
        HANDLE w[2] = { ov.hEvent, GetStdHandle(STD_INPUT_HANDLE) };
        DWORD wr = WaitForMultipleObjects(2, w, FALSE, 200);
        if (wr == WAIT_OBJECT_0) {
            log_info("Agent 连接");
            ClientCtx *ctx = malloc(sizeof(ClientCtx));
            ctx->pipe = hPipe;
            CreateThread(NULL, 0, pipe_thread, ctx, 0, NULL);
            hPipe = CreateNamedPipeA(pn, PIPE_ACCESS_INBOUND,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, PIPE_BUF, PIPE_BUF, 0, NULL);
            if (hPipe == INVALID_HANDLE_VALUE) break;
            CloseHandle(ov.hEvent);
            ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            cp = ConnectNamedPipe(hPipe, &ov);
            if (cp || GetLastError() == ERROR_PIPE_CONNECTED) SetEvent(ov.hEvent);
            else if (GetLastError() != ERROR_IO_PENDING) break;
            continue;
        }
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD a;
        if (GetNumberOfConsoleInputEvents(hIn, &a) && a > 0) {
            char input[512];
            int len = read_console_line(input, sizeof(input));
            if (len == 0) { printf("[server] > "); continue; }
            cmd(input);
            if (!g_running) break;
            printf("[server] > ");
        }
    }

    log_info("关闭完毕: 交付 %d 条, 残留 %d 条, 错误 %d 次",
             g_idx.entry_count, g_buf.buf_count, g_error_count);
    CloseHandle(ov.hEvent);
    CloseHandle(hPipe);
    return 0;
}
