project(vmm-boot-lcm)

cmake_minimum_required(VERSION 3.10)
find_package(PkgConfig)

include(CheckSymbolExists)

check_symbol_exists(strlcat "string.h" HAVE_STRLCAT)

check_symbol_exists(strlcpy "string.h" HAVE_STRLCPY)

if(NOT HAVE_STRLCAT OR NOT HAVE_STRLCPY)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB glib-2.0)
  if (GLIB_FOUND)
    include_directories(${GLIB_INCLUDE_DIRS})
    link_directories(${GLIB_LIBRARY_DIRS})
    add_definitions(-DUSE_GLIB)
    if(NOT HAVE_STRLCAT)
        add_definitions(-Dstrlcat=g_strlcat)
    endif()
    if(NOT HAVE_STRLCPY)
        add_definitions(-Dstrlcpy=g_strlcpy)
    endif()
  else()
    message(FATAL_ERROR "glib-2.0 is required but not found.")
  endif()
endif()

add_definitions(-DVMM_LOG_TAG="VMM-BOOT-LCM")
add_definitions(-DVMM_LOG_OUTPUT_LEVEL=VMM_LOG_LEVEL_DBG)

set(SOURCE_FILES
  src/vmm-boot-lcm.cpp
)

include_directories (${SYSROOTINC_PATH})
include_directories (${SYSROOT_INCLUDEDIR})

link_directories(${SYSROOT_LIBDIR})
link_directories(${CMAKE_INSTALL_LIBDIR})

add_executable (vmm-boot-lcm ${SOURCE_FILES})

SET(CMAKE_INSTALL_PREFIX "/usr/")

#links
if(NOT HAVE_STRLCAT OR NOT HAVE_STRLCPY)
   target_link_libraries (vmm-boot-lcm glib-2.0)
endif()
target_link_libraries (vmm-boot-lcm vmm_client)
target_link_libraries (vmm-boot-lcm abctl)

install(TARGETS vmm-boot-lcm DESTINATION bin)
