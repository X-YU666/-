# Makefile — 向量时钟分布式日志聚合系统 (C语言版)
# 编译器: gcc (MinGW / Linux)
# 用法: make          → 编译全部
#       make test_vc  → 编译并运行向量时钟测试
#       make clean    → 清理

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17 -O2
LDFLAGS = -lws2_32   # Windows Winsock (Linux 移除这行)

SRC = vector_clock.c indexer.c
HDR = vector_clock.h indexer.h

.PHONY: all clean test_vc test_indexer demo

all: agent.exe server.exe

# ---- 向量时钟测试 ----
test_vc.exe: vector_clock.c
	$(CC) $(CFLAGS) -DVECTOR_CLOCK_TEST -o test_vc.exe vector_clock.c
test_vc: test_vc.exe
	./test_vc.exe

# ---- 倒排索引测试 ----
test_indexer.exe: indexer.c vector_clock.c
	$(CC) $(CFLAGS) -DINDEXER_TEST -o test_indexer.exe indexer.c vector_clock.c $(LDFLAGS)
test_indexer: test_indexer.exe
	./test_indexer.exe

# ---- Agent ----
agent.exe: agent.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o agent.exe agent.c $(SRC) $(LDFLAGS)

# ---- Server ----
server.exe: server.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o server.exe server.c $(SRC) $(LDFLAGS)

# ---- Demo: 启动所有窗口 ----
demo: agent.exe server.exe
	@echo "请手动运行:"
	@echo "  窗口1: server.exe"
	@echo "  窗口2: agent.exe A logs_A.txt"
	@echo "  窗口3: agent.exe B logs_B.txt"
	@echo "  窗口4: agent.exe C logs_C.txt"

clean:
	del /f /q *.exe *.o 2>nul || rm -f *.exe *.o
