CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -Isrc -Isrc/dsp -include src/compat.h $(EXTRA_CPPFLAGS)

# ---------- platform ----------
# Linux: ALSA for audio and MIDI, X11 for the CLAP editor.
# Darwin / MINGW (MSYS2): SDL2 fronts CoreAudio or WASAPI for audio, CoreMIDI
# or WinMM for MIDI; the CLAP needs an X11 host so it is Linux-only for now.
UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)
ifeq ($(UNAME_S),Linux)
  OS := linux
  ARCH := $(UNAME_M)
  SYS_LIBS = -lm -lasound -lpthread
  EXE :=
  GUI_LDFLAGS =
else ifeq ($(UNAME_S),Darwin)
  OS := macos
  ARCH := $(UNAME_M)
  SYS_LIBS = -lm -lpthread -framework CoreMIDI -framework CoreFoundation
  EXE :=
  GUI_LDFLAGS =
else ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
  OS := windows
  ARCH := $(if $(findstring x86_64,$(UNAME_M)),x64,$(UNAME_M))
  SYS_LIBS = -lm -lpthread -lwinmm
  EXE := .exe
  GUI_LDFLAGS = -mwindows
else
  $(error unsupported platform '$(UNAME_S)')
endif

DSP_SRC = $(wildcard src/dsp/*.c)
DSP_OBJ = $(DSP_SRC:.c=.o)
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

GUI_SRC = $(wildcard src/gui/*.c)
GUI_OBJ = $(GUI_SRC:.c=.o)
# MSYS2's sdl2.pc adds -Dmain=SDL_main; gui_main.c handles main itself
FT_CFLAGS := $(filter-out -Dmain=SDL_main,$(shell pkg-config --cflags freetype2 sdl2))
FT_LIBS := $(shell pkg-config --libs freetype2 sdl2)
SDL_CFLAGS := $(filter-out -Dmain=SDL_main,$(shell pkg-config --cflags sdl2))
SDL_LIBS := $(shell pkg-config --libs sdl2)
# <SDL2/SDL.h> is spelt using its directory; Homebrew's include root is not
# on the default search path, so add it (a no-op on Linux and MSYS2).
SDL_INCROOT := $(shell pkg-config --variable=includedir sdl2)
ifneq ($(SDL_INCROOT),)
  CPPFLAGS += -I$(SDL_INCROOT)
endif

# the non-Linux audio backend is SDL2, so everything that links audio.o does
ifeq ($(OS),linux)
  AUDIO_LIBS =
else
  AUDIO_LIBS = $(SDL_LIBS)
endif

CLI = blow-your-phase-off$(EXE)
GUI = blow-your-phase-off-gui$(EXE)
TESTS = bypo-tests$(EXE)

all: $(CLI) $(GUI) $(TESTS)

$(CLI): $(DSP_OBJ) src/audio.o src/midi.o src/app.o
	$(CC) $(CFLAGS) -o $@ $^ $(SYS_LIBS) $(AUDIO_LIBS)

$(GUI): $(DSP_OBJ) $(GUI_OBJ) src/audio.o src/midi.o
	$(CC) $(CFLAGS) $(GUI_LDFLAGS) -o $@ $^ $(SYS_LIBS) $(FT_LIBS)

src/gui/%.o: src/gui/%.c src/gui/app.h src/gui/canvas.h src/gui/text.h src/gui/ui.h src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -c $< -o $@

src/audio.o: src/audio.c src/audio.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL_CFLAGS) -c $< -o $@

# the preset tests link the parser and saver
TEST_GUI_OBJ = src/gui/json_session.o src/gui/presets.o

$(TESTS): $(DSP_OBJ) $(TEST_OBJ) $(TEST_GUI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm

check: $(TESTS)
	./$(TESTS)

%.o: %.c src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/%.o: tests/%.c tests/test.h src/dsp/dsp.h src/gui/app.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------- CLAP (Linux only: the editor embeds via X11) ----------
GUI_PIC = $(filter-out src/gui/gui_main.pic.o,$(GUI_SRC:.c=.pic.o))
PIC_OBJ = $(DSP_SRC:.c=.pic.o) $(GUI_PIC) src/audio.pic.o src/midi.pic.o \
          src/plug.pic.o src/plug_gui.pic.o

%.pic.o: %.c src/dsp/dsp.h src/gui/app.h src/plug.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -fPIC -c $< -o $@

bypo.clap: $(PIC_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ -lm -lasound -lpthread -lX11 \
	      $(shell pkg-config --libs freetype2)

# ---------- dist ----------
# One drop-anywhere folder on every platform: the GUI binary with README,
# LICENSE, THIRD-PARTY-LICENSES, assets/ and presets/ beside it. Linux ships
# a tar.gz so the executable bit survives; macOS and Windows ship a zip.
# On Windows the MinGW runtime DLLs the exe needs are copied in beside it
# (ldd tells us which), so the folder runs on a machine without MSYS2.
# Every archive gets a SHA256SUMS file in the coreutils `hash  name` format.
DIST = bypomono-$(OS)-$(ARCH)
ifeq ($(OS),linux)
  DIST_ARCHIVE = $(DIST).tar.gz
else
  DIST_ARCHIVE = $(DIST).zip
endif

stage: $(GUI)
	rm -rf $(DIST)
	mkdir $(DIST)
	cp $(GUI) README.md LICENSE THIRD-PARTY-LICENSES.txt $(DIST)/
	cp -r assets $(DIST)/
	@# the stock bank is optional; without it the app starts on a default patch
	@if [ -d presets/BYPO ]; then \
	  mkdir -p $(DIST)/presets && cp -r presets/BYPO $(DIST)/presets/; \
	fi
ifeq ($(OS),windows)
	ldd $(GUI) | awk '/mingw64|ucrt64|clang64/ {print $$3}' | sort -u | xargs -r -I{} cp {} $(DIST)/
endif
ifeq ($(OS),macos)
	@# Homebrew's SDL2 and FreeType are linked by absolute path; pull them
	@# (and their dependencies) into libs/ and repoint the binary, then
	@# re-sign ad hoc, because editing a Mach-O breaks its signature and
	@# Apple silicon refuses to run an unsigned one.
	@if command -v dylibbundler >/dev/null; then \
	  dylibbundler -od -b -x $(DIST)/$(GUI) -d $(DIST)/libs -p @executable_path/libs && \
	  codesign --force --sign - $(DIST)/libs/*.dylib $(DIST)/$(GUI); \
	else \
	  echo "warning: dylibbundler not found; $(GUI) still links Homebrew dylibs by absolute path" >&2; \
	fi
endif

dist: stage
	rm -f $(DIST).tar.gz $(DIST).zip $(DIST)-SHA256SUMS.txt
ifeq ($(OS),windows)
	zip -qr $(DIST_ARCHIVE) $(DIST)
	sha256sum $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(sha256sum $(DIST)/$(GUI))" >> $(DIST)-SHA256SUMS.txt
else ifeq ($(OS),macos)
	ditto -c -k --keepParent $(DIST) $(DIST_ARCHIVE)
	shasum -a 256 $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(shasum -a 256 $(DIST)/$(GUI))" >> $(DIST)-SHA256SUMS.txt
else
	tar czf $(DIST_ARCHIVE) $(DIST)
	sha256sum $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(sha256sum $(DIST)/$(GUI))" >> $(DIST)-SHA256SUMS.txt
endif
	rm -rf $(DIST)
	@cat $(DIST)-SHA256SUMS.txt

clean:
	rm -f $(DSP_OBJ) $(TEST_OBJ) $(GUI_OBJ) $(PIC_OBJ) src/audio.o src/midi.o \
	      src/app.o $(CLI) $(GUI) $(TESTS) bypo.clap
	rm -rf bypomono-*

.PHONY: all check clean stage dist
