# oveRTOS Application Language Support (NuttX Make)
#
# Include after the app's sources.mk.
# Reads APP_LANG from the app's sources.mk (defaults to "c").

APP_LANG ?= c

ifeq ($(APP_LANG),c)
  # Standard C: add app sources to CSRCS
  CSRCS += $(APP_CSRCS)

else ifeq ($(APP_LANG),cpp)
  # C++: NuttX defaults CXXEXT to .cxx; our sources use .cpp.
  # When TFLM is included (OVE_TFLM_SOURCES.mk sets CXXEXT=.cc via ?=),
  # use .cpp here to override and handle .cc files via the TFLM fragment.
  CXXEXT = .cpp
  # C++: add C++ sources to CXXSRCS, C companion sources to CSRCS
  CSRCS += $(APP_CSRCS)
  CXXSRCS += $(APP_CXXSRCS)
  CXXFLAGS += -fno-exceptions -fno-rtti -std=c++20
  # NuttX builds with -nostdinc++ and provides minimal C++ wrappers in
  # include/cxx/ (cmath, cstdio, etc.). For C++20 headers not provided
  # by NuttX (type_traits, atomic, span, string_view, concepts, array, …),
  # add the toolchain's libstdc++ as a lowest-priority fallback via -idirafter.
  # This way NuttX's C wrappers are found first, avoiding conflicts.
  _CXX_SYSROOT := $(shell $(CXX) -print-sysroot)
  _CXX_VER     := $(shell $(CXX) -dumpversion)
  _CXX_MACHINE := $(shell $(CXX) -dumpmachine)
  CXXFLAGS += -idirafter $(_CXX_SYSROOT)/include/c++/$(_CXX_VER)
  CXXFLAGS += -idirafter $(_CXX_SYSROOT)/include/c++/$(_CXX_VER)/$(_CXX_MACHINE)
  # Propagate C include paths to C++ compilation
  CXXFLAGS += $(APP_INCLUDES)
  CXXFLAGS += -I$(OVE_DIR)/include
  CXXFLAGS += -I$(OVE_DIR)/bindings/cpp
  CXXFLAGS += -I$(OVE_DIR)/backends/nuttx/include
  CXXFLAGS += -I$(OVE_DIR)/backends/common
  CXXFLAGS += -I$(OVE_GEN_DIR)

else ifeq ($(APP_LANG),rust)
  # Rust: delegate to cargo-based build
  # Add C companion sources (e.g. benchmark harness) to CSRCS
  CSRCS += $(APP_CSRCS)
  -include $(OVE_DIR)/config/make/ove_rust.mk

else ifeq ($(APP_LANG),zig)
  # Zig: delegate to zig build-lib
  # Add C companion sources to CSRCS
  CSRCS += $(APP_CSRCS)
  -include $(OVE_DIR)/config/make/ove_zig.mk

endif
