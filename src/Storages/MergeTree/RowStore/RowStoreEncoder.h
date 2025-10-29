#pragma once

#include <Core/Block.h>
#include <Core/NamesAndTypes.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadBuffer.h>
#include <Compression/CompressionFactory.h>
#include <Common/PODArray.h>

namespace DB
{

/// Binary format for encoding a single row
/// Format: [version][null_bitmap][col1_data][col2_data]...[colN_data]
/// For variable-length columns, we store length prefix before data
class RowStoreEncoder
{
public:
    static constexpr UInt8 ROW_STORE_VERSION = 1;
    static constexpr size_t DEFAULT_PAGE_SIZE = 64 * 1024; // 64KB

    struct RowStoreSettings
    {
        size_t page_size = DEFAULT_PAGE_SIZE;
        String compression_codec = "LZ4";
        bool enable_checksums = true;
        
        /// Minimum ratio of columns that must be requested to use row store
        /// 0.8 means if query requests 80%+ of columns, use row store
        double min_columns_ratio_for_point_query = 0.8;
    };

    explicit RowStoreEncoder(const NamesAndTypesList & columns, const RowStoreSettings & settings_);

    /// Encode a single row from a block at given row index
    /// Returns the encoded binary data
    void encodeRow(const Block & block, size_t row_index, WriteBuffer & out);

    /// Decode a single row from binary data
    /// Returns a block with single row
    Block decodeRow(ReadBuffer & in, const NamesAndTypesList & columns);

    /// Get the size estimate for a row (for page packing)
    size_t estimateRowSize(const Block & block, size_t row_index) const;

private:
    NamesAndTypesList columns;
    RowStoreSettings settings;
    
    /// Serializations for each column
    std::vector<SerializationPtr> serializations;
    
    /// Helper to write null bitmap
    void writeNullBitmap(const Block & block, size_t row_index, WriteBuffer & out);
    
    /// Helper to read null bitmap
    std::vector<bool> readNullBitmap(ReadBuffer & in, size_t num_columns);
    
    /// Encode a single column value
    void encodeColumnValue(
        const ColumnPtr & column,
        size_t row_index,
        const SerializationPtr & serialization,
        bool is_null,
        WriteBuffer & out);
    
    /// Decode a single column value
    void decodeColumnValue(
        MutableColumnPtr & column,
        const SerializationPtr & serialization,
        bool is_null,
        ReadBuffer & in);
};

/// Page structure for row store
/// A page contains multiple rows, compressed together
struct RowStorePage
{
    static constexpr UInt32 PAGE_MAGIC = 0x52535047; // "RSPG" - Row Store PaGe
    
    struct PageHeader
    {
        UInt32 magic = PAGE_MAGIC;
        UInt32 uncompressed_size = 0;
        UInt32 compressed_size = 0;
        UInt32 row_count = 0;
        UInt8 compression_codec = 0;
        UInt8 version = RowStoreEncoder::ROW_STORE_VERSION;
        UInt16 reserved = 0;
        UInt128 checksum{0, 0}; // CityHash128
        
        void write(WriteBuffer & out) const;
        void read(ReadBuffer & in);
        bool validate() const { return magic == PAGE_MAGIC; }
    };
    
    PageHeader header;
    PODArray<char> data; // Uncompressed row data
    
    /// Row offsets within the page (for quick lookup)
    std::vector<size_t> row_offsets;
    
    /// Add a row to the page
    /// Returns false if page is full
    bool addRow(const char * row_data, size_t row_size);
    
    /// Get row data at index
    std::pair<const char *, size_t> getRow(size_t row_index) const;
    
    /// Check if page has space for a row
    bool hasSpace(size_t row_size, size_t max_page_size) const;
    
    /// Compress and write page to output
    void writeTo(WriteBuffer & out, CompressionCodecPtr codec);
    
    /// Read and decompress page from input
    void readFrom(ReadBuffer & in, CompressionCodecPtr codec);
    
    /// Clear page data
    void clear();
    
    size_t size() const { return data.size(); }
    size_t rowCount() const { return row_offsets.size(); }
};

/// Manages multiple pages for row store column
class RowStorePageManager
{
public:
    explicit RowStorePageManager(const RowStoreEncoder::RowStoreSettings & settings_);
    
    /// Add a row, returns (page_id, row_offset_in_page)
    std::pair<size_t, size_t> addRow(const char * row_data, size_t row_size);
    
    /// Finalize current page and start new one
    void finalizePage();
    
    /// Write all pages to output stream
    void writePages(WriteBuffer & out);
    
    /// Get total number of pages
    size_t getPageCount() const { return pages.size(); }
    
    /// Get page by index
    const RowStorePage & getPage(size_t page_id) const;
    
    /// Get statistics
    struct Statistics
    {
        size_t total_pages = 0;
        size_t total_rows = 0;
        size_t total_uncompressed_bytes = 0;
        size_t total_compressed_bytes = 0;
        double compression_ratio = 0.0;
    };
    
    Statistics getStatistics() const;

private:
    RowStoreEncoder::RowStoreSettings settings;
    CompressionCodecPtr codec;
    
    /// All pages (finalized)
    std::vector<RowStorePage> pages;
    
    /// Current page being built
    RowStorePage current_page;
    
    /// Statistics
    mutable Statistics stats;
    bool stats_dirty = true;
};

/// Index entry for row store
/// Maps primary key to (page_id, row_offset)
struct RowStoreIndexEntry
{
    size_t page_id = 0;
    size_t row_offset_in_page = 0;
    size_t granule_id = 0; // For compatibility with existing index
    
    void write(WriteBuffer & out) const;
    void read(ReadBuffer & in);
};

/// Row store index - extends primary index with row store metadata
class RowStoreIndex
{
public:
    /// Add an index entry
    void addEntry(const RowStoreIndexEntry & entry);
    
    /// Find entry by granule id (binary search)
    std::optional<RowStoreIndexEntry> findByGranule(size_t granule_id) const;
    
    /// Write index to file
    void writeTo(WriteBuffer & out) const;
    
    /// Read index from file
    void readFrom(ReadBuffer & in);
    
    /// Get number of entries
    size_t size() const { return entries.size(); }
    
    /// Clear all entries
    void clear() { entries.clear(); }

private:
    std::vector<RowStoreIndexEntry> entries;
};

/// Helper class to detect if a query is a point query
class PointQueryDetector
{
public:
    struct QueryPattern
    {
        bool is_point_query = false;
        bool has_pk_equality = false;
        bool selects_all_columns = false;
        double column_coverage_ratio = 0.0;
        size_t expected_rows = 0;
    };
    
    /// Analyze query to determine if it's a point query
    static QueryPattern analyzeQuery(
        const SelectQueryInfo & query_info,
        const StorageMetadataPtr & metadata,
        const RowStoreEncoder::RowStoreSettings & settings);
    
private:
    /// Check if WHERE clause contains pk = value
    static bool hasPrimaryKeyEquality(
        const ASTPtr & where_ast,
        const Names & primary_key_columns);
    
    /// Calculate what fraction of columns are requested
    static double calculateColumnCoverage(
        const Names & required_columns,
        const NamesAndTypesList & all_columns);
};

}

