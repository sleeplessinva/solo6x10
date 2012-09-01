KERNELVER := $(shell uname -r)
KERNELDIR = /lib/modules/$(KERNELVER)/build
KERNELSRC = /lib/modules/$(KERNELVER)/source

# For my local crud
-include make.extras

solo6x10-objs	:= solo6010-core.o solo6010-i2c.o solo6010-p2m.o \
		   solo6010-v4l2.o solo6010-tw28.o solo6010-gpio.o \
		   solo6010-disp.o solo6010-enc.o solo6010-v4l2-enc.o \
		   solo6010-g723.o solo6010-eeprom.o

# For when the kernel isn't compiled with it
ifeq ($(CONFIG_VIDEOBUF_DMA_CONTIG),)
solo6x10-objs += videobuf-dma-contig.o
endif

obj-m		:= solo6x10.o

modules modules_install clean: FORCE
	$(MAKE) $(MAKEARGS) -C $(KERNELDIR) M=$(shell pwd) $@

install: modules_install FORCE
clean: clean_local

clean_local: FORCE
	rm -f Module.markers modules.order videobuf-dma-contig.c videobuf-dma-contig.c.in


# Workaround for Debian et al
ifeq ($(wildcard $(KERNELSRC)/drivers),)
kerneltar := $(wildcard /usr/src/linux-source-*.tar.bz2)
ifeq ($(kerneltar),)
$(error Missing files on the kernel source directory, and no tarball found)
endif
kerneltar := $(firstword $(shell ls -rv $(kerneltar)))

$(obj)/%.in: $(kerneltar)
	$(if $(KBUILD_VERBOSE:1=),@echo '  EXTRACT' $@)
	$(Q)tar -Oxf $< --wildcards '*/$(@F:.in=)' > $@
else
$(obj)/%.in: $(KERNELSRC)/drivers/media/video/%
	$(if $(KBUILD_VERBOSE:1=),@echo '  LN' $@)
	$(Q)ln -s $< $@
endif

$(obj)/videobuf-dma-contig.c: %:%.in
	$(if $(KBUILD_VERBOSE:1=),@echo '  MERGE' $@)
	$(Q)sed '/^MODULE_/d' $< > $@

FORCE:
