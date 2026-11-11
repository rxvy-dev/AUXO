.RECIPEPREFIX := >

CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -I../include
TARGET = test_string

.PHONY: all run clean

all: $(TARGET)

$(TARGET): test_string.c ../libauxo.a
>$(CC) $(CFLAGS) test_string.c ../libauxo.a -o $@

run: $(TARGET)
>./$(TARGET)

clean:
>rm -f $(TARGET)
