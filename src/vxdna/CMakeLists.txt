# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Advanced Micro Devices, Inc.

# Library version
set(VXDNA_VERSION_MAJOR 1)
set(VXDNA_VERSION_MINOR 0)
set(VXDNA_VERSION_PATCH 0)
set(VXDNA_VERSION "${VXDNA_VERSION_MAJOR}.${VXDNA_VERSION_MINOR}.${VXDNA_VERSION_PATCH}")

# Source files
set(VXDNA_SOURCES
    src/vaccel_manager.c
    src/vaccel_context.c
    src/vaccel_resource.c
    src/vaccel_fence.c
    src/vaccel_drm_backend.c
    util/hash_table_u32.c
    util/hash_table_u64.c
    util/hash_table_ptr.c
    util/os_file.c
)

# Create shared library
add_library(vxdna SHARED ${VXDNA_SOURCES})

# Set library version
set_target_properties(vxdna PROPERTIES
    VERSION ${VXDNA_VERSION}
    SOVERSION ${VXDNA_VERSION_MAJOR}
    OUTPUT_NAME "vxdna"
)

# Include directories
target_include_directories(vxdna PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/util
)

# Compiler flags
target_compile_options(vxdna PRIVATE
    -fPIC
    -Wall
    -Wextra
)

# Link libraries
target_link_libraries(vxdna PRIVATE
    pthread
)

# Install library
install(TARGETS vxdna
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT ${XDNA_COMPONENT}
)

# Install headers
install(FILES
    include/vaccel_renderer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT ${XDNA_COMPONENT}
)

# Optional: Build example program
option(BUILD_VXDNA_EXAMPLE "Build vxdna example program" OFF)

if(BUILD_VXDNA_EXAMPLE)
    add_executable(vxdna_example example/example.c)
    target_link_libraries(vxdna_example PRIVATE vxdna)
    target_include_directories(vxdna_example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    
    # Install example (optional)
    install(TARGETS vxdna_example
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${XDNA_COMPONENT}
        OPTIONAL
    )
endif()

