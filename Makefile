# axi-vip —— AXI4 / APB 协议验证平台
CC      := g++
CXXFLAGS:= -O2 -std=c++17 -Wall -Wextra
PY      := python

BIN := build/axi_vip.exe
SRC := $(wildcard src/*.cpp)

all: run report

$(BIN): $(SRC) src/sim.h src/dv.h
	@mkdir -p build
	$(CC) $(CXXFLAGS) -o $(BIN) $(SRC)

build: $(BIN)

run: $(BIN)
	@mkdir -p out
	./$(BIN) --out out

report:
	$(PY) tools/report.py out docs/report.html

# 多 seed 回归：换随机激励再看违规/失配是否仍为 0、覆盖率是否仍收敛
regress: $(BIN)
	@mkdir -p out
	@for sd in 0 1 7 42; do \
		printf 'seed %-3s ' $$sd; ./$(BIN) --out out --seed $$sd --json; \
	done

clean:
	rm -rf build out

.PHONY: all build run report regress clean
