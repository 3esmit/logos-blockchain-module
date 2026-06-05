cmake_minimum_required(VERSION 3.14)
project(LogosBlockchainModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Universal module — generated_code/ is picked up automatically by LogosModule.cmake
logos_module(
    NAME liblogos_blockchain_module
    SOURCES
        src/logos_blockchain_module.h
        src/logos_blockchain_module.cpp
    EXTERNAL_LIBS
        logos_blockchain
)
