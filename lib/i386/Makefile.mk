#===- lib/i386/Makefile.mk ---------------------------------*- Makefile -*--===#
#
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#

Dir := lib/i386
SubDirs := 
OnlyArchs := i386

AsmSources := $(foreach file,$(wildcard $(Dir)/*.S),$(notdir $(file)))
Sources := $(foreach file,$(wildcard $(Dir)/*.c),$(notdir $(file)))
ObjNames := $(Sources:%.c=%.o) $(AsmSources:%.S=%.o)
Target := Optimized

# FIXME: use automatic dependencies?
Dependencies := $(wildcard $(Dir)/*.h)

include make/subdir.mk
