/*
 * agent.c — 日志采集代理 (Linux/UDP)
 *
 * 负责的工作：
 *   1. 用 inotify 监控日志文件变化（事件驱动，不是轮询）
 *   2. 有新内容就增量读取（记偏移量，不重复读）
 *   3. 解析日志级别 [INFO]/[WARN]/[ERROR]
 *   4. 自增向量时钟、打包成 JSON、UDP 发送到服务器
 *   5. 接收服务器回传的全局时钟，合并到自己 VC 里
 *
 * 用法: ./agent <A|B|C> <日志文件> [服务器IP] [端口]
 * 默认: 127.0.0.1:9876
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include "vector_clock.h"

#define MAX_LINE   1024      /* 单行日志最大长度 */
#define MAX_JSON   2048      /* 封装后的 JSON 最大长度 */
#define MAX_LINES  100       /* 一次最多读 100 行 */
#define DEFAULT_PORT 9876
#define DEFAULT_ADDR "127.0.0.1"

/* ===================== 全局变量 ===================== */

static VectorClock g_vc;            /* 本地的向量时钟 */
static char        g_node_id[16];   /* 自己的节点名（A/B/C） */
static char        g_log_path[260]; /* 被监控的日志文件路径 */
static long        g_file_offset = 0;   /* 已读到文件哪了（增量用） */
static int         g_sock = -1;         /* UDP 套接字 */
static struct sockaddr_in g_server_addr; /* 服务器地址 */
static volatile int g_running    = 1;
static volatile int g_monitoring = 1;    /* 是否开启文件监控 */

/* 线程锁：发送锁防止多个线程同时写 socket 导致数据串位 */
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
/* VC 锁：发送线程和接收线程可能同时读写 g_vc */
static pthread_mutex_t g_vc_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* ==================================================================
 * 日志级别提取：从行首找 [INFO]/[WARN]/[ERROR]
 *
 * 比如 "[ERROR] 数据库连接超时" → 取出 "ERROR"，消息体变成"数据库连接超时"
 * 没有级别标记的默认当 INFO
 * ================================================================== */
static const char *parse_level(char *line, char *level_out, int level_sz) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '[') {
        p++;
        char buf[16];
        int i = 0;
        while (*p && *p != ']' && i < (int)sizeof(buf) - 1)
            buf[i++] = *p++;
        if (*p == ']' && i > 0) {
            buf[i] = '\0';
            if (strcmp(buf, "INFO") == 0 || strcmp(buf, "WARN") == 0
                || strcmp(buf, "ERROR") == 0) {
                strncpy(level_out, buf, level_sz - 1);
                level_out[level_sz - 1] = '\0';
                p++;
                while (*p == ' ') p++;
                if (*p == '[') {
                    while (*p && *p != ']') p++;
                    if (*p == ']') p++;
                }
                while (*p == ' ') p++;
                int remaining = (int)strlen(p);
                memmove(line, p, remaining + 1);
                return level_out;
            }
        }
    }
    strncpy(level_out, "INFO", level_sz - 1);
    return level_out;
}

/* ==================================================================
 * UDP 发送：把一条日志打成 JSON 发到服务器
 *
 * JSON 格式：
 * {"node_id":"A","timestamp":... ,"level":"ERROR","message":"...","vector_clock":{"A":3}}
 *
 * 用 sendto 发 UDP，自带线程锁防止多个线程同时写 socket
 * ================================================================== */
static int send_line(const char *node, const char *level,
                     const char *msg, const VectorClock *vc) {
    char json[MAX_JSON], vj[256];
    vc_to_json(vc, vj, sizeof(vj));
    time_t ts = time(NULL);
    int n = snprintf(json, sizeof(json),
        "{\"node_id\":\"%s\",\"timestamp\":%lld,\"level\":\"%s\","
        "\"message\":\"%s\",\"vector_clock\":%s}\n",
        node, (long long)ts, level, msg, vj);

    pthread_mutex_lock(&g_send_mutex);
    ssize_t sent = sendto(g_sock, json, (size_t)n, 0,
                          (struct sockaddr *)&g_server_addr,
                          sizeof(g_server_addr));
    pthread_mutex_unlock(&g_send_mutex);

    if (sent < 0) {
        perror("  [错误] UDP 发送失败");
        return 0;
    }
    return 1;
}

/* ==================================================================
 * 增量读取日志文件
 *
 * 原理：每次打开文件，fseek 到上次读到的位置（g_file_offset），
 * 只读新增的部分。读完更新偏移量。
 *
 * 如果文件变短了（被轮转/截断），就把偏移量重置为 0 从头读
 * ================================================================== */
static int read_lines(const char *path, int maxl, char (*lines)[MAX_LINE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);

    if (sz <= g_file_offset) {
        if (sz < g_file_offset) g_file_offset = 0;
        fclose(f);
        return 0;
    }

    fseek(f, g_file_offset, SEEK_SET);
    int cnt = 0;
    while (cnt < maxl && fgets(lines[cnt], MAX_LINE, f)) {
        int l = (int)strlen(lines[cnt]);
        while (l > 0 && (lines[cnt][l-1]=='\n' || lines[cnt][l-1]=='\r'))
            lines[cnt][--l] = 0;
        if (l > 0) cnt++;
    }
    g_file_offset = ftell(f);
    fclose(f);
    return cnt;
}

/* ==================================================================
 * 文件监控线程 (inotify — Linux 事件驱动文件监听)
 *
 * 原理：不是死循环去"看一眼文件变没"，而是让内核通知我们"文件变了"。
 * 内核在文件有 IN_MODIFY 事件时告诉 agent，agent 才去读新内容。
 * CPU 几乎为零开销。
 * ================================================================== */

static int g_inotify_fd = -1;
static int g_inotify_wd  = -1;  /* watch 描述符，-1 表示失效 */

/* 添加/更新 inotify 监视 */
static int setup_inotify_watch(const char *path) {
    if (g_inotify_wd >= 0) {
        inotify_rm_watch(g_inotify_fd, g_inotify_wd);
        g_inotify_wd = -1;
    }
    if (access(path, F_OK) == 0)
        g_inotify_wd = inotify_add_watch(g_inotify_fd, path,
                                          IN_MODIFY);
    return g_inotify_wd;
}

/* 处理文件新增内容：读取增量行 → 更新 VC → UDP 发送
 *
 * 注意：vc_increment 需要加锁，因为接收线程可能同时在改 g_vc
 */
static void process_new_lines(void) {
    if (!g_monitoring) return;
    char lines[MAX_LINES][MAX_LINE];
    int n = read_lines(g_log_path, MAX_LINES, lines);
    for (int i = 0; i < n; i++) {
        char level_buf[16] = "INFO";
        char *raw = lines[i];
        parse_level(raw, level_buf, sizeof(level_buf));
        /* 自增时要锁 VC，防止和 recv_sync_thread 冲突 */
        pthread_mutex_lock(&g_vc_mutex);
        vc_increment(&g_vc, g_node_id);
        pthread_mutex_unlock(&g_vc_mutex);
        send_line(g_node_id, level_buf, raw, &g_vc);
        printf("\n  [自动][%s][%s] %s\n", g_node_id, level_buf, raw);
        printf("[%s] > ", g_node_id); fflush(stdout);
    }
}

/* 监控线程主体：等待 inotify 事件 → 读文件 → 发 UDP */
static void *monitor_thread(void *arg) {
    (void)arg;

    g_inotify_fd = inotify_init();
    if (g_inotify_fd < 0) {
        perror("  [错误] inotify_init() 失败");
        return NULL;
    }

    /* 若文件已存在，立即添加监视；否则等它出现 */
    setup_inotify_watch(g_log_path);

    /* 足够容纳 64 个事件的缓冲 (4096 字节，inotify 对齐) */
    char evbuf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_inotify_fd, &rfds);

        /* 1s 超时，可控退出且可定期检查文件重创 */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(g_inotify_fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("  [错误] select()");
            break;
        }

        if (ret == 0) {
            /* 超时：若 watch 已失效且文件重新出现，重建监视 */
            if (g_inotify_wd < 0)
                setup_inotify_watch(g_log_path);
            continue;
        }

        /* 读取 inotify 事件 */
        ssize_t len = read(g_inotify_fd, evbuf, sizeof(evbuf));
        if (len <= 0) continue;

        for (char *p = evbuf; p < evbuf + len; ) {
            struct inotify_event *ev = (struct inotify_event *)p;

            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                /* 文件被删除/轮转/移动，watch 自动失效 */
                g_inotify_wd = -1;
                g_file_offset = 0;
            }

            if (ev->mask & IN_MODIFY)
                process_new_lines();

            p += sizeof(struct inotify_event) + ev->len;
        }
    }

    if (g_inotify_wd >= 0)
        inotify_rm_watch(g_inotify_fd, g_inotify_wd);
    close(g_inotify_fd);
    return NULL;
}

/* ==================================================================
 * 服务器反馈接收线程
 *
 * 作用：收到服务器回传的全局时钟后，合并到自己的 VC 里
 *
 * 原理：服务器每次处理完一条日志，会往 agent 的地址发一个包：
 *   {"type":"vc_sync","max_vc":{"A":3,"B":2,"C":1}}
 * 这个线程收到后，调用 vc_merge 把全局状态合并进来。
 * 这样下次发日志时 VC 就带上了其他节点的信息（比如 {A:4, B:2, C:1}）。
 * ================================================================== */
static void *recv_sync_thread(void *arg) {
    (void)arg;
    char pkt[MAX_JSON];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (g_running) {
        int n = (int)recvfrom(g_sock, pkt, sizeof(pkt) - 1, 0,
                              (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;
        pkt[n] = '\0';

        /* 查找 "max_vc":{...} 字段 */
        char *vp = strstr(pkt, "\"max_vc\":");
        if (!vp) continue;
        vp += 9; /* 跳过 "max_vc": */

        VectorClock svc;
        if (!vc_from_json(&svc, vp)) continue;

        /* 合并全局时钟到本地 VC（要加锁，防止和发送线程冲突） */
        pthread_mutex_lock(&g_vc_mutex);
        VectorClock merged;
        vc_merge(&merged, &g_vc, &svc);
        g_vc = merged;
        pthread_mutex_unlock(&g_vc_mutex);
    }
    return NULL;
}

/* ==================================================================
 * 信号处理：按 Ctrl+C 时优雅退出
 * ================================================================== */
static void sig_handler(int signo) {
    (void)signo;
    g_running = 0;
}

/* ==================================================================
 * 主函数
 *
 * 启动流程：
 *   1. 解析命令行参数（节点名、日志文件路径、服务器IP、端口）
 *   2. 初始化向量时钟，记录文件初始偏移
 *   3. 创建 UDP socket → bind（这样才能收服务器回传）
 *   4. 启动监控线程（inotify 监听文件变化）
 *   5. 启动接收线程（收服务器回传的全局时钟）
 *   6. 进入交互 Shell（打字发送 / show 看 VC / monitor 开关监控）
 * ================================================================== */
int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);

    if (argc < 3) {
        fprintf(stderr, "用法: %s <A|B|C> <日志文件> [服务器IP] [端口]\n", argv[0]);
        fprintf(stderr, "默认服务器: %s:%d\n", DEFAULT_ADDR, DEFAULT_PORT);
        return 1;
    }

    strncpy(g_node_id, argv[1], sizeof(g_node_id)-1);
    g_node_id[sizeof(g_node_id)-1] = '\0';
    strncpy(g_log_path, argv[2], sizeof(g_log_path)-1);
    g_log_path[sizeof(g_log_path)-1] = '\0';

    const char *srv_addr = argc > 3 ? argv[3] : DEFAULT_ADDR;
    int         srv_port = argc > 4 ? atoi(argv[4]) : DEFAULT_PORT;

    vc_init(&g_vc);

    /* 记录初始文件偏移：启动时不读已有内容，只读新增 */
    {
        FILE *f = fopen(g_log_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            g_file_offset = ftell(f);
            fclose(f);
        }
    }

    /* 创建 UDP 套接字 */
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        perror("创建 socket 失败");
        return 1;
    }

    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port   = htons(srv_port);
    if (inet_pton(AF_INET, srv_addr, &g_server_addr.sin_addr) <= 0) {
        fprintf(stderr, "无效服务器地址: %s\n", srv_addr);
        close(g_sock);
        return 1;
    }

    /* 绑定本地端口以便接收服务器反馈 */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(0); /* 系统自动分配端口 */
    if (bind(g_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("绑定本地端口失败");
        close(g_sock);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("========================================\n");
    printf(" 日志采集代理 Agent [%s] (Linux/UDP)\n", g_node_id);
    printf(" 目标: %s:%d\n", srv_addr, srv_port);
    printf(" 监控: %s（inotify 事件驱动）\n", g_log_path);
    printf("========================================\n");
    printf(" 打字→发送 | show→查看VC | monitor→开关\n");
    printf(" exit→退出\n\n");

    /* 启动监控线程（文件变化监听） */
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_thread, NULL);

    /* 启动接收线程（收服务器回传的全局时钟） */
    pthread_t sync_thread;
    pthread_create(&sync_thread, NULL, recv_sync_thread, NULL);

    /* 交互 Shell */
    char buf[MAX_LINE];
    while (g_running) {
        printf("[%s] > ", g_node_id); fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        int il = (int)strlen(buf);
        while (il>0 && (buf[il-1]=='\n'||buf[il-1]=='\r')) buf[--il]=0;

        if (strcmp(buf, "exit") == 0) break;
        else if (strcmp(buf, "show") == 0) {
            /* 读 VC 要加锁 */
            pthread_mutex_lock(&g_vc_mutex);
            printf("  vc: "); vc_print(&g_vc); printf("\n");
            pthread_mutex_unlock(&g_vc_mutex);
            printf("  监控: %s  偏移: %ld\n",
                   g_monitoring ? "开" : "关", g_file_offset);
        } else if (strcmp(buf, "monitor") == 0) {
            g_monitoring = !g_monitoring;
            printf("  文件监控: %s\n", g_monitoring ? "开" : "关");
        } else if (il > 0) {
            /* 手动输入一条日志发送 */
            char level_buf[16] = "INFO";
            parse_level(buf, level_buf, sizeof(level_buf));
            pthread_mutex_lock(&g_vc_mutex);
            vc_increment(&g_vc, g_node_id);
            pthread_mutex_unlock(&g_vc_mutex);
            send_line(g_node_id, level_buf, buf, &g_vc);
            printf("  [发送][%s] %s\n", level_buf, buf);
        }
    }

    g_running = 0;
    pthread_join(sync_thread, NULL);
    pthread_join(monitor, NULL);
    if (g_sock >= 0) close(g_sock);
    printf("[%s] 已退出\n", g_node_id);
    return 0;
}
