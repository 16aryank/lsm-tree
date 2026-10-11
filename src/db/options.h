#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "memtable/memtable.h"
#include "sstable/sstable_format.h"
#include "version/version_set.h"

// Configuration for opening and writing a database.
struct Options {
    // Required database directory.
    std::filesystem::path dir;

    // How large a memtable grows before it is frozen and flushed.
    size_t memtable_size_bytes = kDefaultMemTableSizeBytes;

    // Data block size for tables written by a flush or a compaction.
    size_t sstable_block_size = kSSTableBlockSize;

    // Threshold for writing a fresh manifest snapshot.
    uint64_t max_manifest_file_size = kDefaultMaxManifestFileSize;

    // Create a database when the directory has no CURRENT file.
    bool create_if_missing = true;

    // Refuse to open a directory that already holds a database.
    bool error_if_exists = false;
};
