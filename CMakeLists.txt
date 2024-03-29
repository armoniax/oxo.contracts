cmake_minimum_required(VERSION 3.5)

project(oxo)

set(VERSION_MAJOR 0)
set(VERSION_MINOR 2)
set(VERSION_PATCH 0)
set(VERSION_SUFFIX develop2)


set(VERSION_FULL "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

if (VERSION_SUFFIX)
   set(VERSION_FULL "${VERSION_FULL}-${VERSION_SUFFIX}")
endif()

add_custom_target(
   evaluate_every_build ALL
   COMMAND ${CMAKE_COMMAND} -DVERSION_FULL=${VERSION_FULL}
                              -DSRC_DIR=${CMAKE_SOURCE_DIR}/version
                              -DDEST_DIR=${CMAKE_BINARY_DIR}/version
                              -P ${CMAKE_SOURCE_DIR}/version/VersionUtils.cmake
   # BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/src/version_impl.cpp
   COMMENT "Updating version metadata..." VERBATIM )
set(CONTRACT_VERSION_FILE ${CMAKE_BINARY_DIR}/version/ContractVersion.cmake)

include(ExternalProject)

find_package(amax.cdt)

message(STATUS "Building oxo v${VERSION_FULL}")

set(AMAX_CDT_VERSION_MIN "1.7")
set(AMAX_CDT_VERSION_SOFT_MAX "1.9")
#set(AMAX_CDT_VERSION_HARD_MAX "")

### Check the version of amax.cdt
set(VERSION_MATCH_ERROR_MSG "")
AMAX_CHECK_VERSION(VERSION_OUTPUT "${AMAX_CDT_VERSION}"
                                   "${AMAX_CDT_VERSION_MIN}"
                                   "${AMAX_CDT_VERSION_SOFT_MAX}"
                                   "${AMAX_CDT_VERSION_HARD_MAX}"
                                   VERSION_MATCH_ERROR_MSG)
if(VERSION_OUTPUT STREQUAL "MATCH")
   message(STATUS "Using amax.cdt version ${AMAX_CDT_VERSION}")
elseif(VERSION_OUTPUT STREQUAL "WARN")
   message(WARNING "Using amax.cdt version ${AMAX_CDT_VERSION} even though it exceeds the maximum supported version of ${AMAX_CDT_VERSION_SOFT_MAX}; continuing with configuration, however build may fail.\nIt is recommended to use amax.cdt version ${AMAX_CDT_VERSION_SOFT_MAX}.x")
else() # INVALID OR MISMATCH
   message(FATAL_ERROR "Found amax.cdt version ${AMAX_CDT_VERSION} but it does not satisfy version requirements: ${VERSION_MATCH_ERROR_MSG}\nPlease use amax.cdt version ${AMAX_CDT_VERSION_SOFT_MAX}.x")
endif(VERSION_OUTPUT STREQUAL "MATCH")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
   set(TEST_BUILD_TYPE "Debug")
   set(CMAKE_BUILD_TYPE "Release")
else()
   set(TEST_BUILD_TYPE ${CMAKE_BUILD_TYPE})
endif()

ExternalProject_Add(
   contracts_project
   SOURCE_DIR ${CMAKE_SOURCE_DIR}/contracts
   BINARY_DIR ${CMAKE_BINARY_DIR}/contracts
   CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${AMAX_CDT_ROOT}/lib/cmake/amax.cdt/AmaxWasmToolchain.cmake
              -DCONTRACT_VERSION_FILE=${CONTRACT_VERSION_FILE}
   DEPENDS evaluate_every_build
   UPDATE_COMMAND ""
   PATCH_COMMAND ""
   TEST_COMMAND ""
   INSTALL_COMMAND ""
   BUILD_ALWAYS 1
)

if (APPLE)
   set(OPENSSL_ROOT "/usr/local/opt/openssl")
elseif (UNIX)
   set(OPENSSL_ROOT "/usr/include/openssl")
endif()
set(SECP256K1_ROOT "/usr/local")

if (APPLE)
   set(OPENSSL_ROOT "/usr/local/opt/openssl")
elseif (UNIX)
   set(OPENSSL_ROOT "/usr/include/openssl")
endif()
set(SECP256K1_ROOT "/usr/local")

string(REPLACE ";" "|" TEST_PREFIX_PATH "${CMAKE_PREFIX_PATH}")
string(REPLACE ";" "|" TEST_FRAMEWORK_PATH "${CMAKE_FRAMEWORK_PATH}")
string(REPLACE ";" "|" TEST_MODULE_PATH "${CMAKE_MODULE_PATH}")

set(BUILD_TESTS FALSE CACHE BOOL "Build unit tests")

if(BUILD_TESTS)
   message(STATUS "Building unit tests.")
   ExternalProject_Add(
     contracts_unit_tests
     LIST_SEPARATOR | # Use the alternate list separator
     CMAKE_ARGS -DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE} -DCMAKE_PREFIX_PATH=${TEST_PREFIX_PATH} -DCMAKE_FRAMEWORK_PATH=${TEST_FRAMEWORK_PATH} -DCMAKE_MODULE_PATH=${TEST_MODULE_PATH} -DAMAX_ROOT=${AMAX_ROOT} -DLLVM_DIR=${LLVM_DIR} -DBOOST_ROOT=${BOOST_ROOT}
     SOURCE_DIR ${CMAKE_SOURCE_DIR}/tests
     BINARY_DIR ${CMAKE_BINARY_DIR}/tests
     BUILD_ALWAYS 1
     TEST_COMMAND   ""
     INSTALL_COMMAND ""
   )
else()
   message(STATUS "Unit tests will not be built. To build unit tests, set BUILD_TESTS to true.")
endif()
