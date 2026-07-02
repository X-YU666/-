/*
 * server.c v3 — 工作线程：读取 Pipe + 显示收到的消息
 * 新增：CreateThread 工作线程逐行 ReadFile 打印原始消息
 *       主循环续接下一个 Agent 连接
 * 用法: server.exe [管道名]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static volatile LONG g_running = 1;

typedef struct { HANDLE pipe; } ClientCtx;

static DWORD WINAPI pipe_thread(LPVOID arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    char buf[1024];
    while (g_running) {
        DWORD n;
        if (!ReadFile(ctx->pipe, buf, sizeof(buf), &n, NULL) || n == 0)
            break;
        buf[n] = 0;
        printf("\n[收到] %s", buf);
        printf("[server] > "); fflush(stdout);
    }
    DisconnectNamedPipe(ctx->pipe);
    CloseHandle(ctx->pipe);
    free(ctx);
    return 0;
}

static void cmd(const char *c) {
    if (strcmp(c, "exit") == 0) {
        InterlockedExchange(&g_running, 0);
        printf("服务器关闭\n");
        return;
    }
    if (strcmp(c, "help") == 0) {
        printf("  exit  — 关闭服务器\n");
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

    printf("+------------------------------------------+\n");
    printf("| 向量时钟分布式日志聚合服务器 v3           |\n");
    printf("| 新增：工作线程 + 续接下一个 Agent         |\n");
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
            /* v3: 启动工作线程处理已连接的 Agent */
            ClientCtx *ctx = malloc(sizeof(ClientCtx));
            ctx->pipe = hPipe;
            CreateThread(NULL, 0, pipe_thread, ctx, 0, NULL);

            /* 创建下一个管道实例，继续等待新的 Agent */
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
