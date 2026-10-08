EE_BIN = build/PS2-MultiScreen.ELF
EE_OBJS = build/main.o build/DEV9_irx.o build/NETMAN_irx.o build/SMAP_irx.o
EE_LIBS = -lnetman -lps2ip -lpad -lgskit -ldmakit -lpatches -ldebug
EE_INCS = -I$(GSKIT)/ee/gs/include -I$(GSKIT)/ee/dma/include
EE_CFLAGS = -Wall -O2
EE_LDFLAGS = -L$(GSKIT)/build -L$(GSKIT)/lib -L$(PS2SDK)/ports/lib
all: $(EE_BIN)

build:
	mkdir -p build

build/%.o: build/%.c | build
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

build/DEV9_irx.c: $(PS2SDK)/iop/irx/ps2dev9.irx | build
	bin2c $< $@ DEV9_irx

build/NETMAN_irx.c: $(PS2SDK)/iop/irx/netman.irx | build
	bin2c $< $@ NETMAN_irx

build/SMAP_irx.c: $(PS2SDK)/iop/irx/smap.irx | build
	bin2c $< $@ SMAP_irx

build/main.o: src/main.c | build
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

clean:
	rm -rf build dist

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
