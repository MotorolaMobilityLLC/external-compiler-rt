SubDirs := lib

# Set default rule before anything else.
all::

include make/config.mk
include make/util.mk
# If SRCROOT is defined, assume we are doing an Apple style build. We should be
# able to use RC_XBS for this but that is unused during "make installsrc".
ifdef SRCROOT
  include make/AppleBI.mk
endif

# Make sure we don't build with a missing ProjObjRoot.
ifeq ($(ProjObjRoot),)
$(error Refusing to build with empty ProjObjRoot variable)
endif

##############

###
# Rules

###
# Top level targets

# FIXME: Document the available subtargets.
help:
	@echo "usage: make [{VARIABLE=VALUE}*] target"
	@echo
	@echo "User variables:"
	@echo "  VERBOSE=1: Use to show all commands [default=0]"
	@echo
	@echo "Available targets:"
	@echo "  clean: clean all configurations"
	@echo "  test:  run unit tests"
	@echo "  all:   build all configurations"
	@echo

help-devel: help
	@echo "Development targets:"
	@echo "  info-functions: list available compiler-rt functions"
	@echo

help-hidden: help-devel
	@echo "Debugging variables:"
	@echo "  DEBUGMAKE=1: enable some Makefile logging [default=]"
	@echo "           =2: enable more Makefile logging"
	@echo
	@echo "Debugging targets:"
	@echo "  make-print-FOO: print information on the variable 'FOO'"
	@echo

info-functions:
	@echo "compiler-rt Available Functions"
	@echo
	@echo "All Functions: $(AvailableFunctions)"
	@$(foreach fn,$(AvailableFunctions),\
	  printf "  %-20s - available in (%s)\n" $(fn)\
	    "$(foreach key,$(AvailableIn.$(fn)),$($(key).Dir))";)

# Provide default clean target which is extended by other templates.
.PHONY: clean
clean::

# Test
.PHONY: test
test:
	cd test/Unit && ./test

# Template: Config_template Config
#
# This template is used once per Config at the top-level.
define Config_template
$(call Set,ActiveConfig,$1)
$(call Set,ActiveObjPath,$(ProjObjRoot)/$(ActiveConfig))
$(call Set,ActiveLibGen,$(ActiveObjPath)/libcompiler_rt.Generic.a)
$(call Set,ActiveLibOpt,$(ActiveObjPath)/libcompiler_rt.Optimized.a)

# The sublibraries to use for a generic version.
$(call Set,GenericInputs,$(foreach arch,$(TargetArchs),$(ActiveObjPath)/$(arch)/libcompiler_rt.Generic.a))
# The sublibraries to use for an optimized version.
$(call Set,OptimizedInputs,$(foreach arch,$(TargetArchs),$(ActiveObjPath)/$(arch)/libcompiler_rt.Optimized.a))

# Provide top-level fat archive targets. We make sure to not try to lipo if only
# building one target arch.
$(ActiveLibGen): $(GenericInputs) $(ActiveObjPath)/.dir
	$(Summary) "  UNIVERSAL: $(ActiveConfig): $$@"
	-$(Verb) $(RM) $$@
	$(if $(TargetArch), \
	  $(Verb) $(CP) $(GenericInputs) $$@, \
	  $(Verb) $(Lipo) -create -output $$@ $(GenericInputs))
$(ActiveLibOpt): $(OptimizedInputs) $(ActiveObjPath)/.dir
	$(Summary) "  UNIVERSAL: $(ActiveConfig): $$@"
	-$(Verb) $(RM) $$@
	$(if $(TargetArch), \
	  $(Verb) $(CP) $(GenericInputs) $$@, \
	  $(Verb) $(Lipo) -create -output $$@ $(OptimizedInputs))
.PRECIOUS: $(ActiveObjPath)/.dir

# Add to default "alias" target.
$(ActiveConfig):: $(ActiveLibGen) $(ActiveLibOpt)

# Add to target lists.
all:: $(ActiveConfig) $(ActiveLibGen) $(ActiveLibOpt)

# Remove entire config directory on clean.
clean:: $(ActiveObjPath)/.remove
endef

# Template: CNA_template Config Arch
#
# This template is used once per Config/Arch at the top-level.
define CNA_template
$(call Set,ActiveConfig,$1)
$(call Set,ActiveArch,$2)
$(call Set,ActiveObjPath,$(ProjObjRoot)/$(ActiveConfig)/$(ActiveArch))
$(call Set,ActiveLibGen,$(ActiveObjPath)/libcompiler_rt.Generic.a)
$(call Set,ActiveLibOpt,$(ActiveObjPath)/libcompiler_rt.Optimized.a)

# Initialize inputs lists. This are extended by the CNA_subdir template. The one
# tricky bit is that we need to use these quoted, because they are not complete
# until the entire makefile has been processed.
$(call Set,GenericInputs.$(ActiveConfig).$(ActiveArch),)
$(call Set,OptimizedInputs.$(ActiveConfig).$(ActiveArch),)
# Final.Inputs is created once we have loaded all the subdirectories
# and know what the correct inputs are.

# Provide top-level archive targets.
$(ActiveLibGen): $(ActiveObjPath)/.dir
	$(Summary) "  ARCHIVE:   $(ActiveConfig)/$(ActiveArch): $$@"
	-$(Verb) $(RM) $$@
	$(Verb) $(Archive) $$@ $$(Generic.Inputs.$(ActiveConfig).$(ActiveArch))
	$(Verb) $(Ranlib) $$@
# FIXME: The dependency on ActiveLibGen is a hack, this picks up the
# dependencies on the generic inputs.
$(ActiveLibOpt): $(ActiveLibGen) $(ActiveObjPath)/.dir
	$(Summary) "  ARCHIVE:   $(ActiveConfig)/$(ActiveArch): $$@"
	-$(Verb) $(RM) $$@
	$(Verb) $(Archive) $$@ $$(Final.Inputs.$(ActiveConfig).$(ActiveArch))
	$(Verb) $(Ranlib) $$@
.PRECIOUS: $(ActiveObjPath)/.dir

# Provide some default "alias" targets.
$(ActiveConfig):: $(ActiveLibGen) $(ActiveLibOpt)
$(ActiveArch):: $(ActiveLibGen) $(ActiveLibOpt)
$(ActiveConfig)-$(ActiveArch):: $(ActiveLibGen) $(ActiveLibOpt)
endef

$(foreach config,$(Configs), \
  $(foreach arch,$(Archs), \
    $(eval $(call CNA_template,$(config),$(arch)))))

$(foreach config,$(Configs), \
  $(eval $(call Config_template,$(config))))

###
# How to build things.

# Define rules for building on each configuration & architecture. This is not
# exactly obvious, but variables inside the template are being expanded during
# the make processing, so automatic variables must be quoted and normal
# assignment cannot be used.

# Template: CNA_template Config Arch Dir
#   Uses: GetArgs, Dependencies, ObjNames
#
# This template is used once per Config/Arch/Dir.
define CNA_subdir_template
$(call Set,ActiveConfig,$1)
$(call Set,ActiveArch,$2)
$(call Set,ActiveDir,$3)
$(call Set,ActiveSrcPath,$(ProjSrcRoot)/$(ActiveDir))
$(call Set,ActiveObjPath,$(ProjObjRoot)/$(ActiveDir)/$(ActiveConfig)/$(ActiveArch))

$(call Set,ActiveFlags,$(call GetArgs,$(ActiveConfig),$(ActiveArch)))
$(call Set,ActiveObjects,$(ObjNames:%=$(ActiveObjPath)/%))

# Add to the input list for the appropriate library and update the dependency.
$(call Append,$(Target).Inputs.$(ActiveConfig).$(ActiveArch),$(ActiveObjects))
$(ProjObjRoot)/$(ActiveConfig)/$(ActiveArch)/libcompiler_rt.$(Target).a: $(ActiveObjects)

$(ActiveObjPath)/%.o: $(ActiveSrcPath)/%.s $(Dependencies) $(ActiveObjPath)/.dir
	$(Summary) "  ASSEMBLE:  $(ActiveConfig)/$(ActiveArch): $$<"
	$(Verb) $(CC) -c -o $$@ $(ActiveFlags) $$<
.PRECIOUS: $(ActiveObjPath)/.dir

$(ActiveObjPath)/%.o: $(ActiveSrcPath)/%.S $(Dependencies) $(ActiveObjPath)/.dir
	$(Summary) "  ASSEMBLE:  $(ActiveConfig)/$(ActiveArch): $$<"
	$(Verb) $(CC) -c -o $$@ $(ActiveFlags) $$<
.PRECIOUS: $(ActiveObjPath)/.dir

$(ActiveObjPath)/%.o: $(ActiveSrcPath)/%.c $(Dependencies) $(ActiveObjPath)/.dir
	$(Summary) "  COMPILE:   $(ActiveConfig)/$(ActiveArch): $$<"
	$(Verb) $(CC) -c -o $$@ $(ActiveFlags) $$<
.PRECIOUS: $(ActiveObjPath)/.dir

# Remove entire config directory on clean.
clean:: $(ProjObjRoot)/$(ActiveDir)/$(ActiveConfig)/.remove
endef

###
# Directory handling magic.

# Create directories as needed, and timestamp their creation.
%/.dir:
	$(Summary) "  MKDIR:     $*"
	$(Verb) $(MKDIR) $* > /dev/null
	$(Verb) $(DATE) > $@

# Remove directories
%/.remove:
	$(Verb) $(RM) -r $*

###
# Include child makefile fragments

Dir := .
include make/subdir.mk

###
# Determine the actual inputs for an optimized library.

# Template: Final_CNA_template Config Arch
#   Uses: GetArgs, Dependencies, ObjNames
#
# This template is used once per Config/Arch.
define Final_CNA_template
$(call Set,ActiveConfig,$1)
$(call Set,ActiveArch,$2)

$(call Set,Final.Inputs.$(ActiveConfig).$(ActiveArch),\
  $(shell make/filter-inputs \
	          $(Optimized.Inputs.$(ActiveConfig).$(ActiveArch)) \
	          $(Generic.Inputs.$(ActiveConfig).$(ActiveArch))))
endef

$(foreach config,$(Configs), \
  $(foreach arch,$(Archs), \
    $(eval $(call Final_CNA_template,$(config),$(arch)))))

ifneq ($(DEBUGMAKE),)
  $(info MAKE: Done processing Makefile)
  $(info  )
endif

###
# Function Information
#
# FIXME: Factor out.

AvailableObjects := $(sort $(foreach key,$(SubDirKeys),\
	$($(key).ObjNames)))
AvailableFunctions := $(AvailableObjects:%.o=%)

# Compute lists of where each function is available.
$(foreach key,$(SubDirKeys),\
  $(foreach fn,$(subst .o,,$($(key).ObjNames)),\
    $(call Append,AvailableIn.$(fn),$(key))))

