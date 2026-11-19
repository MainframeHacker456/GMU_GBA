CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra
LDFLAGS ?= $(shell sdl2-config --cflags --libs 2>/dev/null || echo "-lSDL2") -lm
ROM ?= Pokemon.gba
BIOS ?= gba_bios.bin
TARGET = gmu_gba

SRC = $(filter-out c_src/sqlite3.c, $(wildcard c_src/*.c))


all: $(TARGET)

$(TARGET): $(SRC) c_src/sqlite3.o
	$(CC) $(CFLAGS) -I./c_src $(SRC) c_src/sqlite3.o $(LDFLAGS) -o $(TARGET)

c_src/sqlite3.o: c_src/sqlite3.c
	gcc -O2 -c c_src/sqlite3.c -o c_src/sqlite3.o

clean:
	rm -f $(TARGET) $(TARGET).exe

run: $(TARGET)
	./$(TARGET) $(ROM) $(BIOS)

.PHONY: all clean run
