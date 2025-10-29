# Row Store Integration Guide

This document describes how to integrate the row store module into ClickHouse's MergeTree engine.

## Integration Points

### 1. Data Part Type Registration

**File**: `src/Storages/MergeTree/MergeTreeDataPartType.cpp`

Add new part type for hybrid storage:

```cpp
enum class Value
{
    Wide,
    Compact,
    Hybrid,  // NEW: Hybrid row-columnar storage
    Unknown,
};

String MergeTreeDataPartType::toString() const
{
    switch (value)
    {
        case Value::Wide: return "Wide";
        case Value::Compact: return "Compact";
        case Value::Hybrid: return "Hybrid";  // NEW
        case Value::Unknown: return "Unknown";
    }
}

void MergeTreeDataPartType::fromString(const String & str)
{
    if (str == "Wide")
        value = Value::Wide;
    else if (str == "Compact")
        value = Value::Compact;
    else if (str == "Hybrid")  // NEW
        value = Value::Hybrid;
    else
        value = Value::Unknown;
}
```

### 2. Part Format Selection

**File**: `src/Storages/MergeTree/MergeTreeData.cpp`

Modify `choosePartFormat()` to select Hybrid format when row store is enabled:

```cpp
MergeTreeData::MutableDataPartPtr MergeTreeData::createPart(
    const String & name,
    MergeTreeDataPartType type,
    const MergeTreePartInfo & part_info,
    const MutableDataPartStoragePtr & data_part_storage,
    const IMergeTreeDataPart * parent_part) const
{
    // Check if row store is enabled
    bool enable_row_store = (*getSettings())[MergeTreeSetting::enable_row_store];
    
    if (enable_row_store && type == MergeTreeDataPartType::Wide)
    {
        // Use Hybrid format instead of Wide
        return std::make_shared<MergeTreeDataPartHybrid>(
            *this, name, part_info, data_part_storage, parent_part);
    }
    
    // Existing logic for Wide/Compact
    if (type == MergeTreeDataPartType::Wide)
        return std::make_shared<MergeTreeDataPartWide>(...);
    else if (type == MergeTreeDataPartType::Compact)
        return std::make_shared<MergeTreeDataPartCompact>(...);
    
    throw Exception(...);
}
```

### 3. Writer Factory

**File**: `src/Storages/MergeTree/IMergeTreeDataPartWriter.cpp`

Update `createMergeTreeDataPartWriter()` to create hybrid writer:

```cpp
MergeTreeDataPartWriterPtr createMergeTreeDataPartWriter(
    MergeTreeDataPartType part_type,
    const String & data_part_name_,
    // ... other parameters
)
{
    if (part_type == MergeTreeDataPartType::Hybrid)
    {
        return std::make_shared<MergeTreeDataPartWriterHybrid>(
            data_part_name_,
            serializations_,
            data_part_storage_,
            // ... other parameters
        );
    }
    else if (part_type == MergeTreeDataPartType::Wide)
    {
        return std::make_shared<MergeTreeDataPartWriterWide>(...);
    }
    else if (part_type == MergeTreeDataPartType::Compact)
    {
        return std::make_shared<MergeTreeDataPartWriterCompact>(...);
    }
    
    throw Exception(...);
}
```

### 4. Query Execution Integration

**File**: `src/Processors/QueryPlan/ReadFromMergeTree.cpp`

Add point query detection and row store path selection:

```cpp
void ReadFromMergeTree::initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings &)
{
    // Detect if this is a point query
    auto query_pattern = PointQueryDetector::analyzeQuery(
        query_info,
        storage_snapshot->metadata,
        row_store_settings);
    
    if (query_pattern.is_point_query && enable_row_store_for_point_queries)
    {
        // Use row store read path
        auto pipe = readFromRowStore(parts_with_ranges, query_pattern);
        pipeline.init(std::move(pipe));
        return;
    }
    
    // Use existing columnar read path
    auto pipe = read(parts_with_ranges, ...);
    pipeline.init(std::move(pipe));
}

Pipe ReadFromMergeTree::readFromRowStore(
    const RangesInDataParts & parts_with_ranges,
    const PointQueryDetector::QueryPattern & pattern)
{
    // Create row store reader source
    Pipes pipes;
    
    for (const auto & part_with_ranges : parts_with_ranges)
    {
        auto hybrid_part = std::dynamic_pointer_cast<MergeTreeDataPartHybrid>(
            part_with_ranges.data_part);
        
        if (!hybrid_part || !hybrid_part->hasRowStore())
        {
            // Fallback to columnar read
            continue;
        }
        
        // Create row store source
        auto source = std::make_shared<RowStoreSource>(
            hybrid_part,
            storage_snapshot,
            part_with_ranges.ranges,
            required_columns);
        
        pipes.emplace_back(std::move(source));
    }
    
    return Pipe::unitePipes(std::move(pipes));
}
```

### 5. Row Store Source Processor

**File**: `src/Storages/MergeTree/RowStore/RowStoreSource.h` (NEW)

```cpp
class RowStoreSource : public ISource
{
public:
    RowStoreSource(
        MergeTreeDataPartHybridPtr data_part_,
        StorageSnapshotPtr storage_snapshot_,
        MarkRanges mark_ranges_,
        Names required_columns_);

protected:
    Chunk generate() override;

private:
    MergeTreeDataPartHybridPtr data_part;
    StorageSnapshotPtr storage_snapshot;
    MarkRanges mark_ranges;
    Names required_columns;
    
    size_t current_mark = 0;
    size_t current_row = 0;
    
    // Page cache for row store
    std::shared_ptr<RowStorePageCache> page_cache;
};
```

**File**: `src/Storages/MergeTree/RowStore/RowStoreSource.cpp` (NEW)

```cpp
Chunk RowStoreSource::generate()
{
    if (current_mark >= mark_ranges.size())
        return {};
    
    const auto & range = mark_ranges[current_mark];
    
    // Read rows from row store
    Columns columns;
    size_t rows_read = 0;
    
    for (size_t granule = range.begin; granule < range.end; ++granule)
    {
        // Read row from row store
        Block row_block = data_part->readRowFromRowStore(
            granule,
            storage_snapshot->metadata->getColumns().getAllPhysical());
        
        // Append to result columns
        if (columns.empty())
        {
            for (const auto & col : row_block)
                columns.push_back(col.column->cloneEmpty());
        }
        
        for (size_t i = 0; i < row_block.columns(); ++i)
            columns[i]->insertFrom(*row_block.getByPosition(i).column, 0);
        
        ++rows_read;
    }
    
    ++current_mark;
    
    return Chunk(std::move(columns), rows_read);
}
```

### 6. Page Cache Implementation

**File**: `src/Storages/MergeTree/RowStore/RowStorePageCache.h` (NEW)

```cpp
class RowStorePageCache
{
public:
    using Key = std::tuple<String, size_t>; // (part_path, page_id)
    using Value = std::shared_ptr<RowStorePage>;
    
    RowStorePageCache(size_t max_size_bytes);
    
    // Get page from cache or load from disk
    Value getOrLoad(
        const Key & key,
        std::function<Value()> load_func);
    
    // Invalidate cache for a part
    void invalidate(const String & part_path);
    
    // Get cache statistics
    struct Statistics
    {
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        size_t size_bytes = 0;
    };
    
    Statistics getStatistics() const;

private:
    using Cache = LRUCache<Key, Value>;
    std::unique_ptr<Cache> cache;
    
    mutable std::mutex mutex;
    Statistics stats;
};
```

### 7. Settings Integration

**File**: `src/Core/Settings.cpp`

Add global settings for row store:

```cpp
DECLARE(Bool, enable_row_store_for_point_queries, true, R"(
Enable automatic use of row store for point queries when available.
Can be disabled to force columnar reads for testing.
)", 0) \
DECLARE(Bool, force_row_store_read, false, R"(
Force use of row store for all queries (testing only).
)", 0) \
DECLARE(Bool, force_columnar_read, false, R"(
Force use of columnar storage for all queries (testing only).
)", 0) \
DECLARE(UInt64, row_store_page_cache_size_bytes, 1024 * 1024 * 1024, R"(
Maximum size of row store page cache in bytes. Default is 1GB.
)", 0) \
```

### 8. System Tables

**File**: `src/Storages/System/StorageSystemParts.cpp`

Add row store columns to `system.parts`:

```cpp
// Add columns
{"has_row_store", std::make_shared<DataTypeUInt8>()},
{"row_store_pages", std::make_shared<DataTypeUInt64>()},
{"row_store_compressed_bytes", std::make_shared<DataTypeUInt64>()},
{"row_store_uncompressed_bytes", std::make_shared<DataTypeUInt64>()},
{"row_store_compression_ratio", std::make_shared<DataTypeFloat64>()},

// Fill data
if (auto hybrid_part = std::dynamic_pointer_cast<MergeTreeDataPartHybrid>(part))
{
    auto stats = hybrid_part->getRowStoreStatistics();
    columns[i++]->insert(hybrid_part->hasRowStore() ? 1 : 0);
    columns[i++]->insert(stats.total_pages);
    columns[i++]->insert(stats.compressed_bytes);
    columns[i++]->insert(stats.uncompressed_bytes);
    columns[i++]->insert(stats.compression_ratio);
}
```

## Build System Integration

**File**: `src/Storages/MergeTree/CMakeLists.txt`

Add row store sources:

```cmake
# Row store sources
set(MERGETREE_ROW_STORE_SOURCES
    RowStore/RowStoreEncoder.cpp
    RowStore/PointQueryDetector.cpp
    RowStore/MergeTreeDataPartHybrid.cpp
    RowStore/MergeTreeDataPartWriterHybrid.cpp
    RowStore/RowStoreSource.cpp
    RowStore/RowStorePageCache.cpp
)

# Add to main sources
list(APPEND MERGETREE_SOURCES ${MERGETREE_ROW_STORE_SOURCES})

# Tests
if (ENABLE_TESTS)
    add_executable(gtest_row_store_encoder
        RowStore/tests/gtest_row_store_encoder.cpp
    )
    target_link_libraries(gtest_row_store_encoder PRIVATE
        clickhouse_storages_mergetree
        gtest
        gtest_main
    )
    add_test(NAME gtest_row_store_encoder COMMAND gtest_row_store_encoder)
endif()
```

## Testing Integration

### Functional Tests

**File**: `tests/queries/0_stateless/03500_row_store_point_query.sql` (NEW)

```sql
-- Test row store point queries

DROP TABLE IF EXISTS test_row_store;

CREATE TABLE test_row_store (
    id UInt64,
    col1 String,
    col2 String,
    col3 UInt32
)
ENGINE = MergeTree
ORDER BY id
SETTINGS enable_row_store = 1;

INSERT INTO test_row_store VALUES
    (1, 'a', 'x', 100),
    (2, 'b', 'y', 200),
    (3, 'c', 'z', 300);

-- Point query (should use row store)
SELECT * FROM test_row_store WHERE id = 2;

-- Verify row store exists
SELECT
    name,
    has_row_store,
    row_store_pages
FROM system.parts
WHERE table = 'test_row_store' AND active;

DROP TABLE test_row_store;
```

## Migration Path

1. **Phase 1**: Implement core components (DONE)
2. **Phase 2**: Integrate with MergeTree (IN PROGRESS)
3. **Phase 3**: Add page cache and optimization
4. **Phase 4**: Production testing and tuning
5. **Phase 5**: Enable by default for wide tables

## Rollback Plan

If issues arise, row store can be disabled:

```sql
-- Disable globally
ALTER TABLE table_name MODIFY SETTING enable_row_store = 0;

-- Disable for new parts only (existing parts keep row store)
SET enable_row_store = 0;
```

Existing parts with row store continue to work in columnar mode.

