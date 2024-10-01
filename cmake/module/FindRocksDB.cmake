# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Find the RocksDB headers and libraries
find_path(ROCKSDB_INCLUDE_DIR NAMES rocksdb/db.h)
find_library(ROCKSDB_LIBRARY NAMES rocksdb)

# Standard CMake helper to ensure the libraries and headers are found
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(RocksDB
  REQUIRED_VARS ROCKSDB_LIBRARY ROCKSDB_INCLUDE_DIR
)

# If RocksDB was found and no existing target is defined, define a new imported target
if(ROCKSDB_FOUND AND NOT TARGET RocksDB::RocksDB)
  add_library(RocksDB::RocksDB STATIC IMPORTED)
  set_target_properties(RocksDB::RocksDB PROPERTIES
      IMPORTED_LOCATION "${ROCKSDB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_INCLUDE_DIR}"
  )
endif()

# Mark the include and library paths as advanced to hide them in the CMake GUI by default
mark_as_advanced(
  ROCKSDB_INCLUDE_DIR
  ROCKSDB_LIBRARY
)
