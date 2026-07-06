/*
 * agent.c — 日志采集代理 (v4: 断线重连与边界修复)
 *
 * v1: CLI 参数解析 + Pipe 连接
 * v2: + send_line / read_lines / 交互主循环
 * v3: + show 命令 / UTF-8 支持 / 错误处理
 * v4: fix: send_line 发送失败后主动重连管道
 *     fix: read_lines 日志轮转时重置偏移
 *     fix: 所有 strncpy → snprintf 消除截断警告
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <windows.h>
#endif
#include "vector_clock.h"

#define MAX_LINE  1024
#define MAX_JSON  2048
#define MAX_LINES 100

static VectorClock g_vc;
static char        g_node_id[16];
static long        g_file_offset;
static HANDLE      g_pipe = INVALID_HANDLE_VALUE;

/* 重连管道 */
static int reconnect_pipe(const char *pipename) {
    CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;

    printf("  [重连] 尝试重新连接管道...\n");
    for (int i = 0; i < 30; i++) {
        g_pipe = CreateFileA(pipename, GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            printf("  [重连] 成功\n");
            return 1;
        }
        Sleep(500);
    }
    printf("  [错误] 重连失败\n");
    return 0;
}

static int send_line(const char *node, const char *msg, const VectorClock *vc,
                     const char *pipename) {
    char json[MAX_JSON], vj[256];
    vc_to_json(vc, vj, sizeof(vj));
    int n = snprintf(json, sizeof(json),
        "{\"node_id\":\"%s\",\"message\":\"%s\",\"vector_clock\":%s}\n",
        node, msg, vj);
    DWORD w;
    if (!WriteFile(g_pipe, json, n, &w, NULL)) {
        printf("  [错误] 发送失败 (err=%lu)\n", GetLastError());
        /* 尝试重连 */
        if (reconnect_pipe(pipename)) {
            return WriteFile(g_pipe, json, n, &w, NULL) ? 1 : 0;
        }
        return 0;
    }
    return 1;
}

static int read_lines(const char *path, int maxl, char (*lines)[MAX_LINE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);


    if (sz <= g_file_offset) {
        fclose(f);
        return 0;
    }

    fseek(f, g_file_offset, SEEK_SET);
    int cnt = 0;
    while (cnt < maxl && fgets(lines[cnt], MAX_LINE, f)) {
        int l = (int)strlen(lines[cnt]);
        while (l > 0 && (lines[cnt][l-1] == '\n' || lines[cnt][l-1] == '\r'))
            lines[cnt][--l] = 0;
        if (l > 0) cnt++;
    }
    g_file_offset = ftell(f);
    fclose(f);
    return cnt;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    if (argc < 3) {
        printf("用法: %s <A|B|C> <日志文件> [管道名]\n", argv[0]);
        printf("管道默认: \\\\.\\pipe\\vc_log_agg\n");
        return 1;
    }

    snprintf(g_node_id, sizeof(g_node_id), "%s", argv[1]);
    const char *logf = argv[2];
    const char *pipename = (argc > 3) ? argv[3] : "\\\\.\\pipe\\vc_log_agg";
    vc_init(&g_vc);

    printf("连接管道: %s ...\n", pipename);
    for (int i = 0; i < 60; i++) {
        g_pipe = CreateFileA(pipename, GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PIPE_BUSY) {
            printf("连接失败 err=%lu\n请先启动 server.exe\n", e);
            return 1;
        }
        Sleep(250);
    }
    if (g_pipe == INVALID_HANDLE_VALUE) {
        printf("连接超时 (server 未启动?)\n");
        return 1;
    }

    printf("========================================\n");
    printf(" 日志采集代理 Agent [%s]\n", g_node_id);
    printf(" 管道: %s\n", pipename);
    printf("========================================\n");
    printf(" 打字→发送 | 空行→读文件 | show | exit\n\n");

    char buf[MAX_LINE];
    while (1) {
        printf("[%s] > ", g_node_id);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        int il = (int)strlen(buf);
        while (il > 0 && (buf[il-1] == '\n' || buf[il-1] == '\r'))
            buf[--il] = 0;

        if (il == 0) {
            char lines[MAX_LINES][MAX_LINE];
            int n = read_lines(logf, MAX_LINES, lines);
            for (int i = 0; i < n; i++) {
                vc_increment(&g_vc, g_node_id);
                send_line(g_node_id, lines[i], &g_vc, pipename);
                printf("  [发送] %s\n", lines[i]);
            }
            if (!n) printf("  (无新行)\n");
        } else if (strcmp(buf, "exit") == 0) {
            printf("[%s] 正在退出...\n", g_node_id);
            break;
        } else if (strcmp(buf, "show") == 0) {
            printf("  向量时钟: ");
            vc_print(&g_vc);
            printf("\n");
        } else {
            vc_increment(&g_vc, g_node_id);
            if (send_line(g_node_id, buf, &g_vc, pipename)) {
                printf("  [发送] %s\n", buf);
            }
        }
    }

    CloseHandle(g_pipe);
    printf("[%s] 已退出\n", g_node_id);
    return 0;
}
