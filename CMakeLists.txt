cmake_minimum_required(VERSION 3.14)
project(LogosBlockchainModulePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# logos_module() handles: Qt/AUTOMOC setup, SDK/module include paths, linking
# libs from EXTERNAL_LIBS into lib/, plugin output naming, RPATH, install rules.
logos_module(
    NAME liblogos_blockchain_module
    SOURCES
        src/i_logos_blockchain_module.h
        src/logos_blockchain_module.h
        src/logos_blockchain_module.cpp
    EXTERNAL_LIBS
        logos_blockchain
)
