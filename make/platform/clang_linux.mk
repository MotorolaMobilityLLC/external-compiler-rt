Description := Static runtime libraries for clang/Linux.

###

CC := clang
Arch := unknown
Configs :=

# We don't currently have any general purpose way to target architectures other
# than the compiler defaults (because there is no generalized way to invoke
# cross compilers). For now, we just find the target archicture of the compiler
# and only define configurations we know that compiler can generate.
CompilerTargetTriple := $(shell \
	$(CC) -v 2>&1 | grep 'Target:' | cut -d' ' -f2)
ifneq ($(DEBUGMAKE),)
ifeq ($(CompilerTargetTriple),)
$(error "unable to infer compiler target triple for $(CC)")
endif
endif

CompilerTargetArch := $(firstword $(subst -, ,$(CompilerTargetTriple)))

# Only define configs if we detected a linux target.
ifneq ($(findstring -linux-,$(CompilerTargetTriple)),)

# Configurations which just include all the runtime functions.
ifeq ($(CompilerTargetArch),i386)
Configs += full-i386
Arch.full-i386 := i386
endif
ifeq ($(CompilerTargetArch),x86_64)
Configs += full-x86_64
Arch.full-x86_64 := x86_64
endif

endif

###

CFLAGS := -Wall -Werror -O3 -fomit-frame-pointer

CFLAGS.full-i386 := $(CFLAGS) -m32
CFLAGS.full-x86_64 := $(CFLAGS) -m64

FUNCTIONS.full-i386 := $(CommonFunctions) $(ArchFunctions.i386)
FUNCTIONS.full-x86_64 := $(CommonFunctions) $(ArchFunctions.x86_64)

# Always use optimized variants.
OPTIMIZED := 1

# We don't need to use visibility hidden on Linux.
VISIBILITY_HIDDEN := 0
