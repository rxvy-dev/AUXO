
.RECIPEPREFIX := >

CC = gcc
AR = ar

CFLAGS = -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-red-zone -Iinclude

TARGET = libauxo.a

SRCS = src/syscall.c \
       src/string.c \
       src/stdio.c \
       src/format.c \
       src/stdlib.c \
       src/ctype.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
>$(AR) rcs $@ $^

src/%.o: src/%.c
>$(CC) $(CFLAGS) -c $< -o $@

clean:
>rm -f $(OBJS) $(TARGET)

