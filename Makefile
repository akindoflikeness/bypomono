CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS = -Isrc -Isrc/dsp -include src/compat.h $(EXTRA_CPPFLAGS)

DSP_SRC = $(wildcard src/dsp/*.c)
DSP_OBJ = $(DSP_SRC:.c=.o)
TEST_SRC = $(wildcard tests/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

GUI_SRC = $(wildcard src/gui/*.c)
GUI_OBJ = $(GUI_SRC:.c=.o)
FT_CFLAGS := $(shell pkg-config --cflags freetype2 sdl2)
FT_LIBS := $(shell pkg-config --libs freetype2 sdl2)

all: blow-your-phase-off blow-your-phase-off-gui bypo-tests

blow-your-phase-off: $(DSP_OBJ) src/audio.o src/midi.o src/app.o
	$(CC) $(CFLAGS) -o $@ $^ -lm -lasound -lpthread

blow-your-phase-off-gui: $(DSP_OBJ) $(GUI_OBJ) src/audio.o src/midi.o
	$(CC) $(CFLAGS) -o $@ $^ -lm -lasound -lpthread $(FT_LIBS)

src/gui/%.o: src/gui/%.c src/gui/app.h src/gui/canvas.h src/gui/text.h src/gui/ui.h src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -c $< -o $@

bypo-tests: $(DSP_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ -lm

check: bypo-tests
	./bypo-tests

%.o: %.c src/dsp/dsp.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

GUI_PIC = $(filter-out src/gui/gui_main.pic.o,$(GUI_SRC:.c=.pic.o))
PIC_OBJ = $(DSP_SRC:.c=.pic.o) $(GUI_PIC) src/audio.pic.o src/midi.pic.o \
          src/plug.pic.o src/plug_gui.pic.o

%.pic.o: %.c src/dsp/dsp.h src/gui/app.h src/plug.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(FT_CFLAGS) -fPIC -c $< -o $@

bypo.clap: $(PIC_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ -lm -lasound -lpthread -lX11 \
	      $(shell pkg-config --libs freetype2)

DIST = bypomono-linux-x86_64

dist: blow-your-phase-off-gui
	rm -rf $(DIST) $(DIST).tar.gz
	mkdir $(DIST)
	cp blow-your-phase-off-gui README.md LICENSE THIRD-PARTY-LICENSES.txt $(DIST)/
	cp -r assets presets $(DIST)/
	tar czf $(DIST).tar.gz $(DIST)
	rm -rf $(DIST)

clean:
	rm -f $(DSP_OBJ) $(TEST_OBJ) $(GUI_OBJ) $(PIC_OBJ) src/audio.o src/midi.o \
	      src/app.o blow-your-phase-off blow-your-phase-off-gui bypo-tests \
	      bypo.clap

.PHONY: all check clean dist
