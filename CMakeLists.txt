cmake_minimum_required(VERSION 3.20)
project(logos-blockchain-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Options ----
set(LOGOS_CORE_ROOT "" CACHE PATH "Path to logos-core root directory.")
set(LOGOS_BLOCKCHAIN_ROOT "" CACHE PATH "Path to logos-blockchain source root.")
set(LOGOS_BLOCKCHAIN_LIB "" CACHE PATH "Path to prebuilt logos-blockchain lib.")
set(LOGOS_BLOCKCHAIN_INCLUDE "" CACHE PATH "Path to prebuilt logos-blockchain include.")

set(HAS_LOGOS_CORE_ROOT FALSE)
set(HAS_LOGOS_BLOCKCHAIN_ROOT FALSE)
set(HAS_LOGOS_BLOCKCHAIN_LIB FALSE)
set(HAS_LOGOS_BLOCKCHAIN_INCLUDE FALSE)

if (DEFINED LOGOS_CORE_ROOT AND NOT "${LOGOS_CORE_ROOT}" STREQUAL "")
    set(HAS_LOGOS_CORE_ROOT TRUE)
endif()

if (DEFINED LOGOS_BLOCKCHAIN_ROOT AND NOT "${LOGOS_BLOCKCHAIN_ROOT}" STREQUAL "")
    set(HAS_LOGOS_BLOCKCHAIN_ROOT TRUE)
endif()

if(DEFINED LOGOS_BLOCKCHAIN_LIB AND NOT "${LOGOS_BLOCKCHAIN_LIB}" STREQUAL "")
    set(HAS_LOGOS_BLOCKCHAIN_LIB TRUE)
endif()

if(DEFINED LOGOS_BLOCKCHAIN_INCLUDE AND NOT "${LOGOS_BLOCKCHAIN_INCLUDE}" STREQUAL "")
    set(HAS_LOGOS_BLOCKCHAIN_INCLUDE TRUE)
endif()

if (NOT HAS_LOGOS_CORE_ROOT)
    message(FATAL_ERROR "LOGOS_CORE_ROOT must be set to the logos-core root directory.")
endif()

if(HAS_LOGOS_BLOCKCHAIN_LIB AND HAS_LOGOS_BLOCKCHAIN_INCLUDE AND NOT HAS_LOGOS_BLOCKCHAIN_ROOT)
    message(STATUS "Using prebuilt logos-blockchain.")
    set(LOGOS_BLOCKCHAIN_PREBUILT TRUE)
elseif(NOT HAS_LOGOS_BLOCKCHAIN_LIB AND NOT HAS_LOGOS_BLOCKCHAIN_INCLUDE AND HAS_LOGOS_BLOCKCHAIN_ROOT)
    message(STATUS "Building logos-blockchain from source.")
    set(LOGOS_BLOCKCHAIN_PREBUILT FALSE)
else()
    message(FATAL_ERROR "Either both LOGOS_BLOCKCHAIN_LIB and LOGOS_BLOCKCHAIN_INCLUDE must be set for prebuilt logos-blockchain, or only LOGOS_BLOCKCHAIN_ROOT must be set for building from source.")
endif()

# ---- Qt ----
find_package(Qt6 REQUIRED COMPONENTS Core RemoteObjects)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---- Directories ----
set(WORKSPACE_ROOT  "${CMAKE_BINARY_DIR}/workspace")
file(MAKE_DIRECTORY "${WORKSPACE_ROOT}")

# ---- Logos Core SDK ----
set(SDK_LIB "${LOGOS_CORE_ROOT}/lib/liblogos_sdk.a")
set(SDK_INC "${LOGOS_CORE_ROOT}/include")

# ---- OS Specifics ----
if(APPLE)
    set(DYLIB_EXT ".dylib")
elseif(WIN32)
    set(DYLIB_EXT ".dll")
    set(IMPLIB_EXT ".lib")
else()
    set(DYLIB_EXT ".so")
endif()

# NOTE (Windows):
# Rust cdylib typically produces:
#   - logos_blockchain.dll (runtime)
#   - logos_blockchain.lib (import lib)
# The Windows build hasn't been yet, so adjust accordingly if the DLL is named without the 'lib' prefix.

# ---- Logos Blockchain (build OR consume) ----
if(LOGOS_BLOCKCHAIN_PREBUILT)
    set(LOGOS_BLOCKCHAIN_DYLIB "${LOGOS_BLOCKCHAIN_LIB}/liblogos_blockchain${DYLIB_EXT}")

    if(WIN32)
        set(LOGOS_BLOCKCHAIN_IMPLIB "${LOGOS_BLOCKCHAIN_LIB}/logos_blockchain${IMPLIB_EXT}")
    endif()

    add_custom_target(logos_blockchain_libs)

else()
    find_program(CARGO_EXECUTABLE cargo REQUIRED)

    set(CARGO_TARGET_DIR       "${WORKSPACE_ROOT}/logos-blockchain/target")
    set(INTERNAL_STAGE         "${WORKSPACE_ROOT}/stage")
    set(INTERNAL_STAGE_LIB     "${INTERNAL_STAGE}/lib")
    set(INTERNAL_STAGE_INCLUDE "${INTERNAL_STAGE}/include")
    file(MAKE_DIRECTORY "${CARGO_TARGET_DIR}" "${INTERNAL_STAGE_LIB}" "${INTERNAL_STAGE_INCLUDE}")

    set(LOGOS_BLOCKCHAIN_LIB     "${INTERNAL_STAGE_LIB}")
    set(LOGOS_BLOCKCHAIN_INCLUDE "${INTERNAL_STAGE_INCLUDE}")

    set(LOGOS_BLOCKCHAIN_DYLIB  "${INTERNAL_STAGE_LIB}/liblogos_blockchain${DYLIB_EXT}")
    set(LOGOS_BLOCKCHAIN_HEADER "${INTERNAL_STAGE_INCLUDE}/logos_blockchain.h")

    add_custom_command(
            OUTPUT "${LOGOS_BLOCKCHAIN_DYLIB}"
            COMMAND ${CMAKE_COMMAND} -E env
            CARGO_TARGET_DIR=${CARGO_TARGET_DIR}
            ${CARGO_EXECUTABLE} build --release
            --package logos-blockchain-c
            --manifest-path "${LOGOS_BLOCKCHAIN_ROOT}/Cargo.toml"
            COMMAND ${CMAKE_COMMAND} -E copy
            "${CARGO_TARGET_DIR}/release/liblogos_blockchain${DYLIB_EXT}"
            "${LOGOS_BLOCKCHAIN_DYLIB}"
            DEPENDS "${LOGOS_BLOCKCHAIN_ROOT}/Cargo.toml"
            VERBATIM
    )

    add_custom_command(
            OUTPUT "${LOGOS_BLOCKCHAIN_HEADER}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LOGOS_BLOCKCHAIN_ROOT}/c-bindings/logos_blockchain.h"
            "${LOGOS_BLOCKCHAIN_HEADER}"
            DEPENDS "${LOGOS_BLOCKCHAIN_DYLIB}"
            VERBATIM
    )

    add_custom_target(logos_blockchain_libs DEPENDS "${LOGOS_BLOCKCHAIN_DYLIB}" "${LOGOS_BLOCKCHAIN_HEADER}")
endif()

# ---- Imported targets ----
add_library(logos_blockchain_interface SHARED IMPORTED GLOBAL)
set_target_properties(logos_blockchain_interface PROPERTIES
        IMPORTED_LOCATION "${LOGOS_BLOCKCHAIN_DYLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_BLOCKCHAIN_INCLUDE}"
)

if(NOT LOGOS_BLOCKCHAIN_PREBUILT)
    add_dependencies(logos_blockchain_interface logos_blockchain_libs)
endif()

if(WIN32)
    set_target_properties(logos_blockchain_interface PROPERTIES IMPORTED_IMPLIB "${LOGOS_BLOCKCHAIN_IMPLIB}")
endif()

add_library(logos_core STATIC IMPORTED)
set_target_properties(logos_core PROPERTIES
        IMPORTED_LOCATION "${SDK_LIB}"
)

add_library(logos_cpp_sdk INTERFACE)
target_include_directories(logos_cpp_sdk INTERFACE "${SDK_INC}" "${SDK_INC}/cpp")

# ---- Plugin ----
set(PLUGIN_TARGET logos_blockchain_module)

qt_add_plugin(${PLUGIN_TARGET} CLASS_NAME LogosBlockchainModule)

target_sources(${PLUGIN_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/logos_blockchain_module.cpp
)

set_property(TARGET ${PLUGIN_TARGET} PROPERTY PUBLIC_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/src/i_logos_blockchain_module.h
)

target_include_directories(${PLUGIN_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PLUGIN_TARGET} PRIVATE
        Qt6::Core
        Qt6::RemoteObjects
        logos_blockchain_interface
        logos_cpp_sdk
        logos_core
)

target_compile_definitions(${PLUGIN_TARGET} PRIVATE
        LOGOS_BLOCKCHAIN_MODULE_METADATA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
)

add_dependencies(${PLUGIN_TARGET} logos_blockchain_libs)

if(APPLE)
    set_target_properties(${PLUGIN_TARGET} PROPERTIES
            BUILD_RPATH "@loader_path"
            INSTALL_RPATH "@loader_path"
    )
elseif(UNIX)
    set_target_properties(${PLUGIN_TARGET} PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN"
    )
endif()

# ---- Install ----
install(TARGETS ${PLUGIN_TARGET}
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        PUBLIC_HEADER DESTINATION include
)
install(DIRECTORY "${LOGOS_BLOCKCHAIN_INCLUDE}/" DESTINATION include)
install(FILES "${LOGOS_BLOCKCHAIN_DYLIB}" DESTINATION lib)
