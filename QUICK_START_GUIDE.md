# Hybrid Row-Columnar Storage - Quick Start Guide

## What is Hybrid Storage?

Hybrid storage combines traditional columnar storage with a row-oriented format to dramatically improve point query performance on wide tables.

**Problem**: Point queries like `SELECT * FROM table WHERE id = 123` on a 100-column table require reading 100 separate column files, resulting in high latency.

**Solution**: Store a binary-encoded copy of each full row alongside columnar data. Point queries read 1 file instead of 100.

## Performance Impact

| Query Type | Columns | Columnar | Hybrid | Speedup |
|------------|---------|----------|--------|---------|
| Point query | 100 | 100ms | 1ms | **100x** |
| Analytical | 100 | 50ms | 50ms | 1x (no change) |

**Trade-off**: 2-3x storage overhead

## Quick Start

### 1. Create a Table with Row Store

```sql
CREATE TABLE user_profiles (
    user_id UInt64,
    name String,
    email String,
    address String,
    phone String,
    -- ... 95 more columns ...
    last_login DateTime
)
ENGINE = MergeTree
ORDER BY user_id
SETTINGS
    enable_row_store = 1,                    -- Enable hybrid storage
    row_store_page_size = 65536,             -- 64KB pages
    row_store_compression_codec = 'LZ4';     -- Fast compression
```

### 2. Insert Data

```sql
-- Data is automatically written to both columnar and row store
INSERT INTO user_profiles VALUES
    (1, 'Alice', 'alice@example.com', '123 Main St', '555-0001', ...),
    (2, 'Bob', 'bob@example.com', '456 Oak Ave', '555-0002', ...);
```

### 3. Query Data

```sql
-- Point query - uses row store (fast!)
SELECT * FROM user_profiles WHERE user_id = 1;

-- Analytical query - uses columnar storage (no change)
SELECT name, count(*) FROM user_profiles GROUP BY name;
```

### 4. Check Row Store Status

```sql
SELECT
    name,
    has_row_store,
    row_store_pages,
    formatReadableSize(row_store_compressed_bytes) AS row_store_size,
    formatReadableSize(bytes_on_disk) AS total_size,
    round(row_store_compression_ratio, 2) AS compression_ratio
FROM system.parts
WHERE table = 'user_profiles' AND active;
```

## When to Use Hybrid Storage

### ✅ Good Use Cases

- **Wide tables** (50+ columns)
- **High point query concurrency** (thousands of QPS)
- **Low update frequency** (mostly inserts)
- **Sufficient storage** (can afford 2-3x overhead)

**Example**: User profiles, product catalogs, configuration tables

### ❌ Poor Use Cases

- **Narrow tables** (<10 columns) - overhead not worth it
- **Analytical workloads** - no benefit, only overhead
- **Storage-constrained** - can't afford 2-3x overhead
- **High update frequency** - write amplification hurts

**Example**: Time-series data, logs, metrics

## Configuration Options

### Table-Level Settings

```sql
SETTINGS
    -- Enable/disable row store
    enable_row_store = 1,
    
    -- Page size (larger = better compression, higher latency)
    row_store_page_size = 65536,  -- 64KB (default)
    
    -- Compression codec
    row_store_compression_codec = 'LZ4',  -- LZ4 (fast) or ZSTD (better compression)
    
    -- Column coverage threshold for point queries
    row_store_min_columns_ratio_for_point_query = 0.8;  -- 80% of columns
```

### Query-Level Settings

```sql
-- Enable/disable row store for point queries
SET enable_row_store_for_point_queries = 1;

-- Force row store (testing only)
SET force_row_store_read = 1;

-- Force columnar (testing only)
SET force_columnar_read = 1;
```

## Monitoring

### Check Row Store Usage

```sql
-- Row store statistics per part
SELECT
    table,
    name,
    has_row_store,
    row_store_pages,
    formatReadableSize(row_store_compressed_bytes) AS compressed,
    formatReadableSize(row_store_uncompressed_bytes) AS uncompressed,
    round(row_store_compression_ratio, 2) AS ratio
FROM system.parts
WHERE has_row_store = 1
ORDER BY row_store_compressed_bytes DESC
LIMIT 10;
```

### Query Performance

```sql
-- Compare point query performance
SELECT
    query,
    type,
    query_duration_ms,
    read_rows,
    read_bytes
FROM system.query_log
WHERE query LIKE '%WHERE user_id = %'
  AND type = 'QueryFinish'
  AND event_time > now() - INTERVAL 1 HOUR
ORDER BY event_time DESC
LIMIT 10;
```

## Troubleshooting

### Row Store Not Being Used

**Problem**: Point queries still slow despite row store enabled.

**Solutions**:
1. Check if row store exists:
   ```sql
   SELECT has_row_store FROM system.parts WHERE table = 'your_table';
   ```

2. Check column coverage:
   ```sql
   -- Query must request ≥80% of columns (default)
   SELECT * FROM table WHERE id = 1;  -- ✅ Uses row store
   SELECT col1, col2 FROM table WHERE id = 1;  -- ❌ Uses columnar
   ```

3. Check settings:
   ```sql
   SELECT name, value FROM system.settings 
   WHERE name LIKE '%row_store%';
   ```

### High Storage Overhead

**Problem**: Row store using too much disk space.

**Solutions**:
1. Use better compression:
   ```sql
   ALTER TABLE your_table MODIFY SETTING row_store_compression_codec = 'ZSTD';
   ```

2. Increase page size (better compression):
   ```sql
   ALTER TABLE your_table MODIFY SETTING row_store_page_size = 131072;  -- 128KB
   ```

3. Disable row store if not needed:
   ```sql
   ALTER TABLE your_table MODIFY SETTING enable_row_store = 0;
   ```

### Slow Writes

**Problem**: Inserts slower with row store enabled.

**Expected**: Row store adds ~2x write overhead (writes both columnar and row store).

**Solutions**:
1. Batch inserts (reduces overhead):
   ```sql
   INSERT INTO table SELECT * FROM source;  -- Better than many small inserts
   ```

2. Disable row store for write-heavy tables:
   ```sql
   ALTER TABLE your_table MODIFY SETTING enable_row_store = 0;
   ```

## Migration Guide

### Enabling Row Store on Existing Table

```sql
-- 1. Enable row store (only affects new parts)
ALTER TABLE your_table MODIFY SETTING enable_row_store = 1;

-- 2. Force rebuild of all parts (optional, for immediate effect)
OPTIMIZE TABLE your_table FINAL;

-- 3. Verify row store created
SELECT count(*) FROM system.parts 
WHERE table = 'your_table' AND has_row_store = 1;
```

### Disabling Row Store

```sql
-- 1. Disable row store (only affects new parts)
ALTER TABLE your_table MODIFY SETTING enable_row_store = 0;

-- 2. Existing parts keep row store (no data loss)
-- 3. To remove row store from all parts, rebuild:
OPTIMIZE TABLE your_table FINAL;
```

## Best Practices

1. **Start Small**: Test on a single table before enabling globally
2. **Monitor Storage**: Track storage growth and compression ratios
3. **Benchmark**: Measure actual performance improvement for your workload
4. **Tune Page Size**: Larger pages = better compression, adjust based on row size
5. **Use LZ4 for Speed**: Use ZSTD only if storage is critical
6. **Batch Inserts**: Minimize write overhead with larger batches
7. **Selective Enablement**: Enable only on tables with point query workloads

## Example: E-commerce Product Catalog

```sql
-- Create product catalog with row store
CREATE TABLE products (
    product_id UInt64,
    name String,
    description String,
    category String,
    brand String,
    price Decimal(10, 2),
    stock_quantity UInt32,
    -- ... 50 more attributes ...
    last_updated DateTime
)
ENGINE = MergeTree
ORDER BY product_id
SETTINGS
    enable_row_store = 1,
    row_store_compression_codec = 'LZ4';

-- Insert products
INSERT INTO products SELECT * FROM source_table;

-- Fast point query (product detail page)
SELECT * FROM products WHERE product_id = 12345;
-- Before: 50ms (50 column reads)
-- After: 1ms (1 row store read)
-- Improvement: 50x faster!

-- Analytical query (still fast)
SELECT category, avg(price) FROM products GROUP BY category;
-- No change in performance (uses columnar storage)
```

## Next Steps

1. Read the [Architecture Documentation](docs/en/development/hybrid-row-columnar-storage.md)
2. Review the [Integration Guide](src/Storages/MergeTree/RowStore/INTEGRATION_GUIDE.md)
3. Run the [Benchmark Script](src/Storages/MergeTree/RowStore/benchmark_row_store.sql)
4. Check the [Implementation Summary](HYBRID_STORAGE_IMPLEMENTATION_SUMMARY.md)

## Support

For issues or questions:
- Check the [README](src/Storages/MergeTree/RowStore/README.md)
- Review the [Troubleshooting](#troubleshooting) section
- File an issue on GitHub

