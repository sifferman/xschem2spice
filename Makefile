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

.PHONY: all clean test

all: $(BIN) $(LIB)

$(BIN): $(CLI_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(BIN)
	$(MAKE) -C test

clean:
	rm -f $(LIB_OBJS) $(CLI_OBJS) $(BIN) $(LIB)
	$(MAKE) -C test clean
