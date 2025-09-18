# SPDX-License-Identifier: GPL-2.0

# make every target depend on the makefile
.EXTRA_PREREQS:= $(abspath $(lastword $(MAKEFILE_LIST)))

CFLAGS := -I. -O2 -ggdb -Wall -Werror -D_FILE_OFFSET_BITS=64 -msse4.2 -fno-strict-aliasing -fno-omit-frame-pointer

# function entrance/exit profiling for uftrace
CFLAGS += -pg

# produce .i and .s
CFLAGS += -save-temps

LDFLAGS := -Wl,--gc-sections -lxxhash -luring -luuid -lsystemd

# provide dynamic symbol tables for backtrace()
LDFLAGS += -rdynamic

# build all c files in source directories
DIR := cli devd shared shared/lk utask
SRC := $(foreach d,$(DIR),$(wildcard $(d)/*.c))
OBJ := $(patsubst %.c,%.o,$(SRC))
DEP := $(foreach d,$(DIR),$(wildcard $(d)/*.d))
SRC_S := $(foreach d,$(DIR),$(wildcard $(d)/*.S))
OBJ_S := $(patsubst %.S,%.o,$(SRC_S))

# source with main() is linked as a binary
BIN := $(patsubst %.c,%,$(shell grep -l "^int main" $(SRC)))

#
# check persistent fixed header structs for internal padding with
# specific minimal compilation that avoids pulling in lots of header
# dependencies that tend to trigger false positives.
#
PADCHECK := $(patsubst %.h,%.o.padcheck,$(wildcard shared/format-*.h))

#
# We have some standalone binaries that generate source.
#
GEN_BIN := $(patsubst %.c,%,$(wildcard shared/lk/gen/*.c))

.PHONY: all
all: $(PADCHECK) $(GEN_BIN) $(BIN)

ifneq ($(DEP),)
-include $(DEP)
endif

# link binaries with all shared objs, removing unused symbols
PERCENT := %
.SECONDEXPANSION:
$(BIN): %: %.o 	$$(filter $$(dir %)$$(PERCENT),$$(OBJ)) \
		$$(filter shared/$$(PERCENT),$$(OBJ)) \
		$$(filter shared/lk/$$(PERCENT),$$(OBJ)) \
		$$(filter utask/$$(PERCENT),$$(OBJ) $$(OBJ_S))
	gcc $(LDFLAGS) -o $@ $^

$(GEN_BIN): %: %.c
	gcc $(LDFLAGS) -o $@ $<

%.o %.d: %.c
	gcc $(CFLAGS) -MD -MP -MF $*.d -c $< -o $*.o
	./scripts/sparse.sh -Wbitwise -D__CHECKER__ $(CFLAGS) $<

#
# The utask/asm building is a bit sloppy.  Half generic rules, but
# explicit dependencies.
#
$(OBJ_S): %.o: %.S utask/utask_defs.h utask/utask_gen_defs.h
	gcc -c $< -o $*.o

# specifically output compiled assembly so we can extract defines
utask/utask.s: utask/utask.c
	gcc -I. -S -o ./utask/utask.s  ./utask/utask.c

# extract defines from complied asm so we can include it from asm source
utask/utask_gen_defs.h: utask/utask.s
	grep '#define.*\<UTASK_ASM' utask/utask.s > utask/utask_gen_defs.h

$(PADCHECK): %.o.padcheck: %.h
	gcc $(CFLAGS) -Wpadded -c $< -o $@

shared/lk/crc64table.h: shared/lk/gen/gen_crc64table
	$^ > $@

$(LIB): $(LIB_OBJ)
	ar rcs $@ $(LIB_OBJ)
	ranlib $@

.PHONY: clean
clean:
	@rm -f $(BIN) $(OBJ) $(OBJ_S) $(DEP) $(PADCHECK) $(GEN_BIN) \
		$(foreach d,$(DIR),$(wildcard $(d)/*.[is])) \
		utask/utask_gen_defs.h \
		.sparse.gcc-defines.h .sparse.output

