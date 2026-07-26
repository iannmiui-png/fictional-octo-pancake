# aheui_libretro -- build with `make`. No external dependencies: the DEFLATE
# decoder, PNG reader and font are all in-tree, so there is no zlib version to
# match and nothing to vendor.
#
#   make                 native shared core
#   make platform=osx    macOS dylib
#   make platform=win    Windows dll (mingw)
#   make DEBUG=1         sanitizers, for the fuzz corpus
#   make test            build the headless frontend and run the suite

TARGET_NAME := aheui
CC          ?= gcc
platform    ?= unix

CFLAGS  += -Wall -Wextra -std=c99 -pedantic
SOURCES := aheui_libretro.c

ifeq ($(DEBUG), 1)
   CFLAGS += -O1 -g -fsanitize=address,undefined
   LDFLAGS += -fsanitize=address,undefined
else
   CFLAGS += -O2
endif

ifeq ($(platform), unix)
   TARGET  := $(TARGET_NAME)_libretro.so
   fpic    := -fPIC
   SHARED  := -shared -Wl,--no-undefined
else ifeq ($(platform), osx)
   TARGET  := $(TARGET_NAME)_libretro.dylib
   fpic    := -fPIC
   SHARED  := -dynamiclib
else ifeq ($(platform), win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   SHARED  := -shared -static-libgcc -Wl,--no-undefined
endif

all: $(TARGET)

$(TARGET): $(SOURCES) aheui_tables.h font8x8.h libretro.h
	$(CC) $(CFLAGS) $(fpic) $(SHARED) -o $@ $(SOURCES) $(LDFLAGS)

# Both generated headers come from bf_to_aheui.py and make_font.py. They are
# the core's copy of tables that also live in the Python compiler and the
# console page; regenerate rather than edit.
tables:
	python3 make_tables.py aheui_tables.h
	python3 make_font.py font8x8.h

test_frontend: test_frontend.c libretro.h font8x8.h
	$(CC) $(CFLAGS) -o $@ test_frontend.c -ldl $(LDFLAGS)

test: $(TARGET) test_frontend
	python3 test_core.py

# Reads RetroArch's own configured directories rather than assuming a layout.
install: $(TARGET)
	./install.sh

clean:
	rm -f $(TARGET_NAME)_libretro.so $(TARGET_NAME)_libretro.dylib \
	      $(TARGET_NAME)_libretro.dll test_frontend

.PHONY: all clean test tables install
