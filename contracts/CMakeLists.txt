cmake_minimum_required( VERSION 3.5 )

project(contracts)

include(${CONTRACT_VERSION_FILE})
message(STATUS "Contract Version: ${CONTRACT_VERSION}")

set(AMAX_WASM_OLD_BEHAVIOR "Off")
find_package(amax.cdt)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $CACHE{CMAKE_CXX_FLAGS}")

set(ICON_BASE_URL "http://127.0.0.1/ricardian_assets/amax.contracts/icons")

add_subdirectory(otcbook)
add_subdirectory(otcconf)
add_subdirectory(otcsettle)
add_subdirectory(otcfeesplit)
