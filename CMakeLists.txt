cmake_minimum_required(VERSION 3.20)
project(logos-blockchain-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COPY_PLUGIN_TO_SOURCE_DIR "Copy built plugin to repo root (dev convenience)" ON)
option(LOGOS_BUILD_DEPS_BY_DEFAULT "Always stage+build Rust deps when building the plugin" OFF)

set(LOGOS_CPP_SDK_ROOT "" CACHE PATH "Path to logos-cpp-sdk root")
set(LOGOS_BLOCKCHAIN_ROOT "" CACHE PATH "Path to logos-blockchain source root")

if(LOGOS_CPP_SDK_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_CPP_SDK_ROOT not set. Pass -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk")
endif()
if(LOGOS_BLOCKCHAIN_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_BLOCKCHAIN_ROOT not set. Pass -DLOGOS_BLOCKCHAIN_ROOT=/path/to/logos-blockchain")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---- writable stage (Nix store is read-only) ----
set(LOGOS_BLOCKCHAIN_WORKDIR     "${CMAKE_BINARY_DIR}/logos_blockchain_src")
set(LOGOS_BLOCKCHAIN_STAGE_STAMP "${LOGOS_BLOCKCHAIN_WORKDIR}/.staged")

add_custom_command(
        OUTPUT  "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory  "${LOGOS_BLOCKCHAIN_ROOT}" "${LOGOS_BLOCKCHAIN_WORKDIR}"
        COMMAND ${CMAKE_COMMAND} -E touch "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        VERBATIM
)
add_custom_target(logos_blockchain_stage DEPENDS "${LOGOS_BLOCKCHAIN_STAGE_STAMP}")

# ---- build + stage c-bindings (Rust) ----
set(LOGOS_INSTALL_DIR "${CMAKE_BINARY_DIR}/logos_stage")
file(MAKE_DIRECTORY "${LOGOS_INSTALL_DIR}/include" "${LOGOS_INSTALL_DIR}/lib")

set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")

if(APPLE)
  set(LOGOS_BLOCKCHAIN_EXT ".dylib")
elseif(WIN32)
  set(LOGOS_BLOCKCHAIN_EXT ".dll")
else()
  set(LOGOS_BLOCKCHAIN_EXT ".so")
endif()

set(LOGOS_BLOCKCHAIN_C_SOURCE_DYLIB_NAME "liblib_logos_blockchain${LOGOS_BLOCKCHAIN_EXT}" CACHE STRING "Filename produced by cargo")
set(LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME "lib_logos_blockchain${LOGOS_BLOCKCHAIN_EXT}"    CACHE STRING "Filename staged next to plugin")
set(LOGOS_BLOCKCHAIN_C_HEADER_NAME       "lib_logos_blockchain.h"                         CACHE STRING "Header in c-bindings crate root")

set(LOGOS_BLOCKCHAIN_STAGED_DYLIB  "${LOGOS_INSTALL_DIR}/lib/${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}")
set(LOGOS_BLOCKCHAIN_STAGED_HEADER "${LOGOS_INSTALL_DIR}/include/${LOGOS_BLOCKCHAIN_C_HEADER_NAME}")

set(PATCHELF_COMMANDS "")
if(UNIX AND NOT APPLE AND NOT WIN32)
  find_program(PATCHELF_EXE patchelf REQUIRED)
  set(PATCHELF_COMMANDS
          COMMAND ${PATCHELF_EXE} --set-soname "${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}" "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
  )
endif()

add_custom_command(
        OUTPUT "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CARGO_TARGET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TERM_COLOR=always
        CARGO_TARGET_DIR=${CARGO_TARGET_DIR}
        cargo build --release --package logos-blockchain-c
        COMMAND ${CMAKE_COMMAND} -E copy
        "${CARGO_TARGET_DIR}/release/${LOGOS_BLOCKCHAIN_C_SOURCE_DYLIB_NAME}"
        "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        ${PATCHELF_COMMANDS}
        DEPENDS "${LOGOS_BLOCKCHAIN_STAGE_STAMP}"
        WORKING_DIRECTORY "${LOGOS_BLOCKCHAIN_WORKDIR}"
        VERBATIM
)

add_custom_command(
        OUTPUT "${LOGOS_BLOCKCHAIN_STAGED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LOGOS_BLOCKCHAIN_WORKDIR}/c-bindings/${LOGOS_BLOCKCHAIN_C_HEADER_NAME}"
        "${LOGOS_BLOCKCHAIN_STAGED_HEADER}"
        DEPENDS "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        VERBATIM
)

add_custom_target(logos_cargo_build DEPENDS "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}" "${LOGOS_BLOCKCHAIN_STAGED_HEADER}")

add_library(logos_blockchain_c SHARED IMPORTED GLOBAL)
set_target_properties(logos_blockchain_c PROPERTIES
        IMPORTED_LOCATION             "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_INSTALL_DIR}/include"
)
add_dependencies(logos_blockchain_c logos_cargo_build)

# ---- logos-cpp-sdk ----
add_library(logos_cpp_sdk INTERFACE)
target_include_directories(logos_cpp_sdk INTERFACE
        "${LOGOS_CPP_SDK_ROOT}/include/core"
        "${LOGOS_CPP_SDK_ROOT}/include/cpp"
        "${LOGOS_CPP_SDK_ROOT}/include"
)

add_library(logos_core STATIC IMPORTED)
set_target_properties(logos_core PROPERTIES
        IMPORTED_LOCATION "${LOGOS_CPP_SDK_ROOT}/lib/liblogos_sdk.a"
)

# ---- plugin ----
set(MODULE_ID "liblogos-blockchain-module")
set(PLUGIN_TARGET "${MODULE_ID}")

qt_add_plugin(${PLUGIN_TARGET} CLASS_NAME LogosBlockchainModule)

# Keep the filename expected by the viewer/tooling (if you still want it this way)
set_target_properties(${PLUGIN_TARGET} PROPERTIES
        OUTPUT_NAME "logos-blockchain-module"
)

target_sources(${PLUGIN_TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/library.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
)

target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        logos_blockchain_c
        logos_cpp_sdk
        logos_core
)

target_compile_definitions(${PLUGIN_TARGET} PRIVATE USING_QT)

if(LOGOS_BUILD_DEPS_BY_DEFAULT)
  add_dependencies(${PLUGIN_TARGET} logos_blockchain_stage logos_cargo_build)
endif()

if(APPLE)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES BUILD_RPATH "$ORIGIN"      INSTALL_RPATH "$ORIGIN")
endif()

add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        "$<TARGET_FILE_DIR:${PLUGIN_TARGET}>/${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}"
        VERBATIM
)

if(COPY_PLUGIN_TO_SOURCE_DIR)
  add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:${PLUGIN_TARGET}>"
          "${CMAKE_SOURCE_DIR}/$<TARGET_FILE_NAME:${PLUGIN_TARGET}>"
          VERBATIM
  )
endif()

install(TARGETS ${PLUGIN_TARGET}
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
)

install(FILES library.h DESTINATION include)

install(FILES "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        DESTINATION lib
        RENAME "${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}"
)
