cmake_minimum_required(VERSION 3.14)
project(BlockchainModuleTests LANGUAGES CXX)

include(LogosTest)

# Unit tests (mocked logos_blockchain)

logos_test(
    NAME blockchain_module_tests
    MODULE_SOURCES
        ../src/logos_blockchain_module.cpp
    TEST_SOURCES
        main.cpp
        test_blockchain.cpp
    MOCK_C_SOURCES
        mocks/mock_logos_blockchain.cpp
    EXTRA_INCLUDES
        stubs
)

# Integration tests (real logos_blockchain library)

find_library(LIBLOGOS_BLOCKCHAIN_PATH
    NAMES liblogos_blockchain.so liblogos_blockchain.dylib
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    NO_DEFAULT_PATH)

if(LIBLOGOS_BLOCKCHAIN_PATH)
    message(STATUS "[BlockchainTests] logos_blockchain found: ${LIBLOGOS_BLOCKCHAIN_PATH} - building integration tests")

    logos_test(
        NAME blockchain_module_integration_tests
        MODULE_SOURCES
            ../src/logos_blockchain_module.cpp
        TEST_SOURCES
            main.cpp
            test_blockchain.cpp
        EXTRA_INCLUDES
            ../lib
        EXTRA_LINK_LIBS
            ${LIBLOGOS_BLOCKCHAIN_PATH}
    )

    get_filename_component(LIBLOGOS_BLOCKCHAIN_DIR "${LIBLOGOS_BLOCKCHAIN_PATH}" DIRECTORY)
    set_target_properties(blockchain_module_integration_tests PROPERTIES
        BUILD_RPATH "${LIBLOGOS_BLOCKCHAIN_DIR}"
    )
else()
    message(STATUS "[BlockchainTests] logos_blockchain not found in ../lib - skipping integration tests")
endif()
