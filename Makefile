CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2
LDFLAGS =

SRC_DIR  = src
LIB_SRCS = $(SRC_DIR)/parser.c \
           $(SRC_DIR)/netlist.c \
           $(SRC_DIR)/hash.c \
           $(SRC_DIR)/strutil.c \
           $(SRC_DIR)/xschemrc.c
CLI_SRCS = $(SRC_DIR)/xschem2spice.c

LIB_OBJS = $(LIB_SRCS:.c=.o)
CLI_OBJS = $(CLI_SRCS:.c=.o)

BIN = xschem2spice
LIB = libxschem2spice.a

# WebAssembly (WASI) build. Set WASI_SDK to your wasi-sdk install root.
WASI_SDK ?= /opt/wasi-sdk
WASI_CC   = $(WASI_SDK)/bin/clang --target=wasm32-wasi --sysroot=$(WASI_SDK)/share/wasi-sysroot
WASM      = xschem2spice.wasm
WASM_RUNNER ?= wasmtime run --dir=.

.PHONY: all clean test wasm wasm-test

all: $(BIN) $(LIB)

$(BIN): $(CLI_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	$(MAKE) -C test

wasm: $(WASM)

$(WASM): $(CLI_SRCS) $(LIB_SRCS)
	$(WASI_CC) $(CFLAGS) -o $@ $^

wasm-test: $(WASM)
	$(WASM_RUNNER) $(WASM) --xschemrc test/wasm/xschemrc test/wasm/divider.sch \
	    | grep -v '^\*\* sch_path:' > test/wasm/divider.out.spice
	diff -u test/wasm/divider.golden.spice test/wasm/divider.out.spice
	@echo "WASM smoke test passed."

clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(BIN) $(LIB) $(WASM) test/wasm/divider.out.spice
	$(MAKE) -C test clean
