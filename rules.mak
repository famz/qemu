
# Don't use implicit rules or variables
# we have explicit rules for everything
MAKEFLAGS += -rR

# Files with this suffixes are final, don't try to generate them
# using implicit rules
%.d:
%.h:
%.c:
%.cc:
%.cpp:
%.m:
%.mak:

# Flags for C++ compilation
QEMU_CXXFLAGS = -D__STDC_LIMIT_MACROS $(filter-out -Wstrict-prototypes -Wmissing-prototypes -Wnested-externs -Wold-style-declaration -Wold-style-definition -Wredundant-decls, $(QEMU_CFLAGS))

# Flags for dependency generation
QEMU_DGFLAGS += -MMD -MP -MT $@ -MF $(*D)/$(*F).d

# Same as -I$(SRC_PATH) -I., but for the nested source/object directories
QEMU_INCLUDES += -I$(<D) -I$(@D)

maybe-add = $(filter-out $1, $2) $1
extract-libs = $(strip $(sort $(foreach o,$1,$($o-libs))) \
                  $(foreach o,$(call expand-objs,$1),$($o-libs)))
expand-objs = $(strip $(sort $(filter %.o,$1) \
                  $(foreach o,$(filter %.mo,$1),$($o-objs))) \
                  $(filter-out %.o %.mo,$1))

%.o: %.c
	$(call quiet-command,$(CC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) $($@-cflags) -c -o $@ $<,"  CC    $(TARGET_DIR)$@")
%.o: %.rc
	$(call quiet-command,$(WINDRES) -I. -o $@ $<,"  RC    $(TARGET_DIR)$@")

# If we have a CXX we might have some C++ objects, in which case we
# must link with the C++ compiler, not the plain C compiler.
LINKPROG = $(or $(CXX),$(CC))

ifeq ($(LIBTOOL),)
LINK = $(call quiet-command,$(LINKPROG) $(QEMU_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
       $(call expand-objs,$1) $(version-obj-y) \
       $(call extract-libs,$1) $(LIBS),"  LINK  $(TARGET_DIR)$@")
else
LIBTOOL += $(if $(V),,--quiet)
%.lo: %.c
	$(call quiet-command,$(LIBTOOL) --mode=compile --tag=CC $(CC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  lt CC $@")
%.lo: %.rc
	$(call quiet-command,$(LIBTOOL) --mode=compile --tag=RC $(WINDRES) -I. -o $@ $<,"lt RC   $(TARGET_DIR)$@")
%.lo: %.dtrace
	$(call quiet-command,$(LIBTOOL) --mode=compile --tag=CC dtrace -o $@ -G -s $<, " lt GEN $(TARGET_DIR)$@")

LINK = $(call quiet-command,\
       $(if $(filter %.lo %.la,$1),$(LIBTOOL) --mode=link --tag=CC \
       )$(LINKPROG) $(QEMU_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ \
       $(call expand-objs,$1) \
       $(if $(filter %.lo %.la,$1),$(version-lobj-y),$(version-obj-y)) \
       $(if $(filter %.lo %.la,$1),$(LIBTOOLFLAGS)) \
       $(call extract-libs,$1) $(LIBS),$(if $(filter %.lo %.la,$1),"lt LINK ", "  LINK  ")"$(TARGET_DIR)$@")
endif

%.asm: %.S
	$(call quiet-command,$(CPP) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -o $@ $<,"  CPP   $(TARGET_DIR)$@")

%.o: %.asm
	$(call quiet-command,$(AS) $(ASFLAGS) -o $@ $<,"  AS    $(TARGET_DIR)$@")

%.o: %.cc
	$(call quiet-command,$(CXX) $(QEMU_INCLUDES) $(QEMU_CXXFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  CXX   $(TARGET_DIR)$@")

%.o: %.cpp
	$(call quiet-command,$(CXX) $(QEMU_INCLUDES) $(QEMU_CXXFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  CXX   $(TARGET_DIR)$@")

%.o: %.m
	$(call quiet-command,$(OBJCC) $(QEMU_INCLUDES) $(QEMU_CFLAGS) $(QEMU_DGFLAGS) $(CFLAGS) -c -o $@ $<,"  OBJC  $(TARGET_DIR)$@")

%.o: %.dtrace
	$(call quiet-command,dtrace -o $@ -G -s $<, "  GEN   $(TARGET_DIR)$@")

%$(DSOSUF): LDFLAGS += $(LDFLAGS_SHARED)
%$(DSOSUF): %.mo libqemustub.a
	$(call LINK,$^)
	@# Copy to build root so modules can be loaded when program started without install
	$(if $(findstring /,$@),$(call quiet-command,cp $@ $(subst /,-,$@), "  CP    $(subst /,-,$@)"))

.PHONY: modules
modules:

%$(EXESUF): %.o
	$(call LINK,$^)

%.a:
	$(call quiet-command,rm -f $@ && $(AR) rcs $@ $^,"  AR    $(TARGET_DIR)$@")

quiet-command = $(if $(V),$1,$(if $(2),@echo $2 && $1, @$1))

# cc-option
# Usage: CFLAGS+=$(call cc-option, -falign-functions=0, -malign-functions=0)

cc-option = $(if $(shell $(CC) $1 $2 -S -o /dev/null -xc /dev/null \
              >/dev/null 2>&1 && echo OK), $2, $3)

VPATH_SUFFIXES = %.c %.h %.S %.cc %.cpp %.m %.mak %.texi %.sh %.rc
set-vpath = $(if $1,$(foreach PATTERN,$(VPATH_SUFFIXES),$(eval vpath $(PATTERN) $1)))

# find-in-path
# Usage: $(call find-in-path, prog)
# Looks in the PATH if the argument contains no slash, else only considers one
# specific directory.  Returns an # empty string if the program doesn't exist
# there.
find-in-path = $(if $(find-string /, $1), \
        $(wildcard $1), \
        $(wildcard $(patsubst %, %/$1, $(subst :, ,$(PATH)))))

# Logical functions (for operating on y/n values like CONFIG_FOO vars)
# Inputs to these must be either "y" (true) or "n" or "" (both false)
# Output is always either "y" or "n".
# Usage: $(call land,$(CONFIG_FOO),$(CONFIG_BAR))
# Logical NOT
lnot = $(if $(subst n,,$1),n,y)
# Logical AND
land = $(if $(findstring yy,$1$2),y,n)
# Logical OR
lor = $(if $(findstring y,$1$2),y,n)
# Logical XOR (note that this is the inverse of leqv)
lxor = $(if $(filter $(call lnot,$1),$(call lnot,$2)),n,y)
# Logical equivalence (note that leqv "","n" is true)
leqv = $(if $(filter $(call lnot,$1),$(call lnot,$2)),y,n)
# Logical if: like make's $(if) but with an leqv-like test
lif = $(if $(subst n,,$1),$2,$3)

# String testing functions: inputs to these can be any string;
# the output is always either "y" or "n". Leading and trailing whitespace
# is ignored when comparing strings.
# String equality
eq = $(if $(subst $2,,$1)$(subst $1,,$2),n,y)
# String inequality
ne = $(if $(subst $2,,$1)$(subst $1,,$2),y,n)
# Emptiness/non-emptiness tests:
isempty = $(if $1,n,y)
notempty = $(if $1,y,n)

# Generate files with tracetool
TRACETOOL=$(PYTHON) $(SRC_PATH)/scripts/tracetool.py

# Generate timestamp files for .h include files

config-%.h: config-%.h-timestamp
	@cmp $< $@ >/dev/null 2>&1 || cp $< $@

config-%.h-timestamp: config-%.mak
	$(call quiet-command, sh $(SRC_PATH)/scripts/create_config < $< > $@, "  GEN   $(TARGET_DIR)config-$*.h")

.PHONY: clean-timestamp
clean-timestamp:
	rm -f *.timestamp
clean: clean-timestamp

# will delete the target of a rule if commands exit with a nonzero exit status
.DELETE_ON_ERROR:

# save-objs
# Usage: $(call save-objs, vars)
# Save each variable $v to save-objs-$v, and save per object variables
define save-objs
    $(foreach v,$1,
        $(foreach o,$($v),
            $(foreach k,cflags libs objs,
                $(if $($o-$k),
                    $(eval save-objs-$o-$k := $($o-$k))
                    $(eval $o-$k := ))))
        $(eval save-objs-$v := $(filter-out %/,$(value $v)))
        $(eval $v := ))
endef

# load-objs
# Usage: $(call load-objs, vars)
# Load each variable $v from save-objs-$v and append to current value,
# and load the per object variables
define load-objs
    $(foreach v,$1,
        $(foreach o,$(save-objs-$v),
            $(foreach k,cflags libs objs,
                $(if $(save-objs-$o-$k),
                    $(eval $o-$k := $(save-objs-$o-$k))
                    $(eval save-objs-$o-$k := ))))
        $(eval $v := $(value save-objs-$v) $(value $v))
        $(eval save-objs-$v := ))
endef

# fix-paths
# Usage: $(call fix-paths, obj_path, src_path, vars)
# Add prefix obj_path to all objects in vars, and add prefix src_path to all
# directories.
define fix-paths
    $(foreach v,$3,
        $(foreach o,$($v),
            $(if $($o-libs),
                $(eval $1$o-libs := $(value $o-libs)))
            $(if $($o-cflags),
                $(eval $1$o-cflags := $(value $o-cflags)))
            $(if $($o-objs),
                $(eval $1$o-objs := $(addprefix $1,$(value $o-objs)))))
        $(eval $v := $(addprefix $1,$(filter-out %/,$(value $v))) \
                     $(addprefix $2, $(filter %/,$(value $v)))))
endef

# unnest-vars-recursive
# Usage: $(call unnest-vars-recursive, obj_prefix, vars)
define unnest-vars-recursive
    $(eval dirs := $(sort $(filter %/,$(foreach v,$2,$($v)))))
    $(foreach v,$2,
        $(eval $v := $(filter-out %/,$($v))))
    $(foreach d,$(patsubst %/,%,$(dirs)),
        $(call save-objs,$2)
        $(eval obj := $(if $1,$1/)$d)
        $(eval -include $(SRC_PATH)/$d/Makefile.objs)
        $(call fix-paths,$(if $1,$1/)$d/,$d/,$2)
        $(call load-objs,$2)
        $(call unnest-vars-recursive,$1,$2)
    )
endef

# unnest-vars
# Usage: $(call unnest-vars, obj_prefix, vars)
# @obj_prefix: object path prefix, can be empty. Don't include ending '/'
# @vars: the list of variable names to unnest
#
# This macro will scan each variable in @vars, and traverse into subdirectories
# (items ending with a /), to include Makefile.objs.
#
# The subdir Makefile.objs may append more items into a nested var, either
# objects or more subdirectories in next level. Those objects are then added
# into variables by prefixing relative path with @obj_prefix, and those next
# level subdirectories are unnested recursively.
#
# Per object and per module cflags and libs are saved with relative path fixed
# as well.
#
# All nested variables postfixed by -m in names are treated as dynamic loading
# variables, and will be built as modules, if enabled.
#
define unnest-vars
    # In the case of target build (i.e. $1 == ..), fix path for top level
    # Makefile.objs objects
    $(if $1,$(call fix-paths,$1/,,$2))

    # Descend and include every subdir Makefile.objs.
    $(call unnest-vars-recursive,$1,$2)

    # Post-process all the unnested vars
    $(foreach v,$2,
        $(foreach o, $(filter %.mo,$($v)),
            # Find all the .mo objects in variables and add dependency rules
            # according to .mo-objs. Report error if not set
            $(if $($o-objs),
                $(eval $o: $($o-objs)),
                $(error $o added in $v but $o-objs is not set))
            # Pass the .mo-cflags along to member objects
            $(if $($o-cflags),
                $(foreach p,$($o-objs),
                    $(eval $p-cflags := $($o-cflags)))))
        $(shell mkdir -p ./ $(sort $(dir $($v))))
        # Include all the .d files
        $(eval -include $(addsuffix *.d, $(sort $(dir $($v)))))
        $(eval $v := $(filter-out %/,$($v))))

    $(foreach v,$(filter %-m,$2),
        $(foreach o,$(filter %.o,$($v)),
            # Add rule: xxx.mo: xxx.o, for single object modules
            $(eval $(patsubst %.o,%.mo,$o): $o))
        # Add DSO as target of modules
        $(foreach o,$($v),
            $(eval modules: $(basename $o)$(DSOSUF))
            $(eval modules-m += $(basename $o)$(DSOSUF))))
endef
