# Makefile for QEMU.

# Always point to the root of the build tree (needs GNU make).
BUILD_DIR=$(CURDIR)

# All following code might depend on configuration variables
ifneq ($(wildcard config-host.mak),)
# Put the all: rule here so that config-host.mak can contain dependencies.
all: build-all
include config-host.mak
include $(SRC_PATH)/rules.mak
config-host.mak: $(SRC_PATH)/configure
	@echo $@ is out-of-date, running configure
	@sed -n "/.*Configured with/s/[^:]*: //p" $@ | sh
else
config-host.mak:
	@echo "Please call configure before running make!"
	@exit 1
endif

GENERATED_HEADERS = config-host.h trace.h qemu-options.def
ifeq ($(TRACE_BACKEND),dtrace)
GENERATED_HEADERS += trace-dtrace.h
endif
GENERATED_SOURCES += trace.c

# Don't try to regenerate Makefile or configure
# We don't generate any of them
Makefile: ;
configure: ;

.PHONY: all clean cscope distclean dvi html info install install-doc \
	pdf recurse-all speed tar tarbin test build-all

$(call set-vpath, $(SRC_PATH))

LIBS+=-lz $(LIBS_TOOLS)

ifneq ($(filter %-softmmu, $(TARGET_DIRS)),)
tools-y += qemu-io$(EXESUF) qemu-img$(EXESUF)
tools-$(CONFIG_SMARTCARD_NSS) += vscclient$(EXESUF)
tools-$(CONFIG_VIRTFS) += fsdev/virtfs-proxy-helper$(EXESUF)
tools-$(CONFIG_POSIX) += qemu-nbd$(EXESUF)
tools-$(CONFIG_GUEST_AGENT) += qemu-ga$(EXESUF)
helpers-$(CONFIG_LINUX) = qemu-bridge-helper$(EXESUF)
docs-y += qemu.1 qemu-img.1 qemu-nbd.8 docs/qmp-commands.txt
docs-$(CONFIG_VIRTFS) += fsdev/virtfs-proxy-helper.1
endif

docs-y += qemu.html qemu-tech.html

SUBDIR_MAKEFLAGS=$(if $(V),,--no-print-directory) BUILD_DIR=$(BUILD_DIR)
SUBDIR_DEVICES_MAK=$(patsubst %, %/config-devices.mak, $(TARGET_DIRS))
SUBDIR_DEVICES_MAK_DEP=$(patsubst %, %/config-devices.mak.d, $(TARGET_DIRS))

config-all-devices.mak: $(SUBDIR_DEVICES_MAK)
	$(call quiet-command,cat $(SUBDIR_DEVICES_MAK) | grep =y | sort -u > $@,"  GEN   $@")

-include $(SUBDIR_DEVICES_MAK_DEP)

%/config-devices.mak: default-configs/%.mak
	$(call quiet-command,$(SHELL) $(SRC_PATH)/scripts/make_device_config.sh $@ $<, "  GEN   $@")
	@if test -f $@; then \
	  if cmp -s $@.old $@; then \
	    mv $@.tmp $@; \
	    cp -p $@ $@.old; \
	  else \
	    if test -f $@.old; then \
	      echo "WARNING: $@ (user modified) out of date.";\
	    else \
	      echo "WARNING: $@ out of date.";\
	    fi; \
	    echo "Run \"make defconfig\" to regenerate."; \
	    rm $@.tmp; \
	  fi; \
	 else \
	  mv $@.tmp $@; \
	  cp -p $@ $@.old; \
	 fi

defconfig:
	rm -f config-all-devices.mak $(SUBDIR_DEVICES_MAK)

-include config-all-devices.mak

build-all: $(tools-y) $(helpers-y) recurse-all

ifdef BUILD_DOCS
build-all: $(docs-y)
install: install-doc
endif

config-host.h: config-host.h-timestamp
config-host.h-timestamp: config-host.mak

SUBDIR_RULES=$(patsubst %,subdir-%, $(TARGET_DIRS))

subdir-%:
	$(call quiet-command,$(MAKE) $(SUBDIR_MAKEFLAGS) -C $* V="$(V)" TARGET_DIR="$*/" all,)

ifneq ($(wildcard config-host.mak),)
include $(SRC_PATH)/Makefile.objs
endif

subdir-libcacard: $(oslib-obj-y) $(trace-obj-y) qemu-timer-common.o

$(filter %-softmmu,$(SUBDIR_RULES)): $(universal-obj-y) $(trace-obj-y) $(common-obj-y) $(extra-obj-y) subdir-libdis

$(filter %-user,$(SUBDIR_RULES)): $(universal-obj-y) $(trace-obj-y) subdir-libdis-user subdir-libuser

ROMSUBDIR_RULES=$(patsubst %,romsubdir-%, $(ROMS))
romsubdir-%:
	$(call quiet-command,$(MAKE) $(SUBDIR_MAKEFLAGS) -C pc-bios/$* V="$(V)" TARGET_DIR="$*/",)

ALL_SUBDIRS=$(TARGET_DIRS) $(patsubst %,pc-bios/%, $(ROMS))

recurse-all: $(SUBDIR_RULES) $(ROMSUBDIR_RULES)

%.def: %.hx
	$(call quiet-command,sh $(SRC_PATH)/scripts/hxtool -h < $< > $@,"  GEN   $@")

QEMU_CFLAGS += $(FMOD_CFLAGS)
QEMU_CFLAGS += $(CURL_CFLAGS)
QEMU_CFLAGS += $(VNC_TLS_CFLAGS)
QEMU_CFLAGS += $(VNC_SASL_CFLAGS)
QEMU_CFLAGS += $(VNC_JPEG_CFLAGS)
QEMU_CFLAGS += $(VNC_PNG_CFLAGS)
QEMU_CFLAGS += $(BLUEZ_CFLAGS)
QEMU_CFLAGS += $(SDL_CFLAGS)

QEMU_CFLAGS += -I$(SRC_PATH)/include
qemu-img.o qemu-io.o $(block-obj-y) $(common-obj-y): \
	QEMU_CFLAGS += -Iqapi-generated

qemu-options.def: $(SRC_PATH)/qemu-options.hx

ui/cocoa.o: ui/cocoa.m

version.o: $(SRC_PATH)/version.rc config-host.h
	$(call quiet-command,$(WINDRES) -I. -o $@ $<,"  RC    $(TARGET_DIR)$@")

version-obj-$(CONFIG_WIN32) += version.o
######################################################################
# Support building shared library libcacard

.PHONY: libcacard.la install-libcacard
ifeq ($(LIBTOOL),)
libcacard.la:
	@echo "libtool is missing, please install and rerun configure"; exit 1

install-libcacard:
	@echo "libtool is missing, please install and rerun configure"; exit 1
else
libcacard.la: $(oslib-obj-y) qemu-timer-common.o $(addsuffix .lo, $(basename $(trace-obj-y)))
	$(call quiet-command,$(MAKE) $(SUBDIR_MAKEFLAGS) -C libcacard V="$(V)" TARGET_DIR="$*/" libcacard.la,)

install-libcacard: libcacard.la
	$(call quiet-command,$(MAKE) $(SUBDIR_MAKEFLAGS) -C libcacard V="$(V)" TARGET_DIR="$*/" install-libcacard,)
endif

vscclient$(EXESUF): $(libcacard-y) $(oslib-obj-y) $(trace-obj-y) qemu-timer-common.o libcacard/vscclient.o
	$(call quiet-command,$(CC) $(LDFLAGS) -o $@ $^ $(libcacard_libs) $(LIBS),"  LINK  $@")

######################################################################

qemu-img.o: qemu-img-cmds.def
qemu-img-cmds.def: $(SRC_PATH)/qemu-img-cmds.hx

tools-obj-y = $(oslib-obj-y) $(trace-obj-y) qemu-tool.o qemu-timer.o \
	qemu-timer-common.o main-loop.o notify.o \
	iohandler.o cutils.o iov.o async.o
tools-obj-$(CONFIG_POSIX) += compatfd.o

qemu-img$(EXESUF): qemu-img.o $(tools-obj-y) $(block-obj-y)
qemu-nbd$(EXESUF): qemu-nbd.o $(tools-obj-y) $(block-obj-y)
qemu-io$(EXESUF): qemu-io.o cmd.o $(tools-obj-y) $(block-obj-y)

qemu-bridge-helper$(EXESUF): qemu-bridge-helper.o

fsdev/virtfs-proxy-helper$(EXESUF): fsdev/virtfs-proxy-helper.o fsdev/virtio-9p-marshal.o oslib-posix.o $(trace-obj-y)
fsdev/virtfs-proxy-helper$(EXESUF): LIBS += -lcap

qemu-ga$(EXESUF): LIBS = $(LIBS_QGA)
qemu-ga$(EXESUF): QEMU_CFLAGS += -Iqga

gen-out-type = $(subst .,-,$(suffix $@))

define qapi-rules
$1qapi-types.c $1qapi-types.h: $2 $(SRC_PATH)/scripts/qapi-types.py
	$$(call quiet-command,$$(PYTHON) $$(SRC_PATH)/scripts/qapi-types.py $$(gen-out-type) -o "$(dir $1)" -p "$(notdir $1)" < $$<, "  GEN   $$@")
$1qapi-visit.c $1qapi-visit.h: $2 $(SRC_PATH)/scripts/qapi-visit.py
	$$(call quiet-command,$$(PYTHON) $$(SRC_PATH)/scripts/qapi-visit.py $$(gen-out-type) -o "$(dir $1)" -p "$(notdir $1)" < $$<, "  GEN   $$@")
$1qmp-commands.h $1qmp-marshal.c: $2 $(SRC_PATH)/scripts/qapi-commands.py
	$$(call quiet-command,$$(PYTHON) $$(SRC_PATH)/scripts/qapi-commands.py $$(gen-out-type) $3 -o "$(dir $1)" -p "$(notdir $1)" < $$<, "  GEN   $$@")

GENERATED_HEADERS += $1qapi-types.h $1qapi-visit.h $1qmp-commands.h
GENERATED_SOURCES += $1qmp-marshal.c $1qapi-types.c $1qapi-visit.c
$1qapi-types.o $1qapi-visit.o $1qmp-marshal.o: \
	$1qapi-types.h $1qapi-visit.h $1qmp-commands.h
endef

$(eval $(call qapi-rules,qga/,$(SRC_PATH)/qapi-schema-guest.json))
$(eval $(call qapi-rules,qapi-generated/,$(SRC_PATH)/qapi-schema.json, -m))
qemu-ga$(EXESUF): qemu-ga.o $(qga-obj-y) $(tools-obj-y) $(qapi-obj-y) $(qobject-obj-y) $(version-obj-y)

ifneq ($(wildcard config-host.mak),)
include $(SRC_PATH)/tests/Makefile
endif

QEMULIBS=libhw32 libhw64 libuser libdis libdis-user

clean:
	rm -f $(addsuffix *.o, $(sort $(dir $(common-obj-y))))
	rm -f $(addsuffix *.d, $(sort $(dir $(common-obj-y))))
	rm -f $(addsuffix *.o, $(sort $(dir $(universal-obj-y))))
	rm -f $(addsuffix *.d, $(sort $(dir $(universal-obj-y))))
	rm -f $(addsuffix *.o, $(sort $(dir $(trace-obj-y))))
	rm -f $(addsuffix *.d, $(sort $(dir $(trace-obj-y))))
	rm -f $(addsuffix *.o, $(sort $(dir $(qga-obj-y))))
	rm -f $(addsuffix *.d, $(sort $(dir $(qga-obj-y))))
	rm -f $(addsuffix *.o, $(sort $(dir $(qapi-obj-y))))
	rm -f $(addsuffix *.d, $(sort $(dir $(qapi-obj-y))))
	rm -f qemu-img-cmds.def qemu-options.def
	rm -f trace-dtrace.dtrace trace-dtrace.dtrace-timestamp
	@# May not be present in GENERATED_HEADERS
	rm -f trace-dtrace.h trace-dtrace.h-timestamp
	rm -f $(foreach f,$(GENERATED_HEADERS),$(f) $(f)-timestamp)
	rm -f $(foreach f,$(GENERATED_SOURCES),$(f) $(f)-timestamp)
	rm -rf qapi-generated
	$(MAKE) -C tests/tcg clean
	for d in $(ALL_SUBDIRS) $(QEMULIBS) libcacard; do \
	if test -d $$d; then $(MAKE) -C $$d $@ || exit 1; fi; \
        done

distclean: clean
	rm -f config-host.mak config-host.h* config-host.ld $(docs-y)
	rm -f qemu-options.texi qemu-img-cmds.texi hmp-commands.texi
	rm -f config-all-devices.mak
	rm -f roms/seabios/config.mak roms/vgabios/config.mak
	rm -f qemu.info qemu.aux qemu.cp qemu.cps qemu.dvi
	rm -f qemu.fn qemu.fns qemu.info qemu.ky qemu.kys
	rm -f qemu.log qemu.pdf qemu.pg qemu.toc qemu.tp
	rm -f qemu.vr
	rm -f config.log
	rm -f linux-headers/asm
	rm -f qemu-tech.info qemu-tech.aux qemu-tech.cp qemu-tech.dvi qemu-tech.fn qemu-tech.info qemu-tech.ky qemu-tech.log qemu-tech.pdf qemu-tech.pg qemu-tech.toc qemu-tech.tp qemu-tech.vr
	for d in $(TARGET_DIRS) $(QEMULIBS); do \
	rm -rf $$d || exit 1 ; \
        done

KEYMAPS=da     en-gb  et  fr     fr-ch  is  lt  modifiers  no  pt-br  sv \
ar      de     en-us  fi  fr-be  hr     it  lv  nl         pl  ru     th \
common  de-ch  es     fo  fr-ca  hu     ja  mk  nl-be      pt  sl     tr \
bepo

ifdef INSTALL_BLOBS
BLOBS=bios.bin sgabios.bin vgabios.bin vgabios-cirrus.bin \
vgabios-stdvga.bin vgabios-vmware.bin vgabios-qxl.bin \
ppc_rom.bin openbios-sparc32 openbios-sparc64 openbios-ppc \
pxe-e1000.rom pxe-eepro100.rom pxe-ne2k_pci.rom \
pxe-pcnet.rom pxe-rtl8139.rom pxe-virtio.rom \
qemu-icon.bmp \
bamboo.dtb petalogix-s3adsp1800.dtb petalogix-ml605.dtb \
multiboot.bin linuxboot.bin kvmvapic.bin \
s390-zipl.rom \
spapr-rtas.bin slof.bin \
palcode-clipper
else
BLOBS=
endif

install-doc: $(docs-y)
	$(INSTALL_DIR) "$(DESTDIR)$(qemu_docdir)"
	$(INSTALL_DATA) qemu.html  qemu-tech.html "$(DESTDIR)$(qemu_docdir)"
	$(INSTALL_DATA) QMP/qmp-commands.txt "$(DESTDIR)$(qemu_docdir)"
ifdef CONFIG_POSIX
	$(INSTALL_DIR) "$(DESTDIR)$(mandir)/man1"
	$(INSTALL_DATA) qemu.1 qemu-img.1 "$(DESTDIR)$(mandir)/man1"
	$(INSTALL_DIR) "$(DESTDIR)$(mandir)/man8"
	$(INSTALL_DATA) qemu-nbd.8 "$(DESTDIR)$(mandir)/man8"
endif
ifdef CONFIG_VIRTFS
	$(INSTALL_DIR) "$(DESTDIR)$(mandir)/man1"
	$(INSTALL_DATA) fsdev/virtfs-proxy-helper.1 "$(DESTDIR)$(mandir)/man1"
endif

install-datadir:
	$(INSTALL_DIR) "$(DESTDIR)$(qemu_datadir)"

install-confdir:
	$(INSTALL_DIR) "$(DESTDIR)$(qemu_confdir)"

install-sysconfig: install-datadir install-confdir
	$(INSTALL_DATA) $(SRC_PATH)/sysconfigs/target/target-x86_64.conf "$(DESTDIR)$(qemu_confdir)"
	$(INSTALL_DATA) $(SRC_PATH)/sysconfigs/target/cpus-x86_64.conf "$(DESTDIR)$(qemu_datadir)"

install: all install-sysconfig install-datadir
	$(INSTALL_DIR) "$(DESTDIR)$(bindir)"
ifneq ($(tools-y),)
	$(INSTALL_PROG) $(STRIP_OPT) $(tools-y) "$(DESTDIR)$(bindir)"
endif
ifneq ($(helpers-y),)
	$(INSTALL_DIR) "$(DESTDIR)$(libexecdir)"
	$(INSTALL_PROG) $(STRIP_OPT) $(helpers-y) "$(DESTDIR)$(libexecdir)"
endif
ifneq ($(BLOBS),)
	set -e; for x in $(BLOBS); do \
		$(INSTALL_DATA) $(SRC_PATH)/pc-bios/$$x "$(DESTDIR)$(qemu_datadir)"; \
	done
endif
	$(INSTALL_DIR) "$(DESTDIR)$(qemu_datadir)/keymaps"
	set -e; for x in $(KEYMAPS); do \
		$(INSTALL_DATA) $(SRC_PATH)/pc-bios/keymaps/$$x "$(DESTDIR)$(qemu_datadir)/keymaps"; \
	done
	for d in $(TARGET_DIRS); do \
	$(MAKE) -C $$d $@ || exit 1 ; \
        done

# various test targets
test speed: all
	$(MAKE) -C tests/tcg $@

.PHONY: TAGS
TAGS:
	find "$(SRC_PATH)" -name '*.[hc]' -print0 | xargs -0 etags

cscope:
	rm -f ./cscope.*
	find "$(SRC_PATH)" -name "*.[chsS]" -print | sed 's,^\./,,' > ./cscope.files
	cscope -b

# documentation
MAKEINFO=makeinfo
MAKEINFOFLAGS=--no-headers --no-split --number-sections
TEXIFLAG=$(if $(V),,--quiet)
%.dvi: %.texi
	$(call quiet-command,texi2dvi $(TEXIFLAG) -I . $<,"  GEN   $@")

%.html: %.texi
	$(call quiet-command,LC_ALL=C $(MAKEINFO) $(MAKEINFOFLAGS) --html $< -o $@, \
	"  GEN   $@")

%.info: %.texi
	$(call quiet-command,$(MAKEINFO) $< -o $@,"  GEN   $@")

%.pdf: %.texi
	$(call quiet-command,texi2pdf $(TEXIFLAG) -I . $<,"  GEN   $@")

docs/%.txt: %.hx
	$(call quiet-command,sh $(SRC_PATH)/scripts/hxtool -q < $< > $@,"  GEN   $@")

%.texi: %.hx
	$(call quiet-command,sh $(SRC_PATH)/scripts/hxtool -t < $< > $@,"  GEN   $@")

%.1: %.texi
	$(call quiet-command, \
	  perl -Ww -- $(SRC_PATH)/scripts/texi2pod.pl $< $*.pod && \
	  $(POD2MAN) --section=1 --center=" " --release=" " $*.pod > $@, \
	  "  GEN   $@")

%.8: %.texi
	$(call quiet-command, \
	  perl -Ww -- $(SRC_PATH)/scripts/texi2pod.pl $< $*.pod && \
	  $(POD2MAN) --section=1 --center=" " --release=" " $*.pod > $@, \
	  "  GEN   $@")

docs/qmp-commands.txt: $(SRC_PATH)/qmp-commands.hx
qemu-options.texi: $(SRC_PATH)/qemu-options.hx
hmp-commands.texi: $(SRC_PATH)/hmp-commands.hx
qemu-img-cmds.texi: $(SRC_PATH)/qemu-img-cmds.hx

qemu.1: qemu.texi qemu-options.texi hmp-commands.texi
qemu-img.1: qemu-img.texi qemu-img-cmds.texi
fsdev/virtfs-proxy-helper.1: fsdev/virtfs-proxy-helper.texi
qemu-nbd.8: qemu-nbd.texi

dvi: qemu.dvi qemu-tech.dvi
html: qemu.html qemu-tech.html
info: qemu.info qemu-tech.info
pdf: qemu.pdf qemu-tech.pdf

qemu.dvi qemu.html qemu.info qemu.pdf: \
	qemu-img.texi qemu-nbd.texi qemu-options.texi \
	hmp-commands.texi qemu-img-cmds.texi

VERSION ?= $(shell cat VERSION)
FILE = qemu-$(VERSION)

# tar release (use 'make -k tar' on a checkouted tree)
tar:
	rm -rf /tmp/$(FILE)
	cp -r . /tmp/$(FILE)
	cd /tmp && tar zcvf ~/$(FILE).tar.gz $(FILE) --exclude CVS --exclude .git --exclude .svn
	rm -rf /tmp/$(FILE)

# Add a dependency on the generated files, so that they are always
# rebuilt before other object files
Makefile: $(GENERATED_HEADERS)

# Include automatically generated dependency files
# Dependencies in Makefile.objs files come from our recursive subdir rules
-include $(wildcard *.d tests/*.d)
