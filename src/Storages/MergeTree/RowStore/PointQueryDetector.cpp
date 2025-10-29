#include <Storages/MergeTree/RowStore/RowStoreEncoder.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>

namespace DB
{

PointQueryDetector::QueryPattern PointQueryDetector::analyzeQuery(
    const SelectQueryInfo & query_info,
    const StorageMetadataPtr & metadata,
    const RowStoreEncoder::RowStoreSettings & settings)
{
    QueryPattern pattern;
    
    // Check if we have a SELECT query
    const auto * select_query = query_info.query->as<ASTSelectQuery>();
    if (!select_query)
        return pattern;
    
    // Check for WHERE clause with primary key equality
    if (select_query->where())
    {
        const auto & primary_key = metadata->getPrimaryKey();
        pattern.has_pk_equality = hasPrimaryKeyEquality(
            select_query->where(),
            primary_key.column_names);
    }
    
    // Calculate column coverage
    if (query_info.syntax_analyzer_result)
    {
        const auto & required_columns = query_info.syntax_analyzer_result->requiredSourceColumns();
        const auto & all_columns = metadata->getColumns().getAllPhysical();
        
        pattern.column_coverage_ratio = calculateColumnCoverage(required_columns, all_columns);
        pattern.selects_all_columns = pattern.column_coverage_ratio >= settings.min_columns_ratio_for_point_query;
    }
    
    // Determine if this is a point query
    pattern.is_point_query = pattern.has_pk_equality && pattern.selects_all_columns;
    
    // Estimate expected rows (1 for point query with pk equality)
    if (pattern.has_pk_equality)
        pattern.expected_rows = 1;
    
    return pattern;
}

bool PointQueryDetector::hasPrimaryKeyEquality(
    const ASTPtr & where_ast,
    const Names & primary_key_columns)
{
    if (!where_ast)
        return false;
    
    // Check if it's a function (comparison)
    const auto * func = where_ast->as<ASTFunction>();
    if (!func)
        return false;
    
    // Check for equality operator
    if (func->name == "equals")
    {
        if (func->arguments && func->arguments->children.size() == 2)
        {
            // Check if left side is a primary key column
            const auto * identifier = func->arguments->children[0]->as<ASTIdentifier>();
            if (identifier)
            {
                String column_name = identifier->name();
                
                // Check if this is the first primary key column
                // (for simplicity, we only support equality on first PK column)
                if (!primary_key_columns.empty() && column_name == primary_key_columns[0])
                {
                    // Check if right side is a literal (constant value)
                    const auto * literal = func->arguments->children[1]->as<ASTLiteral>();
                    if (literal)
                        return true;
                }
            }
        }
    }
    
    // Check for AND conditions - recursively check each side
    if (func->name == "and")
    {
        if (func->arguments)
        {
            for (const auto & child : func->arguments->children)
            {
                if (hasPrimaryKeyEquality(child, primary_key_columns))
                    return true;
            }
        }
    }
    
    return false;
}

double PointQueryDetector::calculateColumnCoverage(
    const Names & required_columns,
    const NamesAndTypesList & all_columns)
{
    if (all_columns.empty())
        return 0.0;
    
    // Count how many of the required columns are in all_columns
    size_t matched = 0;
    for (const auto & req_col : required_columns)
    {
        for (const auto & col : all_columns)
        {
            if (col.name == req_col)
            {
                ++matched;
                break;
            }
        }
    }
    
    return static_cast<double>(matched) / all_columns.size();
}

}

