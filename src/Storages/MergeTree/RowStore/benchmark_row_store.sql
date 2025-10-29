-- Benchmark script for hybrid row-columnar storage
-- Compares point query performance: Columnar vs Row Store
--
-- NOTE: This benchmark is in two phases:
-- PHASE 1 (CURRENT): Demonstrates the problem - slow point queries on wide tables
-- PHASE 2 (FUTURE): Will demonstrate the solution once row store integration is complete
--
-- Current Status: Row store core components are implemented but not yet integrated
-- into MergeTree engine. The enable_row_store setting exists but doesn't create
-- the __row_store column yet. This benchmark currently shows PHASE 1 only.

-- ============================================================================
-- PHASE 1: Demonstrate the Problem (Current - Works Now)
-- ============================================================================

DROP TABLE IF EXISTS benchmark_wide_table;

-- Wide table with 50 columns (simulates real-world use case)
CREATE TABLE benchmark_wide_table (
    id UInt64,
    col1 String,
    col2 String,
    col3 String,
    col4 String,
    col5 String,
    col6 String,
    col7 String,
    col8 String,
    col9 String,
    col10 String,
    col11 UInt64,
    col12 UInt64,
    col13 UInt64,
    col14 UInt64,
    col15 UInt64,
    col16 Float64,
    col17 Float64,
    col18 Float64,
    col19 Float64,
    col20 Float64,
    col21 String,
    col22 String,
    col23 String,
    col24 String,
    col25 String,
    col26 UInt32,
    col27 UInt32,
    col28 UInt32,
    col29 UInt32,
    col30 UInt32,
    col31 String,
    col32 String,
    col33 String,
    col34 String,
    col35 String,
    col36 UInt64,
    col37 UInt64,
    col38 UInt64,
    col39 UInt64,
    col40 UInt64,
    col41 String,
    col42 String,
    col43 String,
    col44 String,
    col45 String,
    col46 Float32,
    col47 Float32,
    col48 Float32,
    col49 Float32,
    col50 Float32
)
ENGINE = MergeTree
ORDER BY id;

-- ============================================================================
-- Data Generation: Insert 100K rows (smaller for quick testing)
-- ============================================================================

INSERT INTO benchmark_wide_table
SELECT
    number AS id,
    concat('str_', toString(number % 1000)) AS col1,
    concat('data_', toString(number % 500)) AS col2,
    concat('value_', toString(number % 250)) AS col3,
    concat('text_', toString(number % 100)) AS col4,
    concat('info_', toString(number % 50)) AS col5,
    concat('label_', toString(number % 25)) AS col6,
    concat('tag_', toString(number % 10)) AS col7,
    concat('name_', toString(number % 5)) AS col8,
    concat('desc_', toString(number % 3)) AS col9,
    concat('meta_', toString(number % 2)) AS col10,
    number * 2 AS col11,
    number * 3 AS col12,
    number * 5 AS col13,
    number * 7 AS col14,
    number * 11 AS col15,
    number * 1.1 AS col16,
    number * 2.2 AS col17,
    number * 3.3 AS col18,
    number * 4.4 AS col19,
    number * 5.5 AS col20,
    concat('field_', toString(number % 100)) AS col21,
    concat('attr_', toString(number % 50)) AS col22,
    concat('prop_', toString(number % 25)) AS col23,
    concat('key_', toString(number % 10)) AS col24,
    concat('val_', toString(number % 5)) AS col25,
    (number % 1000000)::UInt32 AS col26,
    (number % 500000)::UInt32 AS col27,
    (number % 250000)::UInt32 AS col28,
    (number % 100000)::UInt32 AS col29,
    (number % 50000)::UInt32 AS col30,
    concat('extra_', toString(number % 100)) AS col31,
    concat('misc_', toString(number % 50)) AS col32,
    concat('other_', toString(number % 25)) AS col33,
    concat('aux_', toString(number % 10)) AS col34,
    concat('temp_', toString(number % 5)) AS col35,
    number * 13 AS col36,
    number * 17 AS col37,
    number * 19 AS col38,
    number * 23 AS col39,
    number * 29 AS col40,
    concat('alpha_', toString(number % 100)) AS col41,
    concat('beta_', toString(number % 50)) AS col42,
    concat('gamma_', toString(number % 25)) AS col43,
    concat('delta_', toString(number % 10)) AS col44,
    concat('omega_', toString(number % 5)) AS col45,
    (number * 0.1)::Float32 AS col46,
    (number * 0.2)::Float32 AS col47,
    (number * 0.3)::Float32 AS col48,
    (number * 0.4)::Float32 AS col49,
    (number * 0.5)::Float32 AS col50
FROM numbers(100000);

-- ============================================================================
-- Optimize table
-- ============================================================================

OPTIMIZE TABLE benchmark_wide_table FINAL;

-- ============================================================================
-- PHASE 1: Measure Current Performance (Columnar Only)
-- ============================================================================

SELECT '=== PHASE 1: Current State (Columnar Storage) ===' AS phase;

-- Show table statistics
SELECT
    'Table Statistics' AS metric,
    formatReadableSize(sum(bytes_on_disk)) AS total_size,
    sum(rows) AS total_rows,
    count() AS parts
FROM system.parts
WHERE table = 'benchmark_wide_table' AND active;

-- Show column file count (this is the problem!)
SELECT
    'Column Files' AS metric,
    count(DISTINCT name) AS column_count,
    concat('Point query reads ', toString(count(DISTINCT name)), ' files!') AS problem
FROM system.columns
WHERE table = 'benchmark_wide_table';

-- ============================================================================
-- Benchmark: Point Queries (SELECT *)
-- ============================================================================

SELECT '=== Point Query Benchmark (SELECT *) ===' AS benchmark;

-- Warm up
SELECT * FROM benchmark_columnar WHERE id = 12345 FORMAT Null;
SELECT * FROM benchmark_hybrid WHERE id = 12345 FORMAT Null;

-- Benchmark columnar (100 queries)
SELECT
    'Columnar' AS storage_type,
    count() AS queries,
    round(avg(query_duration_ms), 2) AS avg_ms,
    round(min(query_duration_ms), 2) AS min_ms,
    round(max(query_duration_ms), 2) AS max_ms
FROM (
    SELECT query_duration_ms FROM system.query_log
    WHERE query LIKE '%benchmark_columnar%WHERE id =%'
    AND type = 'QueryFinish'
    AND event_time > now() - INTERVAL 1 MINUTE
    ORDER BY event_time DESC
    LIMIT 100
);

-- Benchmark hybrid (100 queries)
SELECT
    'Hybrid (Row Store)' AS storage_type,
    count() AS queries,
    round(avg(query_duration_ms), 2) AS avg_ms,
    round(min(query_duration_ms), 2) AS min_ms,
    round(max(query_duration_ms), 2) AS max_ms
FROM (
    SELECT query_duration_ms FROM system.query_log
    WHERE query LIKE '%benchmark_hybrid%WHERE id =%'
    AND type = 'QueryFinish'
    AND event_time > now() - INTERVAL 1 MINUTE
    ORDER BY event_time DESC
    LIMIT 100
);

-- ============================================================================
-- Cleanup
-- ============================================================================

-- DROP TABLE benchmark_columnar;
-- DROP TABLE benchmark_hybrid;

