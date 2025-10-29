#include <Storages/MergeTree/RowStore/RowStoreEncoder.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <Common/CityHash.h>
#include <Compression/CompressedWriteBuffer.h>
#include <Compression/CompressedReadBuffer.h>
#include <DataTypes/DataTypeFactory.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}

RowStoreEncoder::RowStoreEncoder(const NamesAndTypesList & columns_, const RowStoreSettings & settings_)
    : columns(columns_)
    , settings(settings_)
{
    serializations.reserve(columns.size());
    for (const auto & column : columns)
    {
        serializations.push_back(column.type->getDefaultSerialization());
    }
}

void RowStoreEncoder::encodeRow(const Block & block, size_t row_index, WriteBuffer & out)
{
    if (row_index >= block.rows())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Row index {} out of range {}", row_index, block.rows());

    // Write version
    writeIntBinary(ROW_STORE_VERSION, out);

    // Write null bitmap
    writeNullBitmap(block, row_index, out);

    // Write each column value
    for (size_t col_idx = 0; col_idx < block.columns(); ++col_idx)
    {
        const auto & column = block.getByPosition(col_idx).column;
        bool is_null = column->isNullAt(row_index);

        encodeColumnValue(column, row_index, serializations[col_idx], is_null, out);
    }
}

Block RowStoreEncoder::decodeRow(ReadBuffer & in, const NamesAndTypesList & columns_)
{
    // Read version
    UInt8 version;
    readIntBinary(version, in);

    if (version != ROW_STORE_VERSION)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Unsupported row store version: {}", version);

    // Read null bitmap
    auto null_bitmap = readNullBitmap(in, columns_.size());

    // Create result block
    Block result;
    size_t col_idx = 0;

    for (const auto & name_and_type : columns_)
    {
        auto column = name_and_type.type->createColumn();
        auto serialization = name_and_type.type->getDefaultSerialization();

        decodeColumnValue(column, serialization, null_bitmap[col_idx], in);

        result.insert(ColumnWithTypeAndName{std::move(column), name_and_type.type, name_and_type.name});
        ++col_idx;
    }

    return result;
}

size_t RowStoreEncoder::estimateRowSize(const Block & block, size_t row_index) const
{
    size_t size = 1; // version byte
    size += (columns.size() + 7) / 8; // null bitmap

    for (size_t col_idx = 0; col_idx < block.columns(); ++col_idx)
    {
        const auto & column = block.getByPosition(col_idx).column;
        if (!column->isNullAt(row_index))
        {
            // Rough estimate: use average column size
            size += column->byteSize() / column->size();
        }
    }

    return size;
}

void RowStoreEncoder::writeNullBitmap(const Block & block, size_t row_index, WriteBuffer & out)
{
    size_t num_bytes = (block.columns() + 7) / 8;
    std::vector<UInt8> bitmap(num_bytes, 0);

    for (size_t col_idx = 0; col_idx < block.columns(); ++col_idx)
    {
        if (block.getByPosition(col_idx).column->isNullAt(row_index))
        {
            size_t byte_idx = col_idx / 8;
            size_t bit_idx = col_idx % 8;
            bitmap[byte_idx] |= (1 << bit_idx);
        }
    }

    out.write(reinterpret_cast<const char *>(bitmap.data()), num_bytes);
}

std::vector<bool> RowStoreEncoder::readNullBitmap(ReadBuffer & in, size_t num_columns)
{
    size_t num_bytes = (num_columns + 7) / 8;
    std::vector<UInt8> bitmap(num_bytes);
    in.readStrict(reinterpret_cast<char *>(bitmap.data()), num_bytes);

    std::vector<bool> result(num_columns);
    for (size_t col_idx = 0; col_idx < num_columns; ++col_idx)
    {
        size_t byte_idx = col_idx / 8;
        size_t bit_idx = col_idx % 8;
        result[col_idx] = (bitmap[byte_idx] & (1 << bit_idx)) != 0;
    }

    return result;
}

void RowStoreEncoder::encodeColumnValue(
    const ColumnPtr & column,
    size_t row_index,
    const SerializationPtr & serialization,
    bool is_null,
    WriteBuffer & out)
{
    if (is_null)
        return; // Null values are encoded in bitmap only

    // For variable-length types, write length prefix
    WriteBufferFromOwnString value_buf;
    serialization->serializeBinary(*column, row_index, value_buf, {});

    String value = value_buf.str();
    writeVarUInt(value.size(), out);
    out.write(value.data(), value.size());
}

void RowStoreEncoder::decodeColumnValue(
    MutableColumnPtr & column,
    const SerializationPtr & serialization,
    bool is_null,
    ReadBuffer & in)
{
    if (is_null)
    {
        column->insertDefault();
        return;
    }

    // Read length prefix
    UInt64 length;
    readVarUInt(length, in);

    // Read value
    String value(length, '\0');
    in.readStrict(value.data(), length);

    ReadBufferFromString value_buf(value);
    serialization->deserializeBinary(*column, value_buf, {});
}

// RowStorePage implementation

void RowStorePage::PageHeader::write(WriteBuffer & out) const
{
    writeIntBinary(magic, out);
    writeIntBinary(uncompressed_size, out);
    writeIntBinary(compressed_size, out);
    writeIntBinary(row_count, out);
    writeIntBinary(compression_codec, out);
    writeIntBinary(version, out);
    writeIntBinary(reserved, out);
    writeIntBinary(checksum.items[0], out);
    writeIntBinary(checksum.items[1], out);
}

void RowStorePage::PageHeader::read(ReadBuffer & in)
{
    readIntBinary(magic, in);
    readIntBinary(uncompressed_size, in);
    readIntBinary(compressed_size, in);
    readIntBinary(row_count, in);
    readIntBinary(compression_codec, in);
    readIntBinary(version, in);
    readIntBinary(reserved, in);
    readIntBinary(checksum.items[0], in);
    readIntBinary(checksum.items[1], in);
}

bool RowStorePage::addRow(const char * row_data, size_t row_size)
{
    row_offsets.push_back(data.size());
    data.insert(row_data, row_data + row_size);
    return true;
}

std::pair<const char *, size_t> RowStorePage::getRow(size_t row_index) const
{
    if (row_index >= row_offsets.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Row index {} out of range {}", row_index, row_offsets.size());

    size_t offset = row_offsets[row_index];
    size_t next_offset = (row_index + 1 < row_offsets.size()) ? row_offsets[row_index + 1] : data.size();
    size_t size = next_offset - offset;

    return {data.data() + offset, size};
}

bool RowStorePage::hasSpace(size_t row_size, size_t max_page_size) const
{
    return data.size() + row_size <= max_page_size;
}

void RowStorePage::writeTo(WriteBuffer & out, CompressionCodecPtr codec)
{
    header.uncompressed_size = static_cast<UInt32>(data.size());
    header.row_count = static_cast<UInt32>(row_offsets.size());

    // Calculate checksum
    header.checksum = CityHash_v1_0_2::CityHash128(data.data(), data.size());

    // Compress data
    WriteBufferFromOwnString compressed_buf;
    {
        CompressedWriteBuffer compressor(compressed_buf, codec, header.uncompressed_size);
        compressor.write(data.data(), data.size());
        compressor.finalize();
    }

    String compressed = compressed_buf.str();
    header.compressed_size = static_cast<UInt32>(compressed.size());

    // Write header and compressed data
    header.write(out);
    out.write(compressed.data(), compressed.size());

    // Write row offsets
    writeVarUInt(row_offsets.size(), out);
    for (size_t offset : row_offsets)
        writeVarUInt(offset, out);
}

void RowStorePage::readFrom(ReadBuffer & in, CompressionCodecPtr codec)
{
    // Read header
    header.read(in);

    if (!header.validate())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Invalid page magic number");

    // Read compressed data
    String compressed(header.compressed_size, '\0');
    in.readStrict(compressed.data(), header.compressed_size);

    // Decompress
    ReadBufferFromString compressed_buf(compressed);
    CompressedReadBuffer decompressor(compressed_buf);

    data.resize(header.uncompressed_size);
    decompressor.readStrict(data.data(), header.uncompressed_size);

    // Verify checksum
    auto actual_checksum = CityHash_v1_0_2::CityHash128(data.data(), data.size());
    if (actual_checksum != header.checksum)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Page checksum mismatch");

    // Read row offsets
    size_t num_offsets;
    readVarUInt(num_offsets, in);
    row_offsets.resize(num_offsets);
    for (size_t & offset : row_offsets)
        readVarUInt(offset, in);
}

void RowStorePage::clear()
{
    data.clear();
    row_offsets.clear();
    header = PageHeader();
}

// RowStorePageManager implementation

RowStorePageManager::RowStorePageManager(const RowStoreEncoder::RowStoreSettings & settings_)
    : settings(settings_)
{
    codec = CompressionCodecFactory::instance().get(settings.compression_codec);
}

std::pair<size_t, size_t> RowStorePageManager::addRow(const char * row_data, size_t row_size)
{
    // Check if current page has space
    if (!current_page.hasSpace(row_size, settings.page_size))
    {
        finalizePage();
    }

    size_t page_id = pages.size();
    size_t row_offset = current_page.rowCount();

    current_page.addRow(row_data, row_size);
    stats_dirty = true;

    return {page_id, row_offset};
}

void RowStorePageManager::finalizePage()
{
    if (current_page.rowCount() > 0)
    {
        pages.push_back(std::move(current_page));
        current_page.clear();
        stats_dirty = true;
    }
}

void RowStorePageManager::writePages(WriteBuffer & out)
{
    // Finalize current page if needed
    finalizePage();

    // Write number of pages
    writeVarUInt(pages.size(), out);

    // Write each page
    for (auto & page : pages)
    {
        page.writeTo(out, codec);
    }
}

const RowStorePage & RowStorePageManager::getPage(size_t page_id) const
{
    if (page_id >= pages.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Page id {} out of range {}", page_id, pages.size());

    return pages[page_id];
}

RowStorePageManager::Statistics RowStorePageManager::getStatistics() const
{
    if (!stats_dirty)
        return stats;

    stats = Statistics();
    stats.total_pages = pages.size();

    for (const auto & page : pages)
    {
        stats.total_rows += page.rowCount();
        stats.total_uncompressed_bytes += page.size();
        stats.total_compressed_bytes += page.header.compressed_size;
    }

    if (stats.total_uncompressed_bytes > 0)
        stats.compression_ratio = static_cast<double>(stats.total_compressed_bytes) / stats.total_uncompressed_bytes;

    stats_dirty = false;
    return stats;
}

// RowStoreIndexEntry implementation

void RowStoreIndexEntry::write(WriteBuffer & out) const
{
    writeVarUInt(page_id, out);
    writeVarUInt(row_offset_in_page, out);
    writeVarUInt(granule_id, out);
}

void RowStoreIndexEntry::read(ReadBuffer & in)
{
    readVarUInt(page_id, in);
    readVarUInt(row_offset_in_page, in);
    readVarUInt(granule_id, in);
}

// RowStoreIndex implementation

void RowStoreIndex::addEntry(const RowStoreIndexEntry & entry)
{
    entries.push_back(entry);
}

std::optional<RowStoreIndexEntry> RowStoreIndex::findByGranule(size_t granule_id) const
{
    // Binary search by granule_id
    auto it = std::lower_bound(
        entries.begin(),
        entries.end(),
        granule_id,
        [](const RowStoreIndexEntry & entry, size_t id) { return entry.granule_id < id; });

    if (it != entries.end() && it->granule_id == granule_id)
        return *it;

    return std::nullopt;
}

void RowStoreIndex::writeTo(WriteBuffer & out) const
{
    writeVarUInt(entries.size(), out);
    for (const auto & entry : entries)
        entry.write(out);
}

void RowStoreIndex::readFrom(ReadBuffer & in)
{
    size_t count;
    readVarUInt(count, in);

    entries.resize(count);
    for (auto & entry : entries)
        entry.read(in);
}

}

