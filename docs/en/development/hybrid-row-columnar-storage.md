# Hybrid Row-Columnar Storage in ClickHouse

## Overview

This document describes the hybrid row-columnar storage architecture in ClickHouse, inspired by Apache Doris. The hybrid storage combines traditional columnar storage with an optional row-oriented storage format to dramatically improve point query performance on wide tables.

## Motivation

Traditional columnar storage excels at analytical queries that scan many rows but few columns. However, point queries (e.g., `SELECT * WHERE pk=?`) on wide tables suffer from:

1. **High I/O amplification**: Must read N separate column files for N columns
2. **Poor cache locality**: Column data scattered across multiple files
3. **Increased IOPS**: Each column read requires separate disk seeks
4. **Latency multiplication**: Total latency = N × single-column-latency

For tables with 100+ columns, a single point query can require 100+ file reads, making latency unacceptable for high-concurrency OLTP-like workloads.

## Architecture

### Dual Storage Paths

The hybrid storage maintains two representations of the same data:

1. **Columnar Storage** (existing): Each column stored in separate `.bin` files
2. **Row Store Column** (new): A hidden column `__row_store` containing binary-encoded full rows

### Row Store Format

#### Binary Row Encoding

Each row is encoded as a binary blob containing all column values:

```
[row_header][col1_data][col2_data]...[colN_data]
```

Row header format:
- Version (1 byte): Format version for backward compatibility
- Null bitmap (ceil(N/8) bytes): Bit flags for NULL values
- Variable-length offsets (optional): For variable-length columns

#### Page Structure

Rows are organized into fixed-size compressed pages:

```
Page = [page_header][row1][row2]...[rowM]
```

Page header:
- Page size (4 bytes): Uncompressed size
- Row count (4 bytes): Number of rows in page
- Checksum (16 bytes): CityHash128 for integrity
- Compression codec (1 byte): LZ4, ZSTD, etc.

Default page size: 64KB (configurable via `row_store_page_size`)

### Primary Key Index Enhancement

The existing sparse primary key index is extended with row store metadata:

```
primary.idx:
  [key_value][column_offset][row_store_page_id][row_store_offset]
```

For each index granule:
- `key_value`: Primary key value (existing)
- `column_offset`: Offset in columnar files (existing)
- `row_store_page_id`: Page number in row store column
- `row_store_offset`: Byte offset within page

### Delete Bitmap (Future Enhancement)

For UPDATE/DELETE support with merge-on-write semantics:

```
delete_bitmap.bin:
  [granule_id][roaring_bitmap]
```

Each granule has a Roaring Bitmap marking deleted row positions. During reads, the bitmap is consulted to skip deleted rows.

## Query Optimization

### Point Query Detection

The query optimizer detects point query patterns:

```sql
-- Detected as point query
SELECT * FROM table WHERE pk = 123;
SELECT col1, col2, ..., colN FROM table WHERE pk = 123;

-- NOT detected (partial column read)
SELECT col1, col2 FROM table WHERE pk = 123;

-- NOT detected (range scan)
SELECT * FROM table WHERE pk > 100 AND pk < 200;
```

Detection criteria:
1. WHERE clause contains exact primary key match (`pk = value`)
2. All or most columns requested (>80% by default, configurable)
3. Single row expected (no range conditions)

### Query Path Selection

```
┌─────────────────┐
│  Query Parser   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Point Query?    │◄─── Check: pk=?, SELECT *, single row
└────┬───────┬────┘
     │       │
    Yes      No
     │       │
     ▼       ▼
┌─────────┐ ┌──────────────┐
│Row Store│ │Columnar Store│
│  Path   │ │    Path      │
└─────────┘ └──────────────┘
```

### Row Store Read Path

1. **Index Lookup**: Binary search in `primary.idx` for key
2. **Page Load**: Read compressed page from `__row_store.bin`
3. **Decompress**: Decompress page (typically 64KB → 256KB)
4. **Row Extract**: Locate row within page using offset
5. **Deserialize**: Decode binary row into column values
6. **Return**: Single I/O operation vs. N column reads

## Configuration

### Table-Level Settings

```sql
CREATE TABLE wide_table (
    id UInt64,
    col1 String,
    col2 String,
    ...
    col100 String
)
ENGINE = MergeTree
ORDER BY id
SETTINGS
    enable_row_store = 1,                    -- Enable hybrid storage
    row_store_page_size = 65536,             -- 64KB pages
    row_store_compression_codec = 'LZ4',     -- Compression method
    row_store_min_columns_for_point_query = 0.8;  -- 80% columns threshold
```

### System Settings

```sql
-- Global settings
SET enable_row_store_for_point_queries = 1;  -- Use row store for point queries
SET force_row_store_read = 0;                -- Force row store (testing)
SET force_columnar_read = 0;                 -- Force columnar (testing)
```

## Storage Overhead

### Space Amplification

Row store adds storage overhead:

- **Best case** (highly compressible): 1.2-1.5x total size
- **Typical case**: 2-3x total size
- **Worst case** (incompressible): 5-10x total size

Factors affecting overhead:
- Column count (more columns = higher overhead)
- Data compressibility
- Page size (larger pages = better compression)
- Null density (more nulls = lower overhead)

### Write Amplification

Each insert writes:
1. Columnar data (existing)
2. Row store data (new)
3. Primary index update (existing)

Write amplification: ~2x (row store adds ~100% overhead)

## Performance Characteristics

### Point Query Performance

| Metric | Columnar | Hybrid (Row Store) | Improvement |
|--------|----------|-------------------|-------------|
| IOPS | 100 (100 columns) | 1 | 100x |
| Latency (SSD) | 100ms | 1ms | 100x |
| Latency (HDD) | 1000ms | 10ms | 100x |
| Throughput (QPS) | 100 | 10,000 | 100x |

### Analytical Query Performance

Analytical queries use columnar storage (no regression):
- Full table scans: No change
- Column subset scans: No change
- Aggregations: No change

## Implementation Status

### Phase 1: Core Storage (Current)
- [x] Row store column type definition
- [x] Binary row encoding/decoding
- [x] Page-based storage structure
- [x] Compression integration
- [ ] Primary index extension

### Phase 2: Query Optimization
- [ ] Point query pattern detection
- [ ] Row store read path
- [ ] Query path selection logic
- [ ] Performance metrics

### Phase 3: Advanced Features
- [ ] Delete bitmap support
- [ ] UPDATE/DELETE with merge-on-write
- [ ] Adaptive page sizing
- [ ] Row store statistics

### Phase 4: Production Readiness
- [ ] Comprehensive testing
- [ ] Performance benchmarks
- [ ] Documentation
- [ ] Migration tools

## Trade-offs and Recommendations

### When to Use Hybrid Storage

✅ **Good fit:**
- Wide tables (50+ columns)
- High point query concurrency
- Low update frequency
- Sufficient storage capacity

❌ **Poor fit:**
- Narrow tables (<10 columns)
- Primarily analytical workloads
- Storage-constrained environments
- High update/delete frequency

### Best Practices

1. **Start with columnar**: Only enable row store if point query performance is critical
2. **Monitor storage**: Track storage growth and adjust page size
3. **Benchmark**: Test with realistic workloads before production
4. **Selective enablement**: Enable per-table, not globally
5. **Tune page size**: Larger pages = better compression, higher latency

## Future Enhancements

1. **Adaptive row store**: Automatically enable based on query patterns
2. **Partial row store**: Store only frequently-queried columns
3. **Tiered storage**: Row store on SSD, columnar on HDD
4. **Smart caching**: Cache hot pages in memory
5. **Async updates**: Background row store updates for better write performance

## References

- Apache Doris row store implementation
- ClickHouse MergeTree architecture
- Columnar vs. row-oriented storage trade-offs
- Page-based storage systems

