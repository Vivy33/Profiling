CC = gcc
CFLAGS = -fno-omit-frame-pointer -g -O2 -Wall -I. -I include
LDFLAGS_READER = -lelf -lsqlite3 -lpthread -lmicrohttpd -lcjson
LDFLAGS_QUERY = -lsqlite3 -lcurl -lcjson

# Enable AddressSanitizer/UndefinedSanitizer when SANITIZE=1
ifeq ($(SANITIZE),1)
CFLAGS := -fno-omit-frame-pointer -g -O1 -Wall -I. -I include -fsanitize=address -fsanitize=undefined
LDFLAGS_READER += -fsanitize=address -fsanitize=undefined
LDFLAGS_QUERY += -fsanitize=address -fsanitize=undefined
# Helpful ASAN options; can be adjusted per need
export ASAN_OPTIONS := detect_leaks=1:halt_on_error=0:abort_on_error=0:fast_unwind_on_malloc=0
endif

# VPATH tells make where to look for source files
VPATH = src/core:src/elf:src/utils:src/perf:config

# --- Profiler ---
READER_SRCS = main.c \
              src/core/process.c \
              src/core/system.c \
              src/core/handler.c \
              src/core/main_loop.c \
              src/core/kernel_symbol.c \
              src/elf/elf.c \
              src/elf/symbol_table.c \
              src/elf/vma.c \
              src/utils/rbtree.c \
              src/utils/hash.c \
              src/utils/elf_cache.c \
              src/perf/perf.c \
              config/config.c \
              src/utils/concurrent_queue.c \
              src/utils/database.c \
              src/utils/http_server.c

# Create object file names by taking just the basename and adding .o
READER_OBJS = $(notdir $(READER_SRCS:.c=.o))
READER_TARGET = profiling_tool

# --- Query Tool ---
QUERY_SRCS = config/query_tool.c
QUERY_OBJS = $(notdir $(QUERY_SRCS:.c=.o))
QUERY_TARGET = query_tool

.PHONY: all clean

all: $(READER_TARGET) $(QUERY_TARGET)

$(READER_TARGET): $(READER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_READER)

$(QUERY_TARGET): $(QUERY_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS_QUERY)

# This generic rule will now work correctly with VPATH
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(READER_TARGET) $(QUERY_TARGET)
	find . -type f -name "*.o" -delete