# Build com MinGW-w64.
#   No Windows (MSYS2 UCRT64):   make
#   No Linux (cross-compile):    make CROSS=x86_64-w64-mingw32-
#   Com log de diagnóstico:      make DEBUG=1

CROSS   ?=
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres

TARGET  := VideoWallpaper.exe
SRC     := src/main.c src/gui.c src/engine.c src/wallpaper.c src/config.c
OBJ     := $(SRC:src/%.c=build/%.o) build/app.res.o

CFLAGS  := -std=c11 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
ifeq ($(DEBUG),1)
CFLAGS  += -DVW_DEBUG
endif
LDFLAGS := -municode -mwindows -s -static
LIBS    := -lmfplat -lmfuuid -ld3d11 -ldxgi -ldxguid -lole32 -loleaut32 -luuid \
           -lcomctl32 -lcomdlg32 -lshell32 -ladvapi32 -lgdi32 -luser32

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/app.res.o: res/app.rc res/app.ico res/app.manifest src/resource.h | build
	$(WINDRES) -O coff -i $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
