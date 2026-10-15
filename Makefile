CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra
LDFLAGS ?= $(shell sdl2-config --cflags --libs 2>/dev/null || echo "-lSDL2") -lm

SRC = c_src/main.c \
      c_src/arm.c \
      c_src/arm_mem.c \
      c_src/bios.c \
      c_src/dma.c \
      c_src/io.c \
      c_src/sound.c \
      c_src/timer.c \
      c_src/video.c

TARGET = gmu_gba

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -I./c_src $(SRC) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).exe

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
