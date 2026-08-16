cmake_minimum_required(VERSION 3.20)
# Enable C language alongside CXX so that CheckSymbolExists can compile C test
# files when probing C library symbols like listxattr.
project(mutar VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Optional feature detection ─────────────────────────────────────────────────
# Each feature is detected independently.  When found, a MUTAR_HAVE_* compile
# definition is added and (where needed) the matching library is linked.
# When NOT found, all related code, getopt entries, and --help text are
# compiled out via #ifdef guards in the source.

find_package(PkgConfig QUIET)

# -- Extended Attributes (xattr) -----------------------------------------------
# Look for sys/xattr.h (Linux glibc, macOS) and the listxattr/getxattr symbols.
# On Linux these are in glibc; no extra library needed.  On some BSDs you need
# -lattr from the attr package.
include(CheckIncludeFileCXX)
include(CheckSymbolExists)
include(CMakePushCheckState)

cmake_push_check_state(RESET)
check_include_file_cxx("sys/xattr.h" HAVE_SYS_XATTR_H)
if(HAVE_SYS_XATTR_H)
  check_symbol_exists(listxattr "sys/xattr.h" HAVE_LISTXATTR)
  if(HAVE_LISTXATTR)
    set(MUTAR_HAVE_XATTR TRUE)
    set(MUTAR_XATTR_DISABLE_REASON "")
    message(STATUS "mutar: xattr support enabled (sys/xattr.h + listxattr)")
  else()
    set(MUTAR_XATTR_DISABLE_REASON "listxattr not found in sys/xattr.h")
    message(STATUS "mutar: xattr support disabled (${MUTAR_XATTR_DISABLE_REASON})")
  endif()
else()
  # Try attr/xattr.h (older Linux attr package)
  check_include_file_cxx("attr/xattr.h" HAVE_ATTR_XATTR_H)
  if(HAVE_ATTR_XATTR_H)
    find_library(ATTR_LIB attr)
    if(ATTR_LIB)
      set(MUTAR_HAVE_XATTR TRUE)
      set(MUTAR_XATTR_LIB "${ATTR_LIB}")
      set(MUTAR_XATTR_DISABLE_REASON "")
      message(STATUS "mutar: xattr support enabled (attr/xattr.h + libattr)")
    else()
      set(MUTAR_XATTR_DISABLE_REASON "attr/xattr.h found but libattr not found")
      message(STATUS "mutar: xattr support disabled (${MUTAR_XATTR_DISABLE_REASON})")
    endif()
  else()
    set(MUTAR_XATTR_DISABLE_REASON "no xattr headers found (install libc6-dev or libattr-dev)")
    message(STATUS "mutar: xattr support disabled (${MUTAR_XATTR_DISABLE_REASON})")
  endif()
endif()
cmake_pop_check_state()

# -- POSIX ACLs (libacl) -------------------------------------------------------
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBACL QUIET libacl)
endif()
if(LIBACL_FOUND)
  set(MUTAR_HAVE_ACL TRUE)
  set(MUTAR_ACL_DISABLE_REASON "")
  message(STATUS "mutar: ACL support enabled (libacl via pkg-config)")
else()
  cmake_push_check_state(RESET)
  check_include_file_cxx("sys/acl.h" HAVE_SYS_ACL_H)
  if(HAVE_SYS_ACL_H)
    find_library(ACL_LIB acl)
    if(ACL_LIB)
      set(MUTAR_HAVE_ACL TRUE)
      set(MUTAR_ACL_LIB "${ACL_LIB}")
      set(MUTAR_ACL_DISABLE_REASON "")
      message(STATUS "mutar: ACL support enabled (sys/acl.h + libacl)")
    else()
      set(MUTAR_ACL_DISABLE_REASON "sys/acl.h found but libacl not found (install libacl-dev)")
      message(STATUS "mutar: ACL support disabled (${MUTAR_ACL_DISABLE_REASON})")
    endif()
  else()
    set(MUTAR_ACL_DISABLE_REASON "no ACL headers found (install libacl-dev)")
    message(STATUS "mutar: ACL support disabled (${MUTAR_ACL_DISABLE_REASON})")
  endif()
  cmake_pop_check_state()
endif()

# -- SELinux -------------------------------------------------------------------
# Policy: SELinux is NOT supported (no test hardware). Never enable MUTAR_HAVE_SELINUX.
# --selinux / --no-selinux remain accepted at runtime as no-ops with a warning.
set(MUTAR_HAVE_SELINUX FALSE)
set(MUTAR_SELINUX_DISABLE_REASON "unsupported by policy (no SELinux test hardware)")
message(STATUS "mutar: SELinux support disabled (${MUTAR_SELINUX_DISABLE_REASON})")

# ── Build types ────────────────────────────────────────────────────────────────
# Following ubuntu-gdb-max-debug-best-practices.txt:
#   Debug:   -ggdb3 -O0 -gdwarf-5 -fvar-tracking-assignments ...
#   Release: -O2 -DNDEBUG
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

set(DEBUG_FLAGS
  -ggdb3
  -O0
  -gdwarf-5
  -fvar-tracking-assignments
  -fvar-tracking
  -fno-omit-frame-pointer
  -fstack-protector-all
  -pipe
)

set(RELEASE_FLAGS
  -O2
  -DNDEBUG
  -fno-omit-frame-pointer
)

set(WARN_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wformat=2
  -Wno-unused-parameter
)

# ── mutar executable ────────────────────────────────────────────────────────────
add_executable(mutar
  src/mutar.cpp
)

target_include_directories(mutar PRIVATE src)

target_compile_options(mutar PRIVATE
  ${WARN_FLAGS}
  $<$<CONFIG:Debug>:${DEBUG_FLAGS}>
  $<$<CONFIG:Release>:${RELEASE_FLAGS}>
)

# Feature compile definitions — drive #ifdef guards in mutar.cpp / mutar.hpp
if(MUTAR_HAVE_XATTR)
  target_compile_definitions(mutar PRIVATE MUTAR_HAVE_XATTR=1)
  if(MUTAR_XATTR_LIB)
    target_link_libraries(mutar PRIVATE "${MUTAR_XATTR_LIB}")
  endif()
endif()

if(MUTAR_HAVE_ACL)
  target_compile_definitions(mutar PRIVATE MUTAR_HAVE_ACL=1)
  if(LIBACL_FOUND)
    target_link_libraries(mutar PRIVATE ${LIBACL_LIBRARIES})
    target_include_directories(mutar PRIVATE ${LIBACL_INCLUDE_DIRS})
  elseif(MUTAR_ACL_LIB)
    target_link_libraries(mutar PRIVATE "${MUTAR_ACL_LIB}")
  endif()
endif()


# Link nothing extra beyond optional feature libs — pure POSIX + C++23 stdlib
target_link_options(mutar PRIVATE
  $<$<CONFIG:Debug>:-fstack-protector-all>
)

# ── Install ────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS mutar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Tests (CTest) ──────────────────────────────────────────────────────────────
enable_testing()

# Shell test harness (functional / compatibility tests)
add_test(
  NAME mutar_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# PR #172 feature integration tests
add_test(
  NAME mutar_pr172_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_pr172_features.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Sidecar index + seek (Phase 5)
add_test(
  NAME mutar_index_seek_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_index_seek.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Seekable compression (Phase 6)
add_test(
  NAME mutar_seekable_compress_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_seekable_compress.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# --pax-option=delete=KEYWORD (GOAL_NEXT Phase B / G1)
add_test(
  NAME mutar_pax_option_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_pax_option.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Multi-volume -M -L --volno-file -F (GOAL_NEXT Phase C / G2–G4)
add_test(
  NAME mutar_multi_volume_tests
  COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_multi_volume.sh $<TARGET_FILE:mutar>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Boundary / overflow / memory tests binary
# Compiled with AddressSanitizer + UndefinedBehaviorSanitizer so any OOB
# access, integer overflow, use-after-free, etc. are caught at runtime.
add_executable(mutar_boundary_tests
  tests/boundary_tests.cpp
)

target_include_directories(mutar_boundary_tests PRIVATE src)

# Always build with debug info + sanitizers regardless of CMAKE_BUILD_TYPE
target_compile_options(mutar_boundary_tests PRIVATE
  -std=c++23
  -ggdb3
  -O1                          # O1 keeps optimisations that expose UB
  -gdwarf-5
  -fno-omit-frame-pointer
  -fsanitize=address,undefined # catch OOB, overflow, use-after-free
  -fno-sanitize-recover=all    # treat every sanitizer hit as fatal
  -Wno-unused-result           # test helpers intentionally ignore write()/system()
  ${WARN_FLAGS}
)

target_link_options(mutar_boundary_tests PRIVATE
  -fsanitize=address,undefined
)

# CTest entry: runs the boundary test binary (with ASAN/UBSAN env tuning)
add_test(
  NAME mutar_boundary_tests
  COMMAND ${CMAKE_BINARY_DIR}/mutar_boundary_tests
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(mutar_boundary_tests PROPERTIES
  ENVIRONMENT
    "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:print_stats=1;\
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
)

# ── Summary ────────────────────────────────────────────────────────────────────
message(STATUS "mutar: C++23 GNU tar-compatible archiver")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(MUTAR_HAVE_XATTR)
  message(STATUS "  xattr      : YES")
else()
  message(STATUS "  xattr      : NO (${MUTAR_XATTR_DISABLE_REASON})")
endif()
if(MUTAR_HAVE_ACL)
  message(STATUS "  ACL        : YES")
else()
  message(STATUS "  ACL        : NO (${MUTAR_ACL_DISABLE_REASON})")
endif()
message(STATUS "  SELinux    : NO (${MUTAR_SELINUX_DISABLE_REASON})")
