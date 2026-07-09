# Makefile — 向量时钟分布式日志聚合 (Linux/UDP 版)
# 用法:
#   make             编译全部（agent + server + log_gen + 测试）
#   make demo        单进程无网络演示
#   make test        集成测试
#   make test_vc     向量时钟单元测试
#   make clean       清理
#
# 多进程运行:
#   终端1: ./server
#   终端2: ./agent A logs_A.txt
#   终端3: ./log_gen logs_A.txt

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17 -O2
LDFLAGS = -lpthread

SRC = vector_clock.c indexer.c
HDR = vector_clock.h indexer.h

.PHONY: all clean test test_vc demo

all: agent server log_gen test_e2e demo_all

# ============ 主程序 ============

agent: agent.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ agent.c $(SRC) $(LDFLAGS)

server: server.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ server.c $(SRC) $(LDFLAGS)

log_gen: log_gen.c
	$(CC) $(CFLAGS) -o $@ log_gen.c

# ============ 测试 / 演示 ============

test_e2e: test_e2e.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ test_e2e.c $(SRC)

demo_all: demo_all.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ demo_all.c $(SRC)

test: test_e2e
	./test_e2e

demo: demo_all
	./demo_all

# ============ 向量时钟单元测试 ============

test_vc: vector_clock.c
	$(CC) $(CFLAGS) -DVECTOR_CLOCK_TEST -o $@ vector_clock.c
	./test_vc

# ============ 清理 ============

clean:
	rm -f agent server log_gen test_e2e demo_all test_vc *.o
