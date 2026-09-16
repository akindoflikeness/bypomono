CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -Isrc -Isrc/dsp -include src/compat.h $(EXTRA_CPPFLAGS)

# ---------- platform ----------
# Linux: ALSA for audio and MIDI, X11 for the CLAP editor.
# Darwin / MINGW (MSYS2): SDL2 fronts CoreAudio or WASAPI for audio, CoreMIDI
# or WinMM for MIDI; the CLAP editor embeds in Cocoa and Win32 respectively.
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
  # Deployment target for our own code and for the from-source static deps.
  # CFLAGS is on every macOS compile and link line, so setting it here covers
  # the C, Objective-C and link steps alike.
  MACOS_MIN := 12.0
  CFLAGS += -mmacosx-version-min=$(MACOS_MIN)
  SYS_LIBS = -lm -lpthread -framework CoreMIDI -framework CoreFoundation
  EXE :=
  GUI_LDFLAGS =
else ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S)))
  OS := windows
  ARCH := $(if $(findstring x86_64,$(UNAME_M)),x64,$(UNAME_M))
  SYS_LIBS = -lm -lwinmm
  EXE := .exe
  GUI_LDFLAGS = -mwindows
  # The MinGW runtime (libgcc, winpthread) is linked into every binary, so
  # nothing has to sit beside it for the runtime. -Bstatic stays in force to
  # the end of the line so gcc's own trailing -lpthread resolves static too.
  RUNTIME_LIBS = -static-libgcc -Wl,-Bstatic -lwinpthread
else
  $(error unsupported platform '$(UNAME_S)')
endif
RUNTIME_LIBS ?=

DSP_SRC = $(wildcard src/dsp/*.c)
DSP_OBJ = $(DSP_SRC:.c=.o)
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

GUI_SRC = $(wildcard src/gui/*.c)
GUI_OBJ = $(GUI_SRC:.c=.o)
# macOS links SDL2, FreeType, libpng and zlib as static archives built from
# source, so ask pkg-config for the private deps too (the system frameworks
# SDL2 needs come out of sdl2.pc, same list as `sdl2-config --static-libs`).
ifeq ($(OS),macos)
  PKG_LIBS := pkg-config --static --libs
else
  PKG_LIBS := pkg-config --libs
endif

# MSYS2's sdl2.pc adds -Dmain=SDL_main; gui_main.c handles main itself
FT_CFLAGS := $(filter-out -Dmain=SDL_main,$(shell pkg-config --cflags freetype2 sdl2))
FT_LIBS := $(shell $(PKG_LIBS) freetype2 sdl2)
SDL_CFLAGS := $(filter-out -Dmain=SDL_main,$(shell pkg-config --cflags sdl2))
SDL_LIBS := $(shell $(PKG_LIBS) sdl2)
# <SDL2/SDL.h> is spelt using its directory; the prefix include root is not
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

VERSION := $(shell grep 'define APP_VERSION' src/gui/app.h | cut -d'"' -f2)

CLI = blow-your-phase-off$(EXE)
GUI = blow-your-phase-off-gui$(EXE)
TESTS = bypo-tests$(EXE)

all: $(CLI) $(GUI) $(TESTS)

$(CLI): $(DSP_OBJ) src/audio.o src/midi.o src/app.o
	$(CC) $(CFLAGS) -o $@ $^ $(SYS_LIBS) $(AUDIO_LIBS) $(RUNTIME_LIBS)

$(GUI): $(DSP_OBJ) $(GUI_OBJ) src/audio.o src/midi.o
	$(CC) $(CFLAGS) $(GUI_LDFLAGS) -o $@ $^ $(SYS_LIBS) $(FT_LIBS) $(RUNTIME_LIBS)

src/gui/%.o: src/gui/%.c src/gui/app.h src/gui/canvas.h src/gui/text.h src/gui/ui.h src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -c $< -o $@

src/audio.o: src/audio.c src/audio.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SDL_CFLAGS) -c $< -o $@

# the preset tests link the parser and saver
TEST_GUI_OBJ = src/gui/json_session.o src/gui/presets.o
TEST_GUI_OBJ += src/gui/command.o src/gui/focus.o

$(TESTS): $(DSP_OBJ) $(TEST_OBJ) $(TEST_GUI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm

check: $(TESTS)
	./$(TESTS)

%.o: %.c src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/%.o: tests/%.c tests/test.h src/dsp/dsp.h src/gui/app.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# ---------- CLAP ----------
# The plugin embeds its editor natively (X11 / Cocoa / Win32) and takes its
# audio from the host, so it links FreeType and the window system but no SDL.
FT_ONLY_LIBS := $(shell $(PKG_LIBS) freetype2)
ifeq ($(OS),linux)
  CLAP_LIBS = $(SYS_LIBS) -lX11 $(FT_ONLY_LIBS) -Wl,--no-undefined
  GUI_BACKEND_SRC = src/plug_gui_x11.c
else ifeq ($(OS),macos)
  CLAP_LIBS = -lm -lpthread -framework CoreMIDI -framework CoreFoundation \
              -framework Cocoa -framework QuartzCore -framework CoreVideo \
              $(FT_ONLY_LIBS)
  GUI_BACKEND_SRC = src/plug_gui_cocoa.m
else
  # A host loads the .clap from its own search path, not the plugin's folder,
  # so the plugin carries FreeType, its private deps and the MinGW runtime
  # inside it: -static picks every lib*.a over lib*.dll.a, and -lstdc++ covers
  # the HarfBuzz that MSYS2's FreeType is built against.
  FT_STATIC_LIBS := $(shell pkg-config --static --libs freetype2)
  CLAP_LIBS = -static -static-libgcc -Wl,-Bstatic -lwinpthread \
              $(FT_STATIC_LIBS) -lstdc++ -lm -lwinmm -lgdi32 -luser32
  GUI_BACKEND_SRC = src/plug_gui_win32.c
endif
GUI_BACKEND_OBJ = $(basename $(GUI_BACKEND_SRC)).pic.o

GUI_PIC = $(filter-out src/gui/gui_main.pic.o,$(GUI_SRC:.c=.pic.o))
PIC_OBJ = $(DSP_SRC:.c=.pic.o) $(GUI_PIC) src/plug_audio.pic.o src/midi.pic.o \
          src/plug.pic.o src/plug_gui.pic.o $(GUI_BACKEND_OBJ)

%.pic.o: %.c src/dsp/dsp.h src/gui/app.h src/plug.h src/plug_gui.h src/plug_gui_backend.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -fPIC -c $< -o $@

# A .clap installed on Linux or Windows is one file with nothing beside it,
# so the plugin build carries the faces inside it. .incbin reads them relative
# to this directory. The standalone keeps using assets/, so its object is the
# empty half of the file.
FONT_FILES = assets/fonts/pixeloid_mono/PixeloidMono.ttf \
             assets/fonts/unifontexmono/UnifontExMono.ttf \
             assets/fonts/european_teletext/EuropeanTeletext.ttf

src/gui/fonts_embedded.pic.o: src/gui/fonts_embedded.c src/gui/text.h \
                              $(FONT_FILES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -DBYPO_EMBED_FONTS -fPIC \
	      -c $< -o $@

# manual retain/release, so no -fobjc-arc
src/plug_gui_cocoa.pic.o: src/plug_gui_cocoa.m src/plug_gui_backend.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) \
	      -x objective-c -fno-objc-arc -fPIC -c $< -o $@

ifeq ($(OS),macos)
# A macOS CLAP is a bundle directory; Apple silicon refuses to load one that
# carries no signature, hence the ad hoc (`-`) codesign.
bypo.clap: $(PIC_OBJ)
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	$(CC) $(CFLAGS) -dynamiclib -Wl,-install_name,@rpath/bypo \
	      -o $@/Contents/MacOS/bypo $^ $(CLAP_LIBS)
	printf 'BNDL????' > $@/Contents/PkgInfo
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '	<key>CFBundleIdentifier</key><string>com.akol.bypo</string>' \
	  '	<key>CFBundleName</key><string>BYPO</string>' \
	  '	<key>CFBundleExecutable</key><string>bypo</string>' \
	  '	<key>CFBundlePackageType</key><string>BNDL</string>' \
	  '	<key>CFBundleSignature</key><string>????</string>' \
	  '	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>' \
	  '	<key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '	<key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '</dict>' \
	  '</plist>' > $@/Contents/Info.plist
	@# assets inside the bundle, before the signature so it seals them
	mkdir -p $@/Contents/Resources
	cp -R assets $@/Contents/Resources/
	codesign --force --sign - $@
else
bypo.clap: $(PIC_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(CLAP_LIBS)
endif

# ---------- dist ----------
# One drop-anywhere folder on every platform: the GUI binary with README,
# LICENSE, THIRD-PARTY-LICENSES, assets/ and presets/ beside it. Linux ships
# a tar.gz so the executable bit survives; macOS and Windows ship a zip.
# On Windows the SDL2 and FreeType DLLs the exe needs are copied in beside it
# (ldd tells us which), so the folder runs on a machine without MSYS2; the
# exe ships under the plain name, without the -gui the build uses.
# Every archive gets a SHA256SUMS file in the coreutils `hash  name` format.
DIST = bypomono-$(OS)-$(ARCH)
ifeq ($(OS),linux)
  DIST_ARCHIVE = $(DIST).tar.gz
else
  DIST_ARCHIVE = $(DIST).zip
endif
ifeq ($(OS),windows)
  SHIP_GUI = blow-your-phase-off$(EXE)
else
  SHIP_GUI = $(GUI)
endif
# Finder will not launch a loose Mach-O and offers right-click-Open for
# bundles only, so the macOS GUI ships inside a minimal .app.
MACAPP = BYPO.app
MACAPP_DIR = $(DIST)/$(MACAPP)
MACAPP_BIN = $(MACAPP_DIR)/Contents/MacOS/$(GUI)

stage: $(GUI)
	rm -rf $(DIST)
	mkdir $(DIST)
	cp $(GUI) $(DIST)/$(SHIP_GUI)
	cp README.md LICENSE THIRD-PARTY-LICENSES.txt $(DIST)/
	cp -r assets $(DIST)/
	mkdir -p $(DIST)/presets && cp -r presets/BYPO $(DIST)/presets/
	@# bypo.clap has its own target because it needs the CLAP headers; ship it
	@# if it was built, and -R so the macOS bundle directory survives the copy
	@if [ -e bypo.clap ]; then cp -R bypo.clap $(DIST)/; \
	else echo "warning: bypo.clap not built; this archive ships the app only" >&2; fi
ifeq ($(OS),windows)
	ldd $(GUI) | awk '/mingw64|ucrt64|clang64/ {print $$3}' | sort -u | xargs -r -I{} cp {} $(DIST)/
endif
ifeq ($(OS),macos)
	@# assets/ and presets/ stay beside the bundle; the app finds them by
	@# walking out of Contents/MacOS.
	mkdir -p $(MACAPP_DIR)/Contents/MacOS
	mv $(DIST)/$(GUI) $(MACAPP_BIN)
	printf 'APPL????' > $(MACAPP_DIR)/Contents/PkgInfo
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '	<key>CFBundleIdentifier</key><string>com.akol.bypo.standalone</string>' \
	  '	<key>CFBundleName</key><string>BYPO</string>' \
	  '	<key>CFBundleExecutable</key><string>$(GUI)</string>' \
	  '	<key>CFBundlePackageType</key><string>APPL</string>' \
	  '	<key>CFBundleSignature</key><string>????</string>' \
	  '	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>' \
	  '	<key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '	<key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '	<key>LSMinimumSystemVersion</key><string>$(MACOS_MIN)</string>' \
	  '	<key>NSHighResolutionCapable</key><true/>' \
	  '</dict>' \
	  '</plist>' > $(MACAPP_DIR)/Contents/Info.plist
	@# The binary links SDL2, FreeType, libpng and zlib statically, so there is
	@# nothing to bundle; sign the bundle ad hoc so Finder offers right-click-Open
	@# and Apple silicon will run it.
	codesign --force --sign - $(MACAPP_DIR)
endif

dist: stage
	rm -f $(DIST).tar.gz $(DIST).zip $(DIST)-SHA256SUMS.txt
ifeq ($(OS),windows)
	zip -qr $(DIST_ARCHIVE) $(DIST)
	sha256sum $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(sha256sum $(DIST)/$(SHIP_GUI))" >> $(DIST)-SHA256SUMS.txt
else ifeq ($(OS),macos)
	ditto -c -k --keepParent $(DIST) $(DIST_ARCHIVE)
	shasum -a 256 $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(shasum -a 256 $(MACAPP_BIN))" >> $(DIST)-SHA256SUMS.txt
else
	tar czf $(DIST_ARCHIVE) $(DIST)
	sha256sum $(DIST_ARCHIVE) > $(DIST)-SHA256SUMS.txt
	printf '# inside %s:\n# %s\n' $(DIST_ARCHIVE) "$$(sha256sum $(DIST)/$(GUI))" >> $(DIST)-SHA256SUMS.txt
endif
	rm -rf $(DIST)
	@cat $(DIST)-SHA256SUMS.txt

clean:
	rm -f $(DSP_OBJ) $(TEST_OBJ) $(GUI_OBJ) $(PIC_OBJ) src/audio.o src/midi.o \
	      src/app.o $(CLI) $(GUI) $(TESTS)
	rm -rf bypomono-* bypo.clap

.PHONY: all check clean stage dist
