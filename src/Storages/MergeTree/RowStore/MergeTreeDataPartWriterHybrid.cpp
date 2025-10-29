#include <Storages/MergeTree/RowStore/MergeTreeDataPartWriterHybrid.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <IO/WriteBufferFromFileBase.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

MergeTreeDataPartWriterHybrid::MergeTreeDataPartWriterHybrid(
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
    MergeTreeIndexGranularityPtr index_granularity_ptr_)
    : MergeTreeDataPartWriterWide(
        data_part_name_,
        logger_name_,
        serializations_,
        data_part_storage_,
        index_granularity_info_,
        storage_settings_,
        columns_list,
        metadata_snapshot_,
        virtual_columns_,
        indices_to_recalc,
        stats_to_recalc_,
        marks_file_extension_,
        default_codec_,
        writer_settings_,
        std::move(index_granularity_ptr_))
{
    initRowStore();
}

MergeTreeDataPartWriterHybrid::~MergeTreeDataPartWriterHybrid()
{
    try
    {
        // Finalize row store if not already done
        if (row_store_file || row_store_index_file)
            finalizeRowStore();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

void MergeTreeDataPartWriterHybrid::initRowStore()
{
    // Get row store settings from storage settings
    // For now, use defaults
    row_store_settings.page_size = RowStoreEncoder::DEFAULT_PAGE_SIZE;
    row_store_settings.compression_codec = "LZ4";
    row_store_settings.enable_checksums = true;

    // Initialize encoder and page manager
    row_store_encoder = std::make_unique<RowStoreEncoder>(columns_list, row_store_settings);
    page_manager = std::make_unique<RowStorePageManager>(row_store_settings);

    // Open row store files
    row_store_file = getDataPartStorage().writeFile(
        String(MergeTreeDataPartHybrid::ROW_STORE_COLUMN_NAME) + ".bin",
        DBMS_DEFAULT_BUFFER_SIZE,
        settings.query_write_settings);

    row_store_index_file = getDataPartStorage().writeFile(
        MergeTreeDataPartHybrid::ROW_STORE_INDEX_NAME,
        DBMS_DEFAULT_BUFFER_SIZE,
        settings.query_write_settings);
}

void MergeTreeDataPartWriterHybrid::write(const Block & block, const IColumnPermutation * permutation)
{
    // First, write columnar data using parent implementation
    MergeTreeDataPartWriterWide::write(block, permutation);

    // Then, write each row to row store
    for (size_t row_idx = 0; row_idx < block.rows(); ++row_idx)
    {
        writeRowToRowStore(block, row_idx);
    }
}

void MergeTreeDataPartWriterHybrid::writeRowToRowStore(const Block & block, size_t row_index)
{
    // Encode the row to a buffer
    WriteBufferFromOwnString row_buffer;
    row_store_encoder->encodeRow(block, row_index, row_buffer);

    String row_data = row_buffer.str();

    // Add row to page manager
    auto [page_id, row_offset] = page_manager->addRow(row_data.data(), row_data.size());

    // Add entry to row store index
    // Map current granule to page location
    RowStoreIndexEntry entry;
    entry.granule_id = current_granule;
    entry.page_id = page_id;
    entry.row_offset_in_page = row_offset;

    row_store_index.addEntry(entry);

    // Increment granule counter based on index granularity
    // This is simplified - in reality we need to track granule boundaries
    // For now, assume each row is a separate granule (will be fixed in integration)
    ++current_granule;
}

void MergeTreeDataPartWriterHybrid::finalizeRowStore()
{
    if (!row_store_file || !row_store_index_file)
        return;

    // Write all pages to row store file
    page_manager->writePages(*row_store_file);
    row_store_file->finalize();
    row_store_file.reset();

    // Write row store index
    writeRowStoreIndex();
    row_store_index_file->finalize();
    row_store_index_file.reset();
}

void MergeTreeDataPartWriterHybrid::writeRowStoreIndex()
{
    row_store_index.writeTo(*row_store_index_file);
}

}

