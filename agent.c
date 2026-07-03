/*
 * agent.c — 日志采集代理 (v1: 骨架)
 *
 * 功能：CLI 参数解析 + Named Pipe 连接 + 重试逻辑
 *
 * 用法: agent.exe <A|B|C> <日志文件> [管道名]
 * 默认管道: \\.\pipe\vc_log_agg
 *
 * 后续迭代将加入消息发送、文件读取、交互主循环等。
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

static VectorClock g_vc;
static char        g_node_id[16];
static HANDLE      g_pipe = INVALID_HANDLE_VALUE;

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    if (argc < 3) {
        printf("用法: %s <A|B|C> <日志文件> [管道名]\n", argv[0]);
        printf("管道默认: \\\\.\\pipe\\vc_log_agg\n");
        return 1;
    }

    strncpy(g_node_id, argv[1], sizeof(g_node_id) - 1);
    const char *logf = argv[2];
    const char *pipename = (argc > 3) ? argv[3] : "\\\\.\\pipe\\vc_log_agg";
    vc_init(&g_vc);

    printf("Agent [%s] 启动\n", g_node_id);
    printf("日志文件: %s\n", logf);
    printf("连接管道: %s ...\n", pipename);

    /* 尝试连接 Server 的 Named Pipe，最多重试 60 次（~15秒） */
    for (int i = 0; i < 60; i++) {
        g_pipe = CreateFileA(pipename, GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            printf("管道连接成功\n");
            break;
        }
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PIPE_BUSY) {
            printf("连接失败 (err=%lu)\n请先启动 server.exe\n", e);
            return 1;
        }
        printf("等待 server 启动... (%d/60)\n", i + 1);
        Sleep(250);
    }

    if (g_pipe == INVALID_HANDLE_VALUE) {
        printf("连接超时，请确认 server.exe 已启动\n");
        return 1;
    }

    printf("========================================\n");
    printf(" 日志采集代理 Agent [%s] (骨架)\n", g_node_id);
    printf("========================================\n");
    printf("等待后续实现...\n");

    /* 暂时空循环，后续加入消息发送逻辑 */
    printf("按 Ctrl+C 退出\n");
    while (1) {
        Sleep(1000);
    }

    CloseHandle(g_pipe);
    return 0;
}
