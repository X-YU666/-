/*
 * server.c v1 — 项目骨架
 * 创建管道、等待 Agent 连接、打印信息
 * 用法: server.exe [管道名]
 *
 * 编译: gcc -std=c17 -O2 -o server.exe server.c vector_clock.c indexer.c
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    SetConsoleOutputCP(65001);

    const char *pn = argc > 1 ? argv[1] : "\\\\.\\pipe\\vc_log_agg";

    printf("+------------------------------------------+\n");
    printf("| 向量时钟分布式日志聚合服务器 v1           |\n");
    printf("| 骨架：管道就绪，等待 Agent 连接           |\n");
    printf("+------------------------------------------+\n");
    printf("管道: %s\n", pn);
    printf("等待 Agent 连接...\n");

    HANDLE hPipe = CreateNamedPipeA(pn, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        printf("创建管道失败\n");
        return 1;
    }

    if (ConnectNamedPipe(hPipe, NULL)) {
        printf("Agent 已连接!\n");
    }

    printf("待开发...\n");
    CloseHandle(hPipe);
    return 0;
}
