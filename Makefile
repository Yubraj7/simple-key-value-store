CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Isrc
TARGET = kvstore
SOURCES = src/main.c src/database.c

.PHONY: all clean test
all: $(TARGET)

$(TARGET): $(SOURCES) src/database.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@

test: $(TARGET)
	bash tests/test_database.sh

clean:
	rm -f $(TARGET) data.db
