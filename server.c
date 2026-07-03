/*
 * server.c v6 — 按节点过滤：list <节点>
 * 新增：list_node() 只显示指定节点的日志
 *       help 补全 list <节点> 文档
 * 用法: server.exe [管道名]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "vector_clock.h"
#include "indexer.h"

static InvertedIndex g_idx;
static CausalBuffer  g_buf;
static volatile LONG g_running = 1;

/* ========== JSON 解析 ========== */

static int json_str(const char *s, const char *key, char *out, int n) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    s = strstr(s, pat);
    if (!s) return 0;
    s += strlen(pat);
    int i = 0;
    while (*s && *s != '"' && i < n - 1) { out[i++] = *s; s++; }
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
    json_str(line, "node_id",  e.node_id,  sizeof(e.node_id));
    json_str(line, "message",  e.message,  sizeof(e.message));
    json_vc (line, &e.vector_clock);

    printf("\n[收到] [%s] %s  vc: ", e.node_id, e.message);
    vc_print(&e.vector_clock);
    printf("\n");

    LogEntry d[MAX_ENTRIES];
    int dc = buf_add(&g_buf, &e, d);
    for (int i = 0; i < dc; i++) {
        idx_add(&g_idx, &d[i]);
        printf("[交付] [%s] %s\n", d[i].node_id, d[i].message);
    }
    if (dc == 0) {
        printf("[缓存] [%s] %s  (等待依赖)\n", e.node_id, e.message);
    }
    printf("[server] > "); fflush(stdout);
}

typedef struct { HANDLE pipe; } ClientCtx;

static DWORD WINAPI pipe_thread(LPVOID arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    char line[4096]; int lp = 0;
    while (g_running) {
        DWORD n; char chunk[1024];
        if (!ReadFile(ctx->pipe, chunk, sizeof(chunk), &n, NULL) || n == 0)
            break;
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

/* v6: 按节点过滤 */
static void list_node(const char *node_id) {
    int cnt = 0;
    for (int i = 0; i < g_idx.entry_count; i++) {
        if (strcmp(g_idx.entries[i].node_id, node_id) == 0) {
            printf("  %s\n", g_idx.entries[i].message);
            cnt++;
        }
    }
    if (cnt == 0) {
        printf("  (无节点 %s 的日志)\n", node_id);
    }
}

static void cmd(const char *c) {
    if (strcmp(c, "exit") == 0) {
        InterlockedExchange(&g_running, 0);
        printf("服务器关闭\n");
        return;
    }
    if (strcmp(c, "help") == 0) {
        printf("  search <词>  — 全文检索\n");
        printf("  list         — 列出全部\n");
        printf("  list <节点>  — 按节点过滤\n");
        printf("  count        — 查看统计\n");
        printf("  exit         — 关闭服务器\n");
        return;
    }
    if (strcmp(c, "count") == 0) {
        printf("  已交付: %d  缓冲中: %d\n",
               g_idx.entry_count, g_buf.buf_count);
        return;
    }
    if (strcmp(c, "list") == 0) {
        if (g_idx.entry_count == 0) {
            printf("  (暂无日志)\n");
        } else {
            for (int i = 0; i < g_idx.entry_count; i++) {
                printf("  [%s] %s\n",
                       g_idx.entries[i].node_id,
                       g_idx.entries[i].message);
            }
        }
        return;
    }
    /* v6: list <节点> */
    if (strncmp(c, "list ", 5) == 0) {
        list_node(c + 5);
        return;
    }
    if (strncmp(c, "search ", 7) == 0) {
        LogEntry r[MAX_ENTRIES];
        int n = idx_search(&g_idx, c + 7, r);
        printf("  \"%s\" = %d 条:\n", c + 7, n);
        for (int i = 0; i < n; i++) {
            printf("    [%s] %s\n", r[i].node_id, r[i].message);
        }
        return;
    }
    printf("  未知命令\n");
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

    const char *pn = argc > 1 ? argv[1] : "\\\\.\\pipe\\vc_log_agg";

    idx_init(&g_idx);
    buf_init(&g_buf);

    printf("+------------------------------------------+\n");
    printf("| 向量时钟分布式日志聚合服务器 v6           |\n");
    printf("| 新增：按节点过滤 list <节点>               |\n");
    printf("+------------------------------------------+\n");
    printf("等待 Agent 连接...\n\n[server] > "); fflush(stdout);

    HANDLE hPipe = CreateNamedPipeA(pn, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) return 1;

    OVERLAPPED ov; memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    BOOL cp = ConnectNamedPipe(hPipe, &ov);
    if (cp || GetLastError() == ERROR_PIPE_CONNECTED) SetEvent(ov.hEvent);
    else if (GetLastError() != ERROR_IO_PENDING) return 1;

    while (g_running) {
        HANDLE w[2] = { ov.hEvent, GetStdHandle(STD_INPUT_HANDLE) };
        DWORD wr = WaitForMultipleObjects(2, w, FALSE, 200);
        if (wr == WAIT_OBJECT_0) {
            ClientCtx *ctx = malloc(sizeof(ClientCtx));
            ctx->pipe = hPipe;
            CreateThread(NULL, 0, pipe_thread, ctx, 0, NULL);
            hPipe = CreateNamedPipeA(pn, PIPE_ACCESS_INBOUND,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
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
    CloseHandle(ov.hEvent);
    CloseHandle(hPipe);
    return 0;
}
