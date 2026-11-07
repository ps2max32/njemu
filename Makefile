# NJEMU on maxsdk: the PS2 port, one ELF for each emulated system.
#
#   make [OUT=<dir>]                  all four in $(OUT)/<system>/<system>.elf
#   make TARGET=MVS [FRAMES=n]        one of CPS1, CPS2, MVS, NCDZ
#
# src/ is upstream; src/maxsdk/ is the platform backend in place of src/ps2/,
# which it shares two small policy headers with. Data the emulators read at run
# time (translations, game metadata, the font) is made by upstream's host tool.

HERE     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PS2MAX   ?= $(abspath $(HERE)/../..)
MAXSDK   ?= $(PS2MAX)/libs/maxsdk
MAXSDK_OUT ?= $(PS2MAX)/build/libs/maxsdk
LIBOUT   := $(PS2MAX)/build/apps/njemu
OUT      ?= $(LIBOUT)

ifeq ($(TARGET),)

SYSTEMS := CPS1 CPS2 MVS NCDZ
all: $(SYSTEMS)
$(SYSTEMS): submodules
	$(MAKE) -C $(HERE) TARGET=$@ OUT=$(OUT)

submodules:
	git -C $(HERE) submodule update --init third_party/miniz third_party/libmad

clean:
	rm -rf $(addprefix $(OUT)/,cps1 cps2 mvs ncdz host)

.PHONY: all clean submodules $(SYSTEMS)

else

SYS      := $(shell echo $(TARGET) | tr A-Z a-z)
SOUT     := $(OUT)/$(SYS)
# Neo Geo and CPS2 games are larger than the PS2's memory: they are resident in
# virtual memory, the code in core (docs/vm.md in maxsdk). MX_MEMORY=phys: all
# in core, with upstream's streaming cache instead; only that runs on PCSX2's
# recompiler. CPS1 and NCDZ fit.
ifneq ($(filter MVS CPS2,$(TARGET)),)
MX_MEMORY ?= vm
MX_VM_TEXT ?= core
else
MX_MEMORY ?= phys
endif
# under mx-kernel (docs/kernel.md in maxsdk)
MX_KERNEL ?= 1
all: $(SOUT)/$(SYS).elf data

# maxsdk.mk's OUT: where it would build VU code; this program has none
OUT_TOP  := $(OUT)
OUT      := $(SOUT)
include $(MAXSDK)/mk/maxsdk.mk

PICOLIBC_SRC ?= $(PS2MAX)/libs/picolibc
PICOLIBC := $(LIBOUT)/picolibc/install
PICOLIBC_A := $(PICOLIBC)/lib/libc.a
LIBRT    = $(MAXSDK_OUT)/librt.a
LIBMXC_OBJS = $(SOUT)/libc/glue.o $(MAXSDK_OUT)/libc/compat.c.o $(MAXSDK_OUT)/libc/atomic.c.o \
              $(MAXSDK_OUT)/libc/malloc.c.o
LIBC_LIBS = $(LIBMXC_OBJS) $(PICOLIBC_A) $(MX_LIB) $(LIBRT)

VERSION  := $(shell git -C $(HERE) describe --tags --always 2>/dev/null || echo unknown)
TARGETF  := --target=mips64el -mcpu=r5900 -mabi=n32 -fno-pic -mno-abicalls -G0
OPT      ?= -O3
SRC      := $(HERE)/src
B        := $(SRC)/maxsdk
GEN      := $(SOUT)/gen
HOSTOUT  := $(OUT_TOP)/host
TOOL     := $(HOSTOUT)/install/bin/njemu-tool
MINIZ    := $(HERE)/third_party/miniz
MAD      := $(HERE)/third_party/libmad

DEFS     := -DBUILD_$(TARGET) -DPS2 -DPLATFORM_STR='"PS2"' -DTARGET_STR='"$(TARGET)"' \
            -DGUI -DSAVE_STATE -DCOMMAND_LIST -DPS2_CACHE_RESERVE_KB=1024 \
            -DNJEMU_MAIN_THREAD_PRIORITY=64 -DNJEMU_SOUND_THREAD_PRIORITY=65 \
            -DDEFAULT_VIDEO_OUTPUT_MODE=VIDEO_OUTPUT_480I -DMVS_WIDESCREEN=0 -DCPS_WIDESCREEN=0 \
            -DVERSION_MAJOR=3 -DVERSION_MINOR=1 -DVERSION_BUILD=0 -DVERSION_STR='"$(VERSION)"' \
            -DNJEMU_RUNTIME_BUILD_ID='"$(VERSION)"' -DNJEMU_GIT_SHA='"$(VERSION)"' \
            -DMINIZ_NO_TIME
# a run without a player (src/maxsdk/maxsdk_test.c): end at flip FRAMES; PLAY:
# {flip,buttons}, and SHOTS: flip, each with a comma after it; TESTDIR: the
# directory of the data, for a test ELF that is run from elsewhere
ifneq ($(FRAMES),)
DEFS     += -DMAXSDK_TEST=$(FRAMES) '-DMAXSDK_TEST_PLAY=$(PLAY)' '-DMAXSDK_TEST_SHOTS=$(SHOTS)' \
            $(if $(TESTDIR),'-DMAXSDK_TEST_DIR="$(TESTDIR)"')
endif
ifeq ($(MX_MEMORY),vm)
DEFS     += -DMAXSDK_VM -DUSE_CACHE=0 -DMVS_PCM_CACHE=0 -DCACHE_DEFAULT_READ_SIZE_KB=64
else ifneq ($(filter MVS CPS2,$(TARGET)),)
DEFS     += -DUSE_CACHE=1
ifeq ($(TARGET),MVS)
DEFS     += -DMVS_PCM_CACHE=1 -DCACHE_DEFAULT_READ_SIZE_KB=16
else
DEFS     += -DMVS_PCM_CACHE=0 -DCACHE_DEFAULT_READ_SIZE_KB=64
endif
else
DEFS     += -DUSE_CACHE=0 -DMVS_PCM_CACHE=0 -DCACHE_DEFAULT_READ_SIZE_KB=64
endif
ifeq ($(TARGET),MVS)
DEFS     += -DMVS_DIRTY_SPRITE_UPLOADS=1 -DMVS_NATIVE_WORD_ACCESS=1 -DPS2_MVS_RAM_FASTPATH=1
endif

NJCFLAGS := $(TARGETF) $(OPT) -std=gnu11 -w -MMD -fno-common -ffunction-sections -fdata-sections \
            -fno-strict-aliasing -fsigned-char -ffast-math -fno-math-errno \
            -nostdlibinc -isystem $(B)/include -isystem $(PICOLIBC)/include -D_GNU_SOURCE \
            -I$(SRC) -I$(SRC)/include -I$(GEN) -I$(MINIZ) -I$(MAD) -I$(MAXSDK)/ee/include \
            $(DEFS) $(EXTRA_CFLAGS)

COMMON   := emumain sound/sndintrf common/cache common/filer common/loadrom common/power_driver \
            common/ticker_driver common/input_driver common/input_state common/video_driver \
            common/ui_draw_driver common/ui_layout common/ui_text_driver common/ui_text_catalog \
            common/ui_utf8 common/platform_memory_info common/memory_plan common/sound \
            common/audio_producer_cpu common/audio_producer_binding_cpu common/zip_archive common/cmdlist common/command_data \
            common/state common/font/graphic common/font/ascii_14p common/font/font_s \
            common/font/bshadow common/font/command common/font/ascii_14 common/font/latin1_14 \
            common/font/gbk_s14_runtime common/ui_draw common/config common/icon/menu_extra \
            common/ui common/ui_menu common/png \
            cpu/m68000/m68000 cpu/m68000/c68k cpu/z80/z80 cpu/z80/cz80
BACKEND  := maxsdk/maxsdk_drivers maxsdk/maxsdk_platform maxsdk/maxsdk_ticker maxsdk/maxsdk_time \
            maxsdk/maxsdk_input maxsdk/maxsdk_video maxsdk/maxsdk_audio maxsdk/maxsdk_thread \
            maxsdk/maxsdk_ui_draw maxsdk/maxsdk_posix maxsdk/maxsdk_test

SYS_MVS  := common/game_metadata common/dip_metadata mvs/mvs mvs/driver mvs/memintrf \
            mvs/processed_assets mvs/inptport mvs/dipsw mvs/timer mvs/vidhrdw mvs/sprite_common \
            mvs/sprite mvs/wide common/wide_profile mvs/wide_debug mvs/wide_runtime mvs/pd4990a \
            mvs/neocrypt mvs/biosmenu sound/2610intf sound/ym2610 common/icon/mvs_s common/icon/mvs_l
SYS_CPS1 := common/game_metadata common/dip_metadata common/coin cps1/cps1 cps1/driver cps1/memintrf \
            cps1/inptport cps1/dipsw cps1/timer cps1/vidhrdw cps1/sprite_common cps1/sprite \
            cps1/eeprom cps1/kabuki cps1/wide common/wide_profile sound/2151intf sound/ym2151 \
            sound/qsound common/icon/cps_s common/icon/cps_l
SYS_CPS2 := common/game_database common/coin cps2/cps2 cps2/cps2crpt cps2/driver cps2/memintrf \
            cps2/inptport cps2/timer cps2/vidhrdw cps2/sprite_common cps2/sprite cps2/eeprom \
            cps2/wide common/wide_profile sound/qsound common/icon/cps_s common/icon/cps_l
SYS_NCDZ := common/game_metadata ncdz/cdda ncdz/cdrom ncdz/driver ncdz/inptport ncdz/memintrf \
            ncdz/ncdz ncdz/resource_source ncdz/sprite_common ncdz/sprite ncdz/timer ncdz/vidhrdw \
            sound/2610intf sound/ym2610 common/mp3 common/icon/ncdz_s common/icon/ncdz_l

OBJS     := $(patsubst %,$(SOUT)/o/%.o,$(COMMON) $(BACKEND) $(SYS_$(TARGET))) \
            $(SOUT)/o/gen/ui_unicode_glyph.o $(SOUT)/o/miniz/miniz.o $(SOUT)/o/miniz/miniz_tdef.o \
            $(SOUT)/o/miniz/miniz_tinfl.o $(SOUT)/o/miniz/miniz_zip.o

ifeq ($(TARGET),NCDZ)
MADSRC   := bit decoder fixed frame huffman layer12 layer3 stream synth timer version
OBJS     += $(patsubst %,$(SOUT)/o/mad/%.o,$(MADSRC))
MADFLAGS := -DFPM_64BIT -DHAVE_CONFIG_H -DOPT_SPEED -I$(GEN)/mad
CDEPS_MAD := $(GEN)/mad.h
endif

CDEPS    := $(PICOLIBC_A) $(GEN)/ui_unicode_glyph.c $(GEN)/miniz_export.h $(CDEPS_MAD)
# libmad's header as its configure makes it: 64-bit fixed point, for the R5900
$(GEN)/mad.h: $(MAD)/mad.h
	@mkdir -p $(dir $@)
	sed 's/^# *define FPM_[A-Z0-9]*$$/# define FPM_64BIT/' $< > $@

# the test build's flags, so that a change of them rebuilds what reads them
FLAGSTAMP := $(GEN)/flags.txt
$(shell mkdir -p $(GEN); echo '$(FRAMES) $(PLAY) $(SHOTS) $(MX_MEMORY) $(TESTDIR)' > $(FLAGSTAMP).new; \
  cmp -s $(FLAGSTAMP).new $(FLAGSTAMP) || mv $(FLAGSTAMP).new $(FLAGSTAMP); rm -f $(FLAGSTAMP).new)
$(patsubst %,$(SOUT)/o/%.o,$(BACKEND)): $(FLAGSTAMP)
# what miniz's CMake would generate
$(GEN)/miniz_export.h:
	@mkdir -p $(dir $@)
	echo '#define MINIZ_EXPORT' > $@

$(SOUT)/o/%.o: $(SRC)/%.c $(CDEPS)
	@mkdir -p $(dir $@)
	$(CC) $(NJCFLAGS) -c $< -o $@
$(SOUT)/o/gen/%.o: $(GEN)/%.c $(CDEPS)
	@mkdir -p $(dir $@)
	$(CC) $(NJCFLAGS) -c $< -o $@
$(SOUT)/o/miniz/%.o: $(MINIZ)/%.c $(CDEPS)
	@mkdir -p $(dir $@)
	$(CC) $(NJCFLAGS) -c $< -o $@
$(SOUT)/o/mad/%.o: $(MAD)/%.c $(CDEPS) $(GEN)/mad/config.h
	@mkdir -p $(dir $@)
	$(CC) $(NJCFLAGS) $(MADFLAGS) -c $< -o $@
$(GEN)/mad/config.h:
	@mkdir -p $(dir $@)
	echo '#include <stdlib.h>' > $@

$(SOUT)/libc/glue.o: $(MAXSDK)/ee/libc/glue.c $(PICOLIBC_A) $(HERE)/Makefile
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -ffreestanding,$(CFLAGS)) -nostdlibinc -isystem $(PICOLIBC)/include \
	  -Dread=mx_glue_read -Dwrite=mx_glue_write -c $< -o $@

$(SOUT)/$(SYS).elf: $(OBJS) $(MX_LIB) $(MX_CRT0) $(MX_LD) $(LIBC_LIBS)
	$(call link,$@,-z muldefs --error-limit=0 --defsym=__mx_stack_size=0x40000 $(MX_CRT0) $(OBJS) $(LIBC_LIBS))

$(PICOLIBC_A):
	@mkdir -p $(LIBOUT)/picolibc
	sed "s|@LLVM@|$(LLVM)|g" $(MAXSDK)/ee/libc/cross-r5900.txt.in > $(LIBOUT)/picolibc/cross.txt
	meson setup --wipe $(LIBOUT)/picolibc/build-meson $(PICOLIBC_SRC) \
	  --cross-file $(LIBOUT)/picolibc/cross.txt --prefix=$(PICOLIBC) -Dmultilib=false \
	  -Dpicocrt=false -Dsemihost=false -Dtests=false -Dthread-local-storage=false \
	  -Dposix-io=true -Dspecsdir=none -Dio-long-long=true -Dbuildtype=release
	ninja -C $(LIBOUT)/picolibc/build-meson install

# ---- upstream's host tool and what it makes -----------------------------------

$(TOOL):
	cmake -S $(HERE)/tools/host -B $(HOSTOUT)/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Wno-clobbered \
	  -DCMAKE_INSTALL_PREFIX=$(HOSTOUT)/install
	cmake --build $(HOSTOUT)/build --parallel
	cmake --install $(HOSTOUT)/build

META     := $(HERE)/metadata/$(SYS).tsv
UNICODE  := --unicode-source $(META) --unicode-source $(HERE)/metadata/command_list_unicode.txt
ifneq ($(filter MVS CPS1,$(TARGET)),)
UNICODE  += --unicode-source $(HERE)/metadata/$(SYS)_dips.json
endif
ROMINFO  := $(HERE)/resources/$(SYS)/rominfo.$(SYS)
DATA     := $(SOUT)/data

$(GEN)/ui_unicode_glyph.c: $(TOOL)
	@mkdir -p $(GEN) $(DATA)/lang
	cd $(HERE) && $(TOOL) translations --translations-dir translations --build \
	  --output-dir $(DATA)/lang --unicode-map-output $@ $(UNICODE)

$(DATA)/font/gbk_s14.bin: $(TOOL)
	@mkdir -p $(dir $@)
	$(TOOL) font --source $(SRC)/common/font/gbk_s14.c --table-source $(SRC)/common/font/gbk_tbl.c \
	  --output $@

ifeq ($(TARGET),CPS2)
METADATA := $(DATA)/game_database.cps2
$(METADATA): $(TOOL) $(META) $(ROMINFO)
	@mkdir -p $(DATA)
	cd $(HERE) && $(TOOL) game-database --core cps2 --metadata $(META) --rominfo $(ROMINFO) \
	  --output $@ --gamelist-output $(DATA)/gamelist_cps2.txt
else ifeq ($(TARGET),NCDZ)
METADATA := $(DATA)/game_metadata.ncdz
$(METADATA): $(TOOL) $(META)
	@mkdir -p $(DATA)
	cd $(HERE) && $(TOOL) game-metadata --core ncdz --source $(META) --output $@
else
METADATA := $(DATA)/game_metadata.$(SYS) $(DATA)/dip_metadata.$(SYS)
$(DATA)/game_metadata.$(SYS): $(TOOL) $(META) $(ROMINFO)
	@mkdir -p $(DATA)
	cd $(HERE) && $(TOOL) game-metadata --core $(SYS) --source $(META) --output $@ \
	  --rominfo $(ROMINFO) --gamelist-output $(DATA)/gamelist_$(SYS).txt
$(DATA)/dip_metadata.$(SYS): $(TOOL) $(HERE)/metadata/$(SYS)_dips.json
	@mkdir -p $(DATA)
	cd $(HERE) && $(TOOL) dip-metadata --source $(HERE)/metadata/$(SYS)_dips.json --output $@
endif

data: $(METADATA) $(DATA)/font/gbk_s14.bin

.PHONY: all data

-include $(shell find $(SOUT)/o -name '*.d' 2>/dev/null)

endif
