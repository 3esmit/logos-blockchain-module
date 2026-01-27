cmake_minimum_required(VERSION 3.20)
project(logos-blockchain-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


set(LOGOS_CORE_ROOT "$ENV{LOGOS_CORE_ROOT}" CACHE PATH "Path to logos-core root")
set(LOGOS_BLOCKCHAIN_ROOT "$ENV{LOGOS_BLOCKCHAIN_ROOT}" CACHE PATH "Path to logos-blockchain source root")

if(LOGOS_CORE_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_CORE_ROOT not set. Pass -DLOGOS_CORE_ROOT=/path/to/logos-core")
endif()
if(LOGOS_BLOCKCHAIN_ROOT STREQUAL "")
  message(FATAL_ERROR "LOGOS_BLOCKCHAIN_ROOT not set. Pass -DLOGOS_BLOCKCHAIN_ROOT=/path/to/logos-blockchain")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---- Directory Structure ----
set(WORKSPACE_ROOT   "${CMAKE_BINARY_DIR}/workspace")
set(INTERNAL_STAGE    "${WORKSPACE_ROOT}/stage")
set(FINAL_TARGET      "${CMAKE_SOURCE_DIR}/target")

# Ensure directories exist
file(MAKE_DIRECTORY "${INTERNAL_STAGE}/include" "${INTERNAL_STAGE}/lib" "${FINAL_TARGET}")

# ---- Internal Staging for C++ SDK (Symlinks) ----
set(SDK_STAGED_LIB "${INTERNAL_STAGE}/lib/liblogos_sdk.a")
if(NOT EXISTS "${SDK_STAGED_LIB}")
    file(CREATE_LINK "${LOGOS_CORE_ROOT}/lib/liblogos_sdk.a" "${SDK_STAGED_LIB}" SYMBOLIC)
    file(CREATE_LINK "${LOGOS_CORE_ROOT}/include/core"      "${INTERNAL_STAGE}/include/core" SYMBOLIC)
    file(CREATE_LINK "${LOGOS_CORE_ROOT}/include/cpp"       "${INTERNAL_STAGE}/include/cpp" SYMBOLIC)
endif()

# ---- Rust Blockchain Build (Zero-Copy) ----
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(CARGO_TARGET_DIR "${WORKSPACE_ROOT}/logos-blockchain/target")

if(APPLE)
  set(LOGOS_BLOCKCHAIN_EXT ".dylib")
elseif(WIN32)
  set(LOGOS_BLOCKCHAIN_EXT ".dll")
else()
  set(LOGOS_BLOCKCHAIN_EXT ".so")
endif()

set(LOGOS_BLOCKCHAIN_C_SOURCE_DYLIB_NAME "liblib_logos_blockchain${LOGOS_BLOCKCHAIN_EXT}")
set(LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME "lib_logos_blockchain${LOGOS_BLOCKCHAIN_EXT}")
set(LOGOS_BLOCKCHAIN_C_HEADER_NAME       "lib_logos_blockchain.h")

set(LOGOS_BLOCKCHAIN_STAGED_DYLIB  "${INTERNAL_STAGE}/lib/${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}")
set(LOGOS_BLOCKCHAIN_STAGED_HEADER "${INTERNAL_STAGE}/include/${LOGOS_BLOCKCHAIN_C_HEADER_NAME}")

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
        cargo build --release --package logos-blockchain-c --manifest-path "${LOGOS_BLOCKCHAIN_ROOT}/Cargo.toml"
        COMMAND ${CMAKE_COMMAND} -E copy
        "${CARGO_TARGET_DIR}/release/${LOGOS_BLOCKCHAIN_C_SOURCE_DYLIB_NAME}"
        "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        ${PATCHELF_COMMANDS}
        DEPENDS "${LOGOS_BLOCKCHAIN_ROOT}/Cargo.toml"
        VERBATIM
)

add_custom_command(
        OUTPUT "${LOGOS_BLOCKCHAIN_STAGED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LOGOS_BLOCKCHAIN_ROOT}/c-bindings/${LOGOS_BLOCKCHAIN_C_HEADER_NAME}"
        "${LOGOS_BLOCKCHAIN_STAGED_HEADER}"
        DEPENDS "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        VERBATIM
)

add_custom_target(logos_blockchain_libs DEPENDS "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}" "${LOGOS_BLOCKCHAIN_STAGED_HEADER}")

add_library(logos_blockchain_interface SHARED IMPORTED GLOBAL)
set_target_properties(logos_blockchain_interface PROPERTIES
        IMPORTED_LOCATION             "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${INTERNAL_STAGE}/include"
)
add_dependencies(logos_blockchain_interface logos_blockchain_libs)

# ---- C++ SDK Target (using staged symlinks) ----
add_library(logos_cpp_sdk INTERFACE)
target_include_directories(logos_cpp_sdk INTERFACE
        "${INTERNAL_STAGE}/include"
)

add_library(logos_core STATIC IMPORTED)
set_target_properties(logos_core PROPERTIES
        IMPORTED_LOCATION "${SDK_STAGED_LIB}"
)

# ---- Plugin ----
set(PLUGIN_TARGET "liblogos-blockchain-module")

qt_add_plugin(${PLUGIN_TARGET} CLASS_NAME LogosBlockchainModule)

set_target_properties(${PLUGIN_TARGET} PROPERTIES
        OUTPUT_NAME "logos-blockchain-module"
        LIBRARY_OUTPUT_DIRECTORY "${FINAL_TARGET}"
)

target_sources(${PLUGIN_TARGET} PRIVATE
        "library.cpp"
        "metadata.json"
)

target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        logos_blockchain_interface
        logos_cpp_sdk
        logos_core
)

target_compile_definitions(${PLUGIN_TARGET} PRIVATE USING_QT)

# Ensure the Rust backend is always built before the plugin
add_dependencies(${PLUGIN_TARGET} logos_blockchain_libs)

# RPATH so the plugin finds the Rust .so in the same folder
if(APPLE)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(${PLUGIN_TARGET} PROPERTIES BUILD_RPATH "$ORIGIN"      INSTALL_RPATH "$ORIGIN")
endif()

# Copy Rust library to final target folder next to the plugin
add_custom_command(TARGET ${PLUGIN_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LOGOS_BLOCKCHAIN_STAGED_DYLIB}"
        "${FINAL_TARGET}/${LOGOS_BLOCKCHAIN_C_STAGED_DYLIB_NAME}"
        VERBATIM
)

# ---- Installation ----
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
