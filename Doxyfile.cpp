PROJECT_NAME           = "oveRTOS C++ API"
PROJECT_BRIEF          = "C++20 RAII wrappers for the oveRTOS C API"
INPUT                  = bindings/cpp/ove docs-site/doxygen/doxygen-cpp-mainpage.md
USE_MDFILE_AS_MAINPAGE = docs-site/doxygen/doxygen-cpp-mainpage.md
RECURSIVE              = YES
FILE_PATTERNS          = *.hpp
EXTENSION_MAPPING      = hpp=C++
OUTPUT_DIRECTORY       = output/docs/doxygen-cpp
GENERATE_HTML          = YES
GENERATE_XML           = YES
GENERATE_LATEX         = NO
HTML_OUTPUT            = html
EXTRACT_ALL            = NO
EXTRACT_STATIC         = YES
OPTIMIZE_OUTPUT_FOR_C  = NO
SORT_MEMBER_DOCS       = NO
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = YES
# Heap-mode toggles and Zig guards are static (defined in headers, not
# Kconfig); shared with Doxyfile via Doxyfile.shared_predefined.
# CONFIG_OVE_* Kconfig symbols are auto-generated into Doxyfile.predefined
# by scripts/kconfig-doxyfile-gen.py (re-run by `make docs`).  Order
# matters: shared block uses `=`, generator uses `+=` to accumulate.
@INCLUDE               = Doxyfile.shared_predefined
@INCLUDE               = output/docs/Doxyfile.predefined
EXPAND_ONLY_PREDEF     = YES
WARN_IF_UNDOCUMENTED   = YES
WARN_IF_DOC_ERROR      = YES
