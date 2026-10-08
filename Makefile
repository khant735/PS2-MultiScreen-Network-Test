EE_BIN = build/PS2-MultiScreen.ELF
EE_OBJS = build/main.o
EE_LIBS = -lpad -ldebug
EE_INCS =
EE_CFLAGS = -Wall -O2
EE_LDFLAGS =
all: $(EE_BIN)

build:
	mkdir -p build

build/main.o: src/main.c | build
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

clean:
	rm -rf build dist

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
