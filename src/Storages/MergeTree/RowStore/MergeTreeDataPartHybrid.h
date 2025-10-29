#pragma once

#include <Storages/MergeTree/MergeTreeDataPartWide.h>
#include <Storages/MergeTree/RowStore/RowStoreEncoder.h>

namespace DB
{

/** Hybrid data part format that combines:
  * 1. Wide format: Each column in separate files (for analytical queries)
  * 2. Row store: Hidden column with binary-encoded full rows (for point queries)
  *
  * File structure:
  * - [Column].bin, [Column].mrk - regular columnar files (same as Wide format)
  * - __row_store.bin - row store data (pages of binary-encoded rows)
  * - __row_store.mrk - row store marks
  * - __row_store.idx - row store index (maps granule_id to page_id/offset)
  * - primary.idx - primary index (extended with row store metadata)
  * - checksums.txt, columns.txt - metadata files
  *
  * This format is used when enable_row_store = 1 in table settings.
  */
class MergeTreeDataPartHybrid : public MergeTreeDataPartWide
{
public:
    static constexpr auto ROW_STORE_COLUMN_NAME = "__row_store";
    static constexpr auto ROW_STORE_INDEX_NAME = "__row_store.idx";
    
    MergeTreeDataPartHybrid(
        const MergeTreeData & storage_,
        const String & name_,
        const MergeTreePartInfo & info_,
        const MutableDataPartStoragePtr & data_part_storage_,
        const IMergeTreeDataPart * parent_part_ = nullptr);
    
    ~MergeTreeDataPartHybrid() override;
    
    /// Check if row store is available
    bool hasRowStore() const;
    
    /// Load row store index into memory
    void loadRowStoreIndex();
    
    /// Get row store index
    const RowStoreIndex & getRowStoreIndex() const { return row_store_index; }
    
    /// Read a single row from row store by granule id
    Block readRowFromRowStore(size_t granule_id, const NamesAndTypesList & columns) const;
    
    /// Get row store statistics
    struct RowStoreStatistics
    {
        size_t total_pages = 0;
        size_t total_rows = 0;
        size_t uncompressed_bytes = 0;
        size_t compressed_bytes = 0;
        double compression_ratio = 0.0;
    };
    
    RowStoreStatistics getRowStoreStatistics() const;

protected:
    /// Row store index (lazy loaded)
    mutable std::mutex row_store_mutex;
    mutable RowStoreIndex row_store_index;
    mutable bool row_store_index_loaded = false;
    
    /// Helper to ensure row store index is loaded
    void ensureRowStoreIndexLoaded() const;
};

using MergeTreeDataPartHybridPtr = std::shared_ptr<MergeTreeDataPartHybrid>;

}

