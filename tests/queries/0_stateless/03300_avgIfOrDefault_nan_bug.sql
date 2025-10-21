-- Test for avgIfOrDefault returning NaN instead of 0 when condition filters out all rows
-- https://github.com/ClickHouse/ClickHouse/issues/XXXXX

SELECT '--- Test case 1: condition filters out all rows ---';
SELECT avgIfOrDefault(number, number > 0 AND number < 60), avgIf(number, number > 0 AND number < 60)
FROM (SELECT 1882.6168500000051 AS number);

SELECT '--- Test case 2: condition filters out all rows (zero value) ---';
SELECT avgIfOrDefault(number, number > 0 AND number < 60), avgIf(number, number > 0 AND number < 60)
FROM (SELECT 0 AS number);

SELECT '--- Test case 3: condition matches some rows ---';
SELECT avgIfOrDefault(number, number > 0 AND number < 60), avgIf(number, number > 0 AND number < 60)
FROM (SELECT 30 AS number);

SELECT '--- Test case 4: multiple rows, condition filters out all ---';
SELECT avgIfOrDefault(number, number > 100), avgIf(number, number > 100)
FROM (SELECT number FROM numbers(10));

SELECT '--- Test case 5: multiple rows, condition matches some ---';
SELECT avgIfOrDefault(number, number > 5), avgIf(number, number > 5)
FROM (SELECT number FROM numbers(10));

SELECT '--- Test case 6: avgOrDefaultIf (different combinator order) ---';
SELECT avgOrDefaultIf(number, number > 0 AND number < 60)
FROM (SELECT 1882.6168500000051 AS number);

SELECT '--- Test case 7: avgOrDefaultIf with zero ---';
SELECT avgOrDefaultIf(number, number > 0 AND number < 60)
FROM (SELECT 0 AS number);

SELECT '--- Test case 8: other aggregate functions with OrDefault and If ---';
SELECT sumIfOrDefault(number, number > 100), countIfOrDefault(number, number > 100)
FROM (SELECT number FROM numbers(10));

