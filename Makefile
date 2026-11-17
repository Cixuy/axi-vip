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

clean:
	rm -rf build out

.PHONY: all build run report clean
