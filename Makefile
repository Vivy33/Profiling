CC = gcc
CFLAGS = -fno-omit-frame-pointer -g -O2 -Wall -I. -I include
# -fsanitize=address

SRCS = main.c src/core/process.c src/core/system.c src/core/handler.c src/core/main_loop.c src/core/kernel_symbol.c src/elf/elf.c src/elf/symbol_table.c src/elf/vma.c src/utils/rbtree.c src/utils/hash.c src/utils/elf_cache.c src/perf/perf.c config/config.c
OBJS = $(SRCS:.c=.o)

TARGET = my_elf_reader

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) -lelf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
