# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Define paths for RocksDB sources and build directories
set(ROCKSDB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/rocksdb")
set(ROCKSDB_BUILD_DIR "${CMAKE_BINARY_DIR}/rocksdb_build")

# Specify output for RocksDB libraries and include directories
set(ROCKSDB_INCLUDE_DIR "${ROCKSDB_SOURCE_DIR}/include")
set(ROCKSDB_LIBRARY "${ROCKSDB_BUILD_DIR}/librocksdb.a")

# Create a custom command to build RocksDB separately and copy the library to the build directory
add_custom_command(
    OUTPUT ${ROCKSDB_LIBRARY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROCKSDB_BUILD_DIR}
    COMMAND USE_RTTI=1 ROCKSDB_DISABLE_SNAPPY=1 ROCKSDB_DISABLE_ZLIB=1 ROCKSDB_DISABLE_BZIP=1 ROCKSDB_DISABLE_LZ4=1 ROCKSDB_DISABLE_ZSTD=1 make static_lib -C ${ROCKSDB_SOURCE_DIR} -j$$(nproc)
    COMMAND ${CMAKE_COMMAND} -E copy ${ROCKSDB_SOURCE_DIR}/librocksdb.a ${ROCKSDB_BUILD_DIR}/librocksdb.a
    WORKING_DIRECTORY ${ROCKSDB_SOURCE_DIR}
    COMMENT "Building and copying RocksDB static library"
)

# Add a custom target for RocksDB that depends on the library
add_custom_target(build_rocksdb ALL
    DEPENDS ${ROCKSDB_LIBRARY}
)

# Define RocksDB as an imported target to be used in the rest of the project
add_library(RocksDB::RocksDB STATIC IMPORTED GLOBAL)
set_target_properties(RocksDB::RocksDB PROPERTIES
    IMPORTED_LOCATION ${ROCKSDB_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${ROCKSDB_INCLUDE_DIR}
)

# Ensure that the RocksDB target is built before the rest of the project
add_dependencies(RocksDB::RocksDB build_rocksdb)
