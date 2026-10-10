CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=gnu99 -march=native
LDFLAGS = 

TARGET = net-zerocopy-filter
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests
BENCH_DIR = benchmarks

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

TEST_SRCS = $(wildcard $(TEST_DIR)/*.c)
TEST_TARGETS = $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/test_%, $(TEST_SRCS))

BENCH_SRCS = $(wildcard $(BENCH_DIR)/*.c)
BENCH_TARGETS = $(patsubst $(BENCH_DIR)/%.c, $(BIN_DIR)/bench_%, $(BENCH_SRCS))

.PHONY: all build test clean benchmark directories

all: directories build

directories:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

build: directories $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/test_%: $(TEST_DIR)/%.c $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/bench_%: $(BENCH_DIR)/%.c $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(filter-out $(OBJ_DIR)/main.o, $(OBJS)) $< -o $@ $(LDFLAGS)

test: directories
	@if [ -d "$(TEST_DIR)" ] && [ -n "$(TEST_SRCS)" ]; then \
		$(MAKE) $(TEST_TARGETS); \
		for t in $(TEST_TARGETS); do \
			./$$t || exit 1; \
		done; \
	else \
		echo "No tests found in $(TEST_DIR). Skipping."; \
	fi

benchmark: directories
	@if [ -d "$(BENCH_DIR)" ] && [ -n "$(BENCH_SRCS)" ]; then \
		$(MAKE) $(BENCH_TARGETS); \
		for b in $(BENCH_TARGETS); do \
			./$$b || exit 1; \
		done; \
	else \
		echo "No benchmarks found in $(BENCH_DIR). Skipping."; \
	fi

clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR)