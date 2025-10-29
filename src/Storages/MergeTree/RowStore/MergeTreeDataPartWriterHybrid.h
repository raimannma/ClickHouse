#pragma once

#include <Storages/MergeTree/MergeTreeDataPartWriterWide.h>
#include <Storages/MergeTree/RowStore/RowStoreEncoder.h>
#include <Storages/MergeTree/RowStore/MergeTreeDataPartHybrid.h>

namespace DB
{

/** Writer for hybrid data parts.
  * Extends Wide writer to also write row store data alongside columnar data.
  *
  * Write process:
  * 1. Write columnar data (via parent MergeTreeDataPartWriterWide)
  * 2. For each row, encode it and add to row store page manager
  * 3. When page is full, compress and write to __row_store.bin
  * 4. Build row store index mapping granule_id -> (page_id, offset)
  * 5. Write row store index to __row_store.idx
  */
class MergeTreeDataPartWriterHybrid : public MergeTreeDataPartWriterWide
{
public:
    MergeTreeDataPartWriterHybrid(
        const String & data_part_name_,
        const String & logger_name_,
        const SerializationByName & serializations_,
        MutableDataPartStoragePtr data_part_storage_,
        const MergeTreeIndexGranularityInfo & index_granularity_info_,
        const MergeTreeSettingsPtr & storage_settings_,
        const NamesAndTypesList & columns_list,
        const StorageMetadataPtr & metadata_snapshot_,
        const VirtualsDescriptionPtr & virtual_columns_,
        const std::vector<MergeTreeIndexPtr> & indices_to_recalc,
        const ColumnsStatistics & stats_to_recalc_,
        const String & marks_file_extension_,
        const CompressionCodecPtr & default_codec_,
        const MergeTreeWriterSettings & writer_settings_,
        MergeTreeIndexGranularityPtr index_granularity_ptr_);

    ~MergeTreeDataPartWriterHybrid() override;

    /// Write a block of data
    void write(const Block & block, const IColumnPermutation * permutation) override;

private:
    /// Row store components
    std::unique_ptr<RowStoreEncoder> row_store_encoder;
    std::unique_ptr<RowStorePageManager> page_manager;
    RowStoreIndex row_store_index;

    /// Settings
    RowStoreEncoder::RowStoreSettings row_store_settings;

    /// Output streams for row store
    std::unique_ptr<WriteBuffer> row_store_file;
    std::unique_ptr<WriteBuffer> row_store_index_file;

    /// Current granule counter
    size_t current_granule = 0;

    /// Initialize row store components
    void initRowStore();

    /// Write a single row to row store
    void writeRowToRowStore(const Block & block, size_t row_index);

    /// Finalize row store writing
    void finalizeRowStore();

    /// Write row store index
    void writeRowStoreIndex();
};

}

