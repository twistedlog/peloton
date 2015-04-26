/**
 * @brief Test cases for materialization node.
 *
 * Copyright(c) 2015, CMU
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../src/storage/backend_vm.h"
#include "gtest/gtest.h"

#include "catalog/manager.h"
#include "catalog/schema.h"
#include "common/types.h"
#include "common/value_factory.h"
#include "executor/logical_tile.h"
#include "executor/logical_tile_factory.h"
#include "executor/materialization_executor.h"
#include "planner/abstract_plan_node.h"
#include "planner/materialization_node.h"
#include "storage/tile.h"
#include "storage/tile_group.h"
#include "storage/tuple.h"
#include "executor/executor_tests_util.h"
#include "executor/mock_executor.h"
#include "harness.h"

using ::testing::IsNull;
using ::testing::NotNull;
using ::testing::Return;

namespace nstore {
namespace test {

namespace {

/**
 * @brief Populates the tiles in the given tile-group in a specific manner.
 * @param tile_group Tile-group to populate with values.
 * @param num_rows Number of tuples to insert.
 *
 * This is a convenience function for the test cases.
 */
void PopulateTiles(storage::TileGroup *tile_group, int num_rows) {
  // Create tuple schema from tile schemas.
  std::vector<catalog::Schema> &tile_schemas = tile_group->GetTileSchemas();
  std::unique_ptr<catalog::Schema> schema(
    catalog::Schema::AppendSchemaList(tile_schemas));

  // Ensure that the tile group created by ExecutorTestsUtil is as expected.
  assert(tile_schemas.size() == 2);
  assert(schema->GetColumnCount() == 4);

  // Insert tuples into tile_group.
  const bool allocate = true;
  const txn_id_t txn_id = GetTransactionId();
  for (int i = 0; i < num_rows; i++) {
    storage::Tuple tuple(schema.get(), allocate);
    tuple.SetValue(0, ValueFactory::GetIntegerValue(10 * i));
    tuple.SetValue(1, ValueFactory::GetIntegerValue(10 * i + 1));
    tuple.SetValue(2, ValueFactory::GetTinyIntValue(10 * i + 2));
    tuple.SetValue(
        3,
        ValueFactory::GetStringValue(std::to_string(10 * i + 3),
        tile_group->GetTilePool(1)));
    tile_group->InsertTuple(txn_id, &tuple);
  }
}

} // namespace

// "Pass-through" test case. There is nothing to materialize as
// there is only one base tile in the logical tile.
TEST(MaterializationTests, SingleBaseTileTest) {
  storage::VMBackend backend;
  const int tuple_count = 9;
  std::unique_ptr<storage::TileGroup> tile_group(
      ExecutorTestsUtil::CreateSimpleTileGroup(
        &backend,
        tuple_count));

  PopulateTiles(tile_group.get(), tuple_count);

  // Create logical tile from single base tile.
  storage::Tile *source_base_tile = tile_group->GetTile(0);
  const bool own_base_tiles = false;
  std::unique_ptr<executor::LogicalTile> source_logical_tile(
      executor::LogicalTileFactory::WrapBaseTiles(
        { source_base_tile },
        own_base_tiles));

  // Create materialization node for this test.
  std::unique_ptr<catalog::Schema> output_schema(catalog::Schema::CopySchema(
      &source_base_tile->GetSchema()));
  std::unordered_map<id_t, id_t> old_to_new_cols;

  unsigned int column_count = output_schema->GetColumnCount();
  for (id_t col = 0; col < column_count; col++) {
    // Create identity mapping.
    old_to_new_cols[col] = col;
  }
  planner::MaterializationNode node(
      std::move(old_to_new_cols),
      output_schema.release());

  // Pass them through materialization executor.
  executor::MaterializationExecutor executor(&node);
  MockExecutor child_executor;
  executor.AddChild(&child_executor);

  // Uneventful init...
  EXPECT_CALL(child_executor, SubInit())
    .WillOnce(Return(true));
  EXPECT_TRUE(executor.Init());

  // Where the main work takes place...
  EXPECT_CALL(child_executor, SubGetNextTile())
    .WillOnce(Return(source_logical_tile.release()))
    .WillOnce(Return(nullptr));

  std::unique_ptr<executor::LogicalTile> result_logical_tile(
    executor.GetNextTile());
  EXPECT_THAT(result_logical_tile, NotNull());
  EXPECT_THAT(executor.GetNextTile(), IsNull());

  // Verify that logical tile is only made up of a single base tile.
  int num_cols = result_logical_tile->NumCols();
  EXPECT_EQ(2, num_cols);
  storage::Tile *result_base_tile = result_logical_tile->GetBaseTile(0);
  EXPECT_THAT(result_base_tile, NotNull());
  EXPECT_TRUE(source_base_tile != result_base_tile);
  EXPECT_EQ(result_logical_tile->GetBaseTile(1), result_base_tile);

  // Check that the base tile has the correct values.
  for (int i = 0; i < tuple_count; i++) {
    EXPECT_EQ(ValueFactory::GetIntegerValue(10 * i),
              result_base_tile->GetValue(i, 0));
    EXPECT_EQ(ValueFactory::GetIntegerValue(10 * i + 1),
              result_base_tile->GetValue(i, 1));

    // Double check that logical tile is functioning.
    EXPECT_EQ(result_base_tile->GetValue(i, 0),
              result_logical_tile->GetValue(0, i));
    EXPECT_EQ(result_base_tile->GetValue(i, 1),
              result_logical_tile->GetValue(1, i));
  }
}

// Materializing logical tile composed of two base tiles.
// The materialized tile's output columns are reordered.
// Also, one of the columns is dropped.
TEST(MaterializationTests, TwoBaseTilesWithReorderTest) {
  storage::VMBackend backend;
  const int tuple_count = 9;
  std::unique_ptr<storage::TileGroup> tile_group(
      ExecutorTestsUtil::CreateSimpleTileGroup(
        &backend,
        tuple_count));

  PopulateTiles(tile_group.get(), tuple_count);

  // Create logical tile from two base tiles.
  const std::vector<storage::Tile *> source_base_tiles =
    { tile_group->GetTile(0), tile_group->GetTile(1) };
  const bool own_base_tiles = false;
  std::unique_ptr<executor::LogicalTile> source_tile(
      executor::LogicalTileFactory::WrapBaseTiles(
          source_base_tiles,
          own_base_tiles));

  // Create materialization node for this test.
  // Construct output schema. We drop column 3 and reorder the others to 3,1,0.
  std::vector<catalog::ColumnInfo> output_columns;
  // Note that Column 3 in the tile group is column 1 in the second tile.
  output_columns.push_back(source_base_tiles[1]->GetSchema().GetColumnInfo(1));
  output_columns.push_back(source_base_tiles[0]->GetSchema().GetColumnInfo(1));
  output_columns.push_back(source_base_tiles[0]->GetSchema().GetColumnInfo(0));
  std::unique_ptr<catalog::Schema> output_schema(
      new catalog::Schema(output_columns));

  // Construct mapping using the ordering mentioned above.
  std::unordered_map<id_t, id_t> old_to_new_cols;
  old_to_new_cols[3] = 0;
  old_to_new_cols[1] = 1;
  old_to_new_cols[0] = 2;
  planner::MaterializationNode node(
      std::move(old_to_new_cols),
      output_schema.release());

  // Pass them through materialization executor.
  executor::MaterializationExecutor executor(&node);
  MockExecutor child_executor;
  executor.AddChild(&child_executor);

  // Uneventful init...
  EXPECT_CALL(child_executor, SubInit())
    .WillOnce(Return(true));
  EXPECT_TRUE(executor.Init());

  // Where the main work takes place...
  EXPECT_CALL(child_executor, SubGetNextTile())
    .WillOnce(Return(source_tile.release()))
    .WillOnce(Return(nullptr));

  std::unique_ptr<executor::LogicalTile> result_logical_tile(
    executor.GetNextTile());
  EXPECT_THAT(result_logical_tile, NotNull());
  EXPECT_THAT(executor.GetNextTile(), IsNull());

  // Verify that logical tile is only made up of a single base tile.
  int num_cols = result_logical_tile->NumCols();
  EXPECT_EQ(3, num_cols);
  storage::Tile *result_base_tile = result_logical_tile->GetBaseTile(0);
  EXPECT_THAT(result_base_tile, NotNull());
  EXPECT_EQ(result_base_tile, result_logical_tile->GetBaseTile(1));
  EXPECT_EQ(result_base_tile, result_logical_tile->GetBaseTile(2));

  // Check that the base tile has the correct values.
  for (int i = 0; i < tuple_count; i++) {
    // Output column 2.
    EXPECT_EQ(ValueFactory::GetIntegerValue(10 * i),
              result_base_tile->GetValue(i, 2));
    // Output column 1.
    EXPECT_EQ(ValueFactory::GetIntegerValue(10 * i + 1),
              result_base_tile->GetValue(i, 1));
    // Output column 0.
    Value string_value(
        ValueFactory::GetStringValue(std::to_string(10 * i + 3)));
    EXPECT_EQ(string_value,
              result_base_tile->GetValue(i, 0));
    string_value.FreeUninlinedData();

    // Double check that logical tile is functioning.
    EXPECT_EQ(result_base_tile->GetValue(i, 0),
              result_logical_tile->GetValue(0, i));
    EXPECT_EQ(result_base_tile->GetValue(i, 1),
              result_logical_tile->GetValue(1, i));
    EXPECT_EQ(result_base_tile->GetValue(i, 2),
              result_logical_tile->GetValue(2, i));
  }
}

} // namespace test
} // namespace nstore