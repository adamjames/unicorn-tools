.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET   := cosmic_stream
BUILD    := build
SOURCES  := source
INCLUDES := source
RSF      := $(TOPDIR)/cosmic_stream.rsf

# Title ID for Luma3DS sysmodule loading
# Replacing NIM (system update service) - safe to disable
# To restore: delete this CXI from /luma/sysmodules/
TITLE_ID := 0004013000002C02

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) $(INCLUDE) -D__3DS__
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lctru -lm

LIBDIRS  := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD      := $(CC)
export OFILES  := $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).3dsx $(OUTPUT).elf $(OUTPUT).cxi $(CURDIR)/$(TITLE_ID).cxi

# Build CXI with title ID filename for Luma3DS sysmodule loading
$(CURDIR)/$(TITLE_ID).cxi: $(BUILD)
	@makerom -f ncch -rsf $(RSF) -o $@ -elf $(OUTPUT).elf
	@echo "Built sysmodule: $(TITLE_ID).cxi"

cxi: $(CURDIR)/$(TITLE_ID).cxi

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

-include $(DEPENDS)

endif
