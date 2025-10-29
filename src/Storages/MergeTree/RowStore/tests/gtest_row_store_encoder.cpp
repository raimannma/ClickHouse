#include <gtest/gtest.h>
#include <Storages/MergeTree/RowStore/RowStoreEncoder.h>
#include <Core/Block.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromString.h>

using namespace DB;

class RowStoreEncoderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a simple schema
        columns.emplace_back("id", std::make_shared<DataTypeUInt64>());
        columns.emplace_back("name", std::make_shared<DataTypeString>());
        columns.emplace_back("value", std::make_shared<DataTypeUInt32>());
    }

    NamesAndTypesList columns;
};

TEST_F(RowStoreEncoderTest, EncodeDecodeSimpleRow)
{
    RowStoreEncoder::RowStoreSettings settings;
    RowStoreEncoder encoder(columns, settings);

    // Create a test block
    Block block;
    
    auto id_col = ColumnUInt64::create();
    id_col->insert(123);
    block.insert(ColumnWithTypeAndName{std::move(id_col), std::make_shared<DataTypeUInt64>(), "id"});
    
    auto name_col = ColumnString::create();
    name_col->insert("test_name");
    block.insert(ColumnWithTypeAndName{std::move(name_col), std::make_shared<DataTypeString>(), "name"});
    
    auto value_col = ColumnUInt32::create();
    value_col->insert(456);
    block.insert(ColumnWithTypeAndName{std::move(value_col), std::make_shared<DataTypeUInt32>(), "value"});

    // Encode the row
    WriteBufferFromOwnString write_buf;
    encoder.encodeRow(block, 0, write_buf);
    
    String encoded = write_buf.str();
    EXPECT_GT(encoded.size(), 0);

    // Decode the row
    ReadBufferFromString read_buf(encoded);
    Block decoded = encoder.decodeRow(read_buf, columns);

    // Verify decoded data
    EXPECT_EQ(decoded.rows(), 1);
    EXPECT_EQ(decoded.columns(), 3);
    
    EXPECT_EQ(decoded.getByName("id").column->getUInt(0), 123);
    EXPECT_EQ(decoded.getByName("name").column->getDataAt(0).toString(), "test_name");
    EXPECT_EQ(decoded.getByName("value").column->getUInt(0), 456);
}

TEST_F(RowStoreEncoderTest, EstimateRowSize)
{
    RowStoreEncoder::RowStoreSettings settings;
    RowStoreEncoder encoder(columns, settings);

    // Create a test block
    Block block;
    
    auto id_col = ColumnUInt64::create();
    id_col->insert(123);
    block.insert(ColumnWithTypeAndName{std::move(id_col), std::make_shared<DataTypeUInt64>(), "id"});
    
    auto name_col = ColumnString::create();
    name_col->insert("test");
    block.insert(ColumnWithTypeAndName{std::move(name_col), std::make_shared<DataTypeString>(), "name"});
    
    auto value_col = ColumnUInt32::create();
    value_col->insert(456);
    block.insert(ColumnWithTypeAndName{std::move(value_col), std::make_shared<DataTypeUInt32>(), "value"});

    size_t estimated = encoder.estimateRowSize(block, 0);
    EXPECT_GT(estimated, 0);
    EXPECT_LT(estimated, 1000); // Should be reasonable size
}

TEST_F(RowStoreEncoderTest, PageAddRow)
{
    RowStorePage page;
    
    const char * row1 = "row1_data";
    const char * row2 = "row2_data";
    
    EXPECT_TRUE(page.addRow(row1, 9));
    EXPECT_TRUE(page.addRow(row2, 9));
    
    EXPECT_EQ(page.rowCount(), 2);
    EXPECT_EQ(page.size(), 18);
    
    auto [data1, size1] = page.getRow(0);
    EXPECT_EQ(size1, 9);
    EXPECT_EQ(std::string(data1, size1), "row1_data");
    
    auto [data2, size2] = page.getRow(1);
    EXPECT_EQ(size2, 9);
    EXPECT_EQ(std::string(data2, size2), "row2_data");
}

TEST_F(RowStoreEncoderTest, PageHasSpace)
{
    RowStorePage page;
    
    EXPECT_TRUE(page.hasSpace(100, 1000));
    
    // Add some data
    const char * data = "test_data";
    page.addRow(data, 9);
    
    EXPECT_TRUE(page.hasSpace(100, 1000));
    EXPECT_FALSE(page.hasSpace(1000, 100)); // Exceeds max size
}

TEST_F(RowStoreEncoderTest, PageWriteRead)
{
    RowStorePage page;
    
    const char * row1 = "row1_data";
    const char * row2 = "row2_data";
    page.addRow(row1, 9);
    page.addRow(row2, 9);
    
    // Write page
    WriteBufferFromOwnString write_buf;
    auto codec = CompressionCodecFactory::instance().get("LZ4");
    page.writeTo(write_buf, codec);
    
    String serialized = write_buf.str();
    EXPECT_GT(serialized.size(), 0);
    
    // Read page
    ReadBufferFromString read_buf(serialized);
    RowStorePage loaded_page;
    loaded_page.readFrom(read_buf, codec);
    
    EXPECT_EQ(loaded_page.rowCount(), 2);
    EXPECT_EQ(loaded_page.size(), 18);
    
    auto [data1, size1] = loaded_page.getRow(0);
    EXPECT_EQ(std::string(data1, size1), "row1_data");
}

TEST_F(RowStoreEncoderTest, PageManagerAddRows)
{
    RowStoreEncoder::RowStoreSettings settings;
    settings.page_size = 100; // Small page for testing
    
    RowStorePageManager manager(settings);
    
    const char * row1 = "row1";
    const char * row2 = "row2";
    const char * row3 = "row3";
    
    auto [page1, offset1] = manager.addRow(row1, 4);
    auto [page2, offset2] = manager.addRow(row2, 4);
    auto [page3, offset3] = manager.addRow(row3, 4);
    
    // All should be in same page initially
    EXPECT_EQ(page1, 0);
    EXPECT_EQ(page2, 0);
    EXPECT_EQ(page3, 0);
    
    EXPECT_EQ(offset1, 0);
    EXPECT_EQ(offset2, 1);
    EXPECT_EQ(offset3, 2);
}

TEST_F(RowStoreEncoderTest, PageManagerStatistics)
{
    RowStoreEncoder::RowStoreSettings settings;
    RowStorePageManager manager(settings);
    
    const char * row = "test_row_data";
    for (int i = 0; i < 10; ++i)
    {
        manager.addRow(row, 13);
    }
    
    auto stats = manager.getStatistics();
    EXPECT_EQ(stats.total_rows, 10);
    EXPECT_GT(stats.total_uncompressed_bytes, 0);
}

TEST_F(RowStoreEncoderTest, IndexAddFind)
{
    RowStoreIndex index;
    
    RowStoreIndexEntry entry1{0, 0, 100};
    RowStoreIndexEntry entry2{0, 1, 200};
    RowStoreIndexEntry entry3{1, 0, 300};
    
    index.addEntry(entry1);
    index.addEntry(entry2);
    index.addEntry(entry3);
    
    EXPECT_EQ(index.size(), 3);
    
    auto found = index.findByGranule(200);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->page_id, 0);
    EXPECT_EQ(found->row_offset_in_page, 1);
    
    auto not_found = index.findByGranule(999);
    EXPECT_FALSE(not_found.has_value());
}

TEST_F(RowStoreEncoderTest, IndexWriteRead)
{
    RowStoreIndex index;
    
    RowStoreIndexEntry entry1{0, 0, 100};
    RowStoreIndexEntry entry2{1, 5, 200};
    
    index.addEntry(entry1);
    index.addEntry(entry2);
    
    // Write index
    WriteBufferFromOwnString write_buf;
    index.writeTo(write_buf);
    
    String serialized = write_buf.str();
    EXPECT_GT(serialized.size(), 0);
    
    // Read index
    ReadBufferFromString read_buf(serialized);
    RowStoreIndex loaded_index;
    loaded_index.readFrom(read_buf);
    
    EXPECT_EQ(loaded_index.size(), 2);
    
    auto found = loaded_index.findByGranule(200);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->page_id, 1);
    EXPECT_EQ(found->row_offset_in_page, 5);
}

int main(int argc, char ** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

