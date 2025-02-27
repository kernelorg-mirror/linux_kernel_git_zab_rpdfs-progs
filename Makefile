# SPDX-License-Identifier: GPL-2.0

CFLAGS := -I. -O2 -ggdb -Wall -Werror -D_FILE_OFFSET_BITS=64 -msse4.2 -fno-strict-aliasing -fno-omit-frame-pointer

# function entrance/exit profiling for uftrace
CFLAGS += -pg

# produce .i and .s
CFLAGS += -save-temps

LDFLAGS := -Wl,--gc-sections -lurcu-common -lurcu -lurcu-cds -lxxhash

# provide dynamic symbol tables for backtrace()
LDFLAGS += -rdynamic

# build all c files in source directories
DIR := cli devd shared shared/lk
SRC := $(foreach d,$(DIR),$(wildcard $(d)/*.c))
OBJ := $(patsubst %.c,%.o,$(SRC))
DEP := $(foreach d,$(DIR),$(wildcard $(d)/*.d))
DGH := shared/generated-trace-inlines.h

# source with main() is linked as a binary
BIN := $(patsubst %.c,%,$(shell grep -l "^int main" $(SRC)))

# make a static library out of everything in shared
LIB := lib/libngnfs.a
LIB_DIR := shared shared/lk
LIB_SRC := $(foreach d,$(LIB_DIR),$(wildcard $(d)/*.c))
LIB_OBJ := $(patsubst %.c,%.o,$(LIB_SRC))
LIB_DEP := $(foreach d,$(LIB_DIR),$(wildcard $(d)/*.d))

# binary names have ngnfs- prefixed
#binname = $(dir $1)ngnfs-$(notdir $1)

#
# check persistent fixed header structs for internal padding with
# specific minimal compilation that avoids pulling in lots of header
# dependencies that tend to trigger false positives.
#
PADCHECK := $(patsubst %.h,%.o.padcheck,$(wildcard shared/format-*.h))

.PHONY: all
all: $(PADCHECK) $(BIN) $(DGH) $(LIB)

ifneq ($(DEP),)
-include $(DEP)
endif

# link binaries with all shared objs, removing unused symbols
PERCENT := %
.SECONDEXPANSION:
$(BIN): %: %.o 	$$(filter $$(dir %)$$(PERCENT),$$(OBJ)) \
		$$(filter shared/$$(PERCENT),$$(OBJ)) \
		$$(filter shared/lk/$$(PERCENT),$$(OBJ))
	gcc $(LDFLAGS) -o $@ $^

#	gcc $(LDFLAGS) -o $(call binname,$@) $^

%.o %.d: %.c $(DGH) Makefile
	gcc $(CFLAGS) -MD -MP -MF $*.d -c $< -o $*.o
	./scripts/sparse.sh -Wbitwise -D__CHECKER__ $(CFLAGS) $<

$(PADCHECK): %.o.padcheck: %.h Makefile
	gcc $(CFLAGS) -Wpadded -c $< -o $@

$(DGH): scripts/generate-trace-events.awk shared/trace-events.txt Makefile
	gawk -f $< < shared/trace-events.txt > $@

$(LIB): $(LIB_OBJ) Makefile
	ar rcs $@ $(LIB_OBJ)
	ranlib $@

.PHONY: clean
clean:
	@rm -f $(BIN) $(OBJ) $(DEP) $(PADCHECK) $(DGH) $(LIB)\
		$(foreach d,$(DIR),$(wildcard $(d)/*.[is])) \
		.sparse.gcc-defines.h .sparse.output

