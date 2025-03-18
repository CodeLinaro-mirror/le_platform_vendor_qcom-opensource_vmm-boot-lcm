project(vmm-boot-lcm)

cmake_minimum_required(VERSION 3.10)
find_package(PkgConfig)
pkg_check_modules(GLIB REQUIRED glib-2.0)

add_definitions(-Dstrlcpy=g_strlcpy)
add_definitions(-Dstrlcat=g_strlcat)

add_definitions(-DUSE_GLIB)
add_definitions(-DVMM_LOG_TAG="VMM-BOOT-LCM")
add_definitions(-DVMM_LOG_OUTPUT_LEVEL=VMM_LOG_LEVEL_DBG)

set(SOURCE_FILES
  src/vmm-boot-lcm.cpp
)

include_directories (${SYSROOTINC_PATH})
include_directories (${SYSROOT_INCLUDEDIR})
include_directories (${GLIB_INCLUDE_DIRS})

link_directories(${SYSROOT_LIBDIR})
link_directories(${CMAKE_INSTALL_LIBDIR})

add_executable (vmm-boot-lcm ${SOURCE_FILES})

SET(CMAKE_INSTALL_PREFIX "/usr/")

#links
target_link_libraries (vmm-boot-lcm glib-2.0)
target_link_libraries (vmm-boot-lcm vmm_client)
target_link_libraries (vmm-boot-lcm abctl)

install(TARGETS vmm-boot-lcm DESTINATION bin)
