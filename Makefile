CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -D_XOPEN_SOURCE=700
TARGET  = mysh
SRC     = mysh.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)