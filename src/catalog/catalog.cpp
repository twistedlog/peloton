//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// catalog.cpp
//
// Identification: src/catalog/catalog.cpp
//
// Copyright (c) 2015-17, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#include "catalog/catalog.h"

#include <iostream>

#include "catalog/manager.h"
#include "common/exception.h"
#include "common/macros.h"
#include "expression/date_functions.h"
#include "expression/string_functions.h"
#include "index/index_factory.h"
#include "util/string_util.h"

namespace peloton {
namespace catalog {

// Get instance of the global catalog
Catalog *Catalog::GetInstance(void) {
  static std::unique_ptr<Catalog> global_catalog(new Catalog());
  return global_catalog.get();
}

Catalog::Catalog() {
  // Initialization of catalog, including:
  // 1) create pg_catalog database, create catalog tables, add them into
  // pg_catalog database, insert columns into pg_attribute
  // 2) insert pg_catalog into pg_database, catalog tables into pg_table
  // 3) create necessary indexes, insert into pg_index
  // when logging is enabled, this should be changed
  InitializeCatalog();

  // Create metrics table in default database
  // TODO: stats?
  CreateMetricsCatalog();

  InitializeFunctions();
}

void Catalog::InitializeCatalog() {
  // Create pg_catalog database
  auto pg_catalog = new storage::Database(CATALOG_DATABASE_OID);
  pg_catalog->setDBName(CATALOG_DATABASE_NAME);
  databases_.push_back(pg_catalog);

  // Create catalog tables, add into pg_catalog database, insert columns into
  // pg_attribute
  auto pg_database = DatabaseCatalog::GetInstance(pg_catalog, pool_.get());
  auto pg_table = TableCatalog::GetInstance(pg_catalog, pool_.get());
  IndexCatalog::GetInstance(pg_catalog, pool_.get());
  //  ColumnCatalog::GetInstance();
  // Called implicitly

  // Insert pg_catalog database into pg_database
  pg_database->InsertDatabase(CATALOG_DATABASE_OID, CATALOG_DATABASE_NAME,
                              pool_.get(), nullptr);

  // Insert catalog tables into pg_table
  pg_table->InsertTable(DATABASE_CATALOG_OID, DATABASE_CATALOG_NAME,
                        CATALOG_DATABASE_OID, CATALOG_DATABASE_NAME,
                        pool_.get(), nullptr);
  pg_table->InsertTable(TABLE_CATALOG_OID, TABLE_CATALOG_NAME,
                        CATALOG_DATABASE_OID, CATALOG_DATABASE_NAME,
                        pool_.get(), nullptr);
  pg_table->InsertTable(INDEX_CATALOG_OID, INDEX_CATALOG_NAME,
                        CATALOG_DATABASE_OID, CATALOG_DATABASE_NAME,
                        pool_.get(), nullptr);
  pg_table->InsertTable(COLUMN_CATALOG_OID, COLUMN_CATALOG_NAME,
                        CATALOG_DATABASE_OID, CATALOG_DATABASE_NAME,
                        pool_.get(), nullptr);

  // Create indexes on catalog tables, insert them into pg_index
  // TODO: This should be hash index rather than tree index?? (postgres use
  // btree!!)
  CreatePrimaryIndex(CATALOG_DATABASE_NAME, DATABASE_CATALOG_NAME);
  CreateIndex(CATALOG_DATABASE_NAME, DATABASE_CATALOG_NAME,
              std::vector<std::string>({1}), DATABASE_CATALOG_NAME + "_SKEY",
              true, IndexType::BWTREE);

  CreatePrimaryIndex(CATALOG_DATABASE_NAME, TABLE_CATALOG_NAME);
  CreateIndex(CATALOG_DATABASE_NAME, TABLE_CATALOG_NAME,
              std::vector<std::string>({1, 3}), TABLE_CATALOG_NAME + "_SKEY",
              true, IndexType::BWTREE);
  CreateIndex(CATALOG_DATABASE_NAME, TABLE_CATALOG_NAME,
              std::vector<std::string>({2}), TABLE_CATALOG_NAME + "_SKEY",
              false, IndexType::BWTREE);

  CreatePrimaryIndex(CATALOG_DATABASE_NAME, INDEX_CATALOG_NAME);
  // TODO: index secondary indexes??

  CreatePrimaryIndex(CATALOG_DATABASE_NAME, COLUMN_CATALOG_NAME);
  CreateIndex(CATALOG_DATABASE_NAME, COLUMN_CATALOG_NAME,
              std::vector<std::string>({0, 2}), COLUMN_CATALOG_NAME + "_SKEY",
              true, IndexType::BWTREE);
  CreateIndex(CATALOG_DATABASE_NAME, COLUMN_CATALOG_NAME,
              std::vector<std::string>({0}), COLUMN_CATALOG_NAME + "_SKEY",
              false, IndexType::BWTREE);
}

// Create a database
ResultType Catalog::CreateDatabase(const std::string &database_name,
                                   concurrency::Transaction *txn) {
  auto pg_database = DatabaseCatalog::GetInstance();
  // Check if a database with the same name exists
  oid_t database_oid = pg_database->GetDatabaseOid(database_name, txn);
  if (database_oid != INVALID_OID) {
    LOG_TRACE("Database already exists. Returning ResultType::FAILURE.");
    return ResultType::FAILURE;
  }

  // Create actual database
  database_oid = pg_database->GetNextOid();

  storage::Database *database = new storage::Database(database_oid);

  // TODO: This should be deprecated
  database->setDBName(database_name);

  {
    std::lock_guard<std::mutex> lock(catalog_mutex);
    databases_.push_back(database);
  }

  // Insert database tuple
  pg_database->InsertDatabase(database_oid, database_name, pool_.get(), txn);

  LOG_TRACE("Database created. Returning RESULT_SUCCESS.");
  return ResultType::SUCCESS;
}

// This should be deprecated! this can screw up the database oid system
void Catalog::AddDatabase(storage::Database *database) {
  std::lock_guard<std::mutex> lock(catalog_mutex);
  databases_.push_back(database);
  std::string database_name;
  DatabaseCatalog::GetInstance()->InsertDatabase(
      database->GetOid() | static_cast<oid_t>(type::CatalogType::DATABASE),
      database_name, pool_.get(), nullptr);
}

// Create a table in a database
ResultType Catalog::CreateTable(std::string database_name,
                                std::string table_name,
                                std::unique_ptr<catalog::Schema> schema,
                                concurrency::Transaction *txn) {
  LOG_TRACE("Creating table %s in database %s", table_name.c_str(),
            database_name.c_str());

  storage::Database *database = GetDatabaseWithName(database_name);
  if (database == nullptr) {
    LOG_TRACE("Can't found database. Returning RESULT_FAILURE");
    return ResultType::FAILURE;
  }

  storage::Database *table = database->GetTableWithName(table_name);
  if (table != nullptr) {
    LOG_TRACE("Found a table with the same name. Returning RESULT_FAILURE");
    return ResultType::FAILURE;
  } else {
    // Table doesn't exist, now create it
    bool own_schema = true;
    bool adapt_table = false;
    oid_t database_oid = database->GetOid();
    oid_t table_oid = TableCatalog::GetInstance()->GetNextOid();
    storage::DataTable *table = storage::TableFactory::GetDataTable(
        database_oid, table_oid, schema.release(), table_name,
        DEFAULT_TUPLES_PER_TILEGROUP, own_schema, adapt_table);
    database->AddTable(table);

    // Update pg_table with this table info
    TableCatalog::GetInstance()->InsertTable(
        table_oid, table_name, database_oid, database_name, pool_.get(), txn);

    // Create the primary key index for that table if there's primary key
    // Update pg_index, pg_attribute at the same time
    bool has_primary_key = false;
    auto &schema_columns = table->GetSchema()->GetColumns();

    for (auto &column : schema_columns) {
      ColumnCatalog::GetInstance()->InsertColumn(
          table_oid, column.GetName(), column.GetOffset(), column.GetType(),
          column.IsInlined(), column.GetConstraints(), pool_.get(), txn);

      if (column.IsPrimary()) {
        has_primary_key = true;
      }
    }

    if (has_primary_key == true)
      auto result = CreatePrimaryIndex(database_name, table_name);
    // if createPrimaryKey succeed, then return success
    return result;
  }
}

ResultType Catalog::CreatePrimaryIndex(const std::string &database_name,
                                       const std::string &table_name) {
  LOG_TRACE("Trying to create primary index for table %s", table_name.c_str());
  storage::Database *database = GetDatabaseWithName(database_name);
  if (database) {
    auto table = database->GetTableWithName(table_name);
    if (table == nullptr) {
      LOG_TRACE(
          "Cannot find the table to create the primary key index. Return "
          "RESULT_FAILURE.");
      return ResultType::FAILURE;
    }

    std::vector<oid_t> key_attrs;
    catalog::Schema *key_schema = nullptr;
    index::IndexMetadata *index_metadata = nullptr;
    auto schema = table->GetSchema();

    // Find primary index attributes
    int column_idx = 0;
    auto &schema_columns = schema->GetColumns();
    for (auto &column : schema_columns) {
      if (column.IsPrimary()) {
        key_attrs.push_back(column_idx);
      }
      column_idx++;
    }

    key_schema = catalog::Schema::CopySchema(schema, key_attrs);
    key_schema->SetIndexedColumns(key_attrs);

    std::string index_name = table->GetName() + "_PKEY";

    bool unique_keys = true;

    index_metadata = new index::IndexMetadata(
        StringUtil::Upper(index_name), GetNextOid(), table->GetOid(),
        database->GetOid(), IndexType::BWTREE, IndexConstraintType::PRIMARY_KEY,
        schema, key_schema, key_attrs, unique_keys);

    std::shared_ptr<index::Index> pkey_index(
        index::IndexFactory::GetIndex(index_metadata));
    table->AddIndex(pkey_index);

    LOG_TRACE("Successfully created primary key index '%s' for table '%s'",
              pkey_index->GetName().c_str(), table->GetName().c_str());
    return ResultType::SUCCESS;
  } else {
    LOG_TRACE("Could not find a database with name %s", database_name.c_str());
    return ResultType::FAILURE;
  }
}

// Function to add non-primary Key index
ResultType Catalog::CreateIndex(const std::string &database_name,
                                const std::string &table_name,
                                std::vector<std::string> index_attr,
                                std::string index_name, bool unique_keys,
                                IndexType index_type) {
  oid_t database_oid =
      DatabaseCatalog::GetInstance()->GetDatabaseOid(database_name, nullptr);
  if (database_oid == INVALID_OID) {
    LOG_TRACE(
        "Cannot find the database to create the primary key index. Return "
        "RESULT_FAILURE.");
    return ResultType::FAILURE;
  }

  oid_t table_oid =
      TableCatalog::GetInstance()->GetTableOid(table_name, nullptr);
  if (table_oid == INVALID_OID) {
    LOG_TRACE(
        "Cannot find the table to create the primary key index. Return "
        "RESULT_FAILURE.");
    return ResultType::FAILURE;
  }

  auto database = GetDatabaseWithOid(database_oid);
  if (database == nullptr) {
    LOG_TRACE(
        "Cannot find the database to create the primary key index. Return "
        "RESULT_FAILURE.");
    return ResultType::FAILURE;
  }

  auto table = database->GetTableWithOid(table_oid);
  if (table == nullptr) {
    LOG_TRACE(
        "Cannot find the table to create the primary key index. Return "
        "RESULT_FAILURE.");
    return ResultType::FAILURE;
  }

  std::vector<oid_t> key_attrs;
  catalog::Schema *key_schema = nullptr;
  index::IndexMetadata *index_metadata = nullptr;
  auto schema = table->GetSchema();

  // check if index attributes are in table
  auto &columns = schema->GetColumns();
  for (auto attr : index_attr) {
    for (uint i = 0; i < columns.size(); ++i) {
      if (attr == columns[i].column_name) {
        key_attrs.push_back(i);
      }
    }
  }

  // Check for mismatch between key attributes and attributes
  // that came out of the parser
  if (key_attrs.size() != index_attr.size()) {
    LOG_TRACE("Some columns are missing");
    return ResultType::FAILURE;
  }

  key_schema = catalog::Schema::CopySchema(schema, key_attrs);
  key_schema->SetIndexedColumns(key_attrs);
  oid_t index_oid = IndexCatalog::GetInstance()->GetNextOid();

  // Check if unique index or not
  if (unique_keys == false) {
    index_metadata = new index::IndexMetadata(
        index_name.c_str(), index_oid, table->GetOid(), database->GetOid(),
        index_type, IndexConstraintType::DEFAULT, schema, key_schema, key_attrs,
        true);
  } else {
    index_metadata = new index::IndexMetadata(
        index_name.c_str(), index_oid, table->GetOid(), database->GetOid(),
        index_type, IndexConstraintType::UNIQUE, schema, key_schema, key_attrs,
        true);
  }

  // Add index to table
  std::shared_ptr<index::Index> key_index(
      index::IndexFactory::GetIndex(index_metadata));
  table->AddIndex(key_index);

  // Add index to catalog
  // TODO: add more columns into index catalog
  IndexCatalog::GetInstance()->Insert(index_oid, index_name, table_oid,
                                      database_oid, unique_keys, pool_.get(),
                                      txn);

  LOG_TRACE("Successfully add index for table %s", table->GetName().c_str());
  return ResultType::SUCCESS;
}

ResultType Catalog::DropIndex(const oid_t database_oid, const oid_t index_oid) {
  auto database = GetDatabaseWithOid(database_oid);
  if (database != nullptr) {
    LOG_TRACE("Cannot find database");
    return ResultType::FAILURE;
  }

  // find table_oid by looking up pg_index using index_oid
  // txn is nullptr, one sentence Transaction
  oid_t table_oid =
      IndexCatalog::GetInstance()->GetTableidByOid(index_oid, nullptr);
  if (table_oid == INVALID_OID) {
    LOG_TRACE(
        "Cannot find the table to create the index. Return RESULT_FAILURE.");
    return ResultType::FAILURE;
  }

  auto table = database->GetTableWithOid(table_oid);
  if (table == nullptr) {
    LOG_TRACE(
        "Cannot find the table to create the index. Return RESULT_FAILURE.");
    return ResultType::FAILURE;
  }
  // drop index in actual table
  table->DropIndexWithOid(index_oid);

  // drop tuple in index catalog
  IndexCatalog::GetInstance()->DeleteByOid(index_oid, nullptr);

  LOG_TRACE("Successfully add index for table %s", table->GetName().c_str());
  return ResultType::SUCCESS;
}

index::Index *Catalog::GetIndexWithOid(const oid_t database_oid,
                                       const oid_t table_oid,
                                       const oid_t index_oid) const {
  // Lookup table
  auto table = GetTableWithOid(database_oid, table_oid);

  // Lookup index
  if (table != nullptr) {
    auto index = table->GetIndexWithOid(index_oid);
    return index.get();
  }

  return nullptr;
}

// Drop a database, only for test purposes
ResultType Catalog::DropDatabaseWithName(std::string &database_name,
                                         concurrency::Transaction *txn) {
  oid_t database_oid =
      DatabaseCatalog::GetInstance()->GetDatabaseOid(database_name, txn);
  if (database_oid == INVALID_OID) {
    LOG_TRACE("Database is not found!");
    return ResultType::FAILURE;
  }

  // Drop database record in catalog
  LOG_TRACE("Deleting tuple from catalog");
  if (!DatabaseCatalog::GetInstance()->DeleteDatabase(database_oid, txn)) {
    LOG_TRACE("Database tuple is not found!");
    return ResultType::FAILURE;
  }
  return ResultType::SUCCESS;
}

// Drop a database with its oid
ResultType Catalog::DropDatabaseWithOid(const oid_t database_oid,
                                        concurrency::Transaction *txn) {
  // Drop actual tables in the database
  auto table_oids =
      TableCatalog::GetInstance()->GetTableOids(database_oid, txn);
  for (auto table_oid : table_oids) {
    DropTableWithOid(database_oid, table_oid, txn);
  }

  // Drop database record in catalog
  LOG_TRACE("Deleting tuple from catalog");
  if (!DatabaseCatalog::GetInstance()->DeleteDatabase(database_oid, txn)) {
    LOG_TRACE("Database tuple is not found!");
    return ResultType::FAILURE;
  }

  // Drop actual database object
  LOG_TRACE("Dropping database with oid: %d", database_oid);
  bool found_database = false;
  std::lock_guard<std::mutex> lock(catalog_mutex);
  for (auto it = databases_.begin(); it != databases_.end(); ++it) {
    if (it->GetOid() == database_oid) {
      LOG_TRACE("Deleting database object in database vector");
      delete (*it);
      databases_.erase(database);
      found_database = true;
      break;
    }
  }
  if (!found_database) {
    LOG_TRACE("Database is not found!");
    return ResultType::FAILURE;
  }
  return ResultType::SUCCESS;
}

// Drop a table, CHANGING
ResultType Catalog::DropTable(std::string database_name, std::string table_name,
                              concurrency::Transaction *txn) {
  LOG_TRACE("Dropping table %s from database %s", table_name.c_str(),
            database_name.c_str());

  storage::Database *database = GetDatabaseWithName(database_name);
  if (database == nullptr) {
    LOG_TRACE("Can't Found database!");
    return ResultType::FAILURE;
  }

  storage::DataTable *table = database->GetTableWithName(table_name);
  if (database == nullptr) {
    LOG_TRACE("Can't Found Table!");
    return ResultType::FAILURE;
  }

  LOG_TRACE("Found table!");
  oid_t table_oid = table->GetOid();

  // drop actual data table
  // requires lock in DropTableWithOid() methods
  // cleanup schema, foreign keys, tile_groups, also delete Indexes that belong
  // to the table
  LOG_TRACE("Deleting table!");
  // TODO: data_table has DropIndexes(), but indexTuner also has
  // DropIndexes(table)
  table->DropIndexes();
  database->DropTableWithOid(table_oid);

  // change metadata
  LOG_TRACE("Deleting tuple from catalog!");
  // delete record in pg_table
  TableCatalog::GetInstance()->DeleteTable(table_oid, txn);
  // delete records in pg_attribute
  auto &schema_columns = table->GetSchema()->GetColumns();
  for (auto &column : schema_columns) {
    // std::string column_name = column.GetName();
    ColumnCatalog::GetInstance()->DeleteColumn(table_oid, column.GetName(),
                                               txn);
  }
  // TODO: delete records in pg_index

  return ResultType::SUCCESS;
}

// Drop a table, CHANGING
ResultType Catalog::DropTableWithOid(const oid_t database_oid,
                                     const oid_t table_oid,
                                     concurrency::Transaction *txn) {
  LOG_TRACE("Dropping table %d from database %d", database_oid, table_oid);

  storage::Database *database = GetDatabaseWithOid(database_oid);
  if (database == nullptr) {
    LOG_TRACE("Can't Found database!");
    return ResultType::FAILURE;
  }

  storage::DataTable *table = database->GetTableWithOid(table_oid);
  if (database == nullptr) {
    LOG_TRACE("Can't Found Table!");
    return ResultType::FAILURE;
  }

  LOG_TRACE("Found table!");

  // drop actual data table
  // requires lock in DropTableWithOid() methods
  // cleanup schema, foreign keys, tile_groups, also delete Indexes that belong
  // to the table
  LOG_TRACE("Deleting table!");
  // TODO: data_table has DropIndexes(), but indexTuner also has
  // DropIndexes(table), do we need mutex lock when droping index??
  table->DropIndexes();
  database->DropTableWithOid(table_oid);

  // change metadata
  LOG_TRACE("Deleting tuple from catalog!");
  // delete record in pg_table
  TableCatalog::GetInstance()->DeleteTable(table_oid, txn);
  // delete records in pg_attribute
  auto &schema_columns = table->GetSchema()->GetColumns();
  for (auto &column : schema_columns) {
    // std::string column_name = column.GetName();
    ColumnCatalog::GetInstance()->DeleteColumn(table_oid, column.GetName(),
                                               txn);
  }
  // TODO: delete records in pg_index

  return ResultType::SUCCESS;
}

// Only used for testing
bool Catalog::HasDatabase(const oid_t db_oid) const {
  return (GetDatabaseWithOid(db_oid) != nullptr);
}

// Find a database using its id
storage::Database *Catalog::GetDatabaseWithOid(const oid_t db_oid) const {
  for (auto database : databases_)
    if (database->GetOid() == db_oid) return database;
  return nullptr;
}

// Find a database using its name. TODO: This should be deprecated
// GetOidByName(string database_name, Transaction *txn)
storage::Database *Catalog::GetDatabaseWithName(
    const std::string database_name) const {
  oid_t database_oid =
      DatabaseCatalog::GetInstance()->GetDatabaseOid(database_name, nullptr);
  return GetDatabaseWithOid(database_oid);
}

storage::Database *Catalog::GetDatabaseWithOffset(
    const oid_t database_offset) const {
  PL_ASSERT(database_offset < databases_.size());
  auto database = databases_.at(database_offset);
  return database;
}

// Get table from a database
storage::DataTable *Catalog::GetTableWithName(std::string database_name,
                                              std::string table_name) {
  LOG_TRACE("Looking for table %s in database %s", table_name.c_str(),
            database_name.c_str());
  storage::Database *database = GetDatabaseWithName(database_name);
  if (database != nullptr) {
    storage::DataTable *table = database->GetTableWithName(table_name);
    if (table) {
      LOG_TRACE("Found table.");
      return table;
    } else {
      LOG_TRACE("Couldn't find table.");
      return nullptr;
    }
  } else {
    LOG_TRACE("Well, database wasn't found in the first place.");
    return nullptr;
  }
}

storage::DataTable *Catalog::GetTableWithOid(const oid_t database_oid,
                                             const oid_t table_oid) const {
  LOG_TRACE("Getting table with oid %d from database with oid %d", database_oid,
            table_oid);
  // Lookup DB
  auto database = GetDatabaseWithOid(database_oid);

  // Lookup table
  if (database != nullptr) {
    auto table = database->GetTableWithOid(table_oid);
    return table;
  }

  return nullptr;
}

oid_t Catalog::GetDatabaseCount() { return databases_.size(); }

Catalog::~Catalog() {
  delete GetDatabaseWithName(CATALOG_DATABASE_NAME);

  delete pool_;
}

//===--------------------------------------------------------------------===//
// METRIC
//===--------------------------------------------------------------------===//

void Catalog::CreateMetricsCatalog() {
  auto default_db = GetDatabaseWithName(CATALOG_DATABASE_NAME);
  auto default_db_oid = default_db->GetOid();

  // Create table for database metrics
  auto database_metrics_catalog =
      CreateMetricsCatalog(default_db_oid, DATABASE_METRIC_NAME);
  default_db->AddTable(database_metrics_catalog.release(), true);

  // Create table for index metrics
  auto index_metrics_catalog =
      CreateMetricsCatalog(default_db_oid, INDEX_METRIC_NAME);
  default_db->AddTable(index_metrics_catalog.release(), true);

  // Create table for table metrics
  auto table_metrics_catalog =
      CreateMetricsCatalog(default_db_oid, TABLE_METRIC_NAME);
  default_db->AddTable(table_metrics_catalog.release(), true);

  // Create table for query metrics
  auto query_metrics_catalog =
      CreateMetricsCatalog(default_db_oid, QUERY_METRIC_NAME);
  default_db->AddTable(query_metrics_catalog.release(), true);
  LOG_TRACE("Metrics tables created");
}

// Create table for metrics tables
std::unique_ptr<storage::DataTable> Catalog::CreateMetricsCatalog(
    oid_t database_id, std::string table_name) {
  bool own_schema = true;
  bool adapt_table = false;
  bool is_catalog = true;
  auto table_schema = InitializeQueryMetricsSchema();

  catalog::Schema *schema = nullptr;
  if (table_name == QUERY_METRIC_NAME) {
    schema = InitializeQueryMetricsSchema().release();
  } else if (table_name == TABLE_METRIC_NAME) {
    schema = InitializeTableMetricsSchema().release();
  } else if (table_name == DATABASE_METRIC_NAME) {
    schema = InitializeDatabaseMetricsSchema().release();
  } else if (table_name == INDEX_METRIC_NAME) {
    schema = InitializeIndexMetricsSchema().release();
  }

  std::unique_ptr<storage::DataTable> table(storage::TableFactory::GetDataTable(
      database_id, GetNextOid(), schema, table_name,
      DEFAULT_TUPLES_PER_TILEGROUP, own_schema, adapt_table, is_catalog));

  return table;
}

std::unique_ptr<catalog::Schema> Catalog::InitializeDatabaseMetricsSchema() {
  const std::string not_null_constraint_name = "not_null";
  catalog::Constraint not_null_constraint(ConstraintType::NOTNULL,
                                          not_null_constraint_name);
  oid_t integer_type_size = type::Type::GetTypeSize(type::Type::INTEGER);
  type::Type::TypeId integer_type = type::Type::INTEGER;

  auto id_column =
      catalog::Column(integer_type, integer_type_size, "database_id", true);
  id_column.AddConstraint(not_null_constraint);
  auto txn_committed_column =
      catalog::Column(integer_type, integer_type_size, "txn_committed", true);
  txn_committed_column.AddConstraint(not_null_constraint);
  auto txn_aborted_column =
      catalog::Column(integer_type, integer_type_size, "txn_aborted", true);
  txn_aborted_column.AddConstraint(not_null_constraint);

  auto timestamp_column =
      catalog::Column(integer_type, integer_type_size, "time_stamp", true);
  timestamp_column.AddConstraint(not_null_constraint);

  std::unique_ptr<catalog::Schema> database_schema(new catalog::Schema(
      {id_column, txn_committed_column, txn_aborted_column, timestamp_column}));
  return database_schema;
}

std::unique_ptr<catalog::Schema> Catalog::InitializeTableMetricsSchema() {
  const std::string not_null_constraint_name = "not_null";
  catalog::Constraint not_null_constraint(ConstraintType::NOTNULL,
                                          not_null_constraint_name);
  oid_t integer_type_size = type::Type::GetTypeSize(type::Type::INTEGER);
  type::Type::TypeId integer_type = type::Type::INTEGER;

  auto database_id_column =
      catalog::Column(integer_type, integer_type_size, "database_id", true);
  database_id_column.AddConstraint(not_null_constraint);
  auto table_id_column =
      catalog::Column(integer_type, integer_type_size, "table_id", true);
  table_id_column.AddConstraint(not_null_constraint);

  auto reads_column =
      catalog::Column(integer_type, integer_type_size, "reads", true);
  reads_column.AddConstraint(not_null_constraint);
  auto updates_column =
      catalog::Column(integer_type, integer_type_size, "updates", true);
  updates_column.AddConstraint(not_null_constraint);
  auto deletes_column =
      catalog::Column(integer_type, integer_type_size, "deletes", true);
  deletes_column.AddConstraint(not_null_constraint);
  auto inserts_column =
      catalog::Column(integer_type, integer_type_size, "inserts", true);
  inserts_column.AddConstraint(not_null_constraint);

  // MAX_INT only tracks the number of seconds since epoch until 2037
  auto timestamp_column =
      catalog::Column(integer_type, integer_type_size, "time_stamp", true);
  timestamp_column.AddConstraint(not_null_constraint);

  std::unique_ptr<catalog::Schema> database_schema(new catalog::Schema(
      {database_id_column, table_id_column, reads_column, updates_column,
       deletes_column, inserts_column, timestamp_column}));
  return database_schema;
}

std::unique_ptr<catalog::Schema> Catalog::InitializeIndexMetricsSchema() {
  const std::string not_null_constraint_name = "not_null";
  catalog::Constraint not_null_constraint(ConstraintType::NOTNULL,
                                          not_null_constraint_name);
  oid_t integer_type_size = type::Type::GetTypeSize(type::Type::INTEGER);
  type::Type::TypeId integer_type = type::Type::INTEGER;

  auto database_id_column =
      catalog::Column(integer_type, integer_type_size, "database_id", true);
  database_id_column.AddConstraint(not_null_constraint);
  auto table_id_column =
      catalog::Column(integer_type, integer_type_size, "table_id", true);
  table_id_column.AddConstraint(not_null_constraint);
  auto index_id_column =
      catalog::Column(integer_type, integer_type_size, "index_id", true);
  index_id_column.AddConstraint(not_null_constraint);

  auto reads_column =
      catalog::Column(integer_type, integer_type_size, "reads", true);
  reads_column.AddConstraint(not_null_constraint);
  auto deletes_column =
      catalog::Column(integer_type, integer_type_size, "deletes", true);
  deletes_column.AddConstraint(not_null_constraint);
  auto inserts_column =
      catalog::Column(integer_type, integer_type_size, "inserts", true);
  inserts_column.AddConstraint(not_null_constraint);

  auto timestamp_column =
      catalog::Column(integer_type, integer_type_size, "time_stamp", true);
  timestamp_column.AddConstraint(not_null_constraint);

  std::unique_ptr<catalog::Schema> database_schema(new catalog::Schema(
      {database_id_column, table_id_column, index_id_column, reads_column,
       deletes_column, inserts_column, timestamp_column}));
  return database_schema;
}

std::unique_ptr<catalog::Schema> Catalog::InitializeQueryMetricsSchema() {
  const std::string not_null_constraint_name = "not_null";
  catalog::Constraint not_null_constraint(ConstraintType::NOTNULL,
                                          not_null_constraint_name);
  oid_t integer_type_size = type::Type::GetTypeSize(type::Type::INTEGER);
  oid_t varbinary_type_size = type::Type::GetTypeSize(type::Type::VARBINARY);

  type::Type::TypeId integer_type = type::Type::INTEGER;
  type::Type::TypeId varbinary_type = type::Type::VARBINARY;

  auto name_column = catalog::Column(
      type::Type::VARCHAR, type::Type::GetTypeSize(type::Type::VARCHAR),
      "query_name", false);
  name_column.AddConstraint(not_null_constraint);
  auto database_id_column =
      catalog::Column(integer_type, integer_type_size, "database_id", true);
  database_id_column.AddConstraint(not_null_constraint);

  // Parameters
  auto num_param_column = catalog::Column(integer_type, integer_type_size,
                                          QUERY_NUM_PARAM_COL_NAME, true);
  num_param_column.AddConstraint(not_null_constraint);
  // For varbinary types, we don't want to inline it since it could be large
  auto param_type_column = catalog::Column(varbinary_type, varbinary_type_size,
                                           QUERY_PARAM_TYPE_COL_NAME, false);
  auto param_format_column = catalog::Column(
      varbinary_type, varbinary_type_size, QUERY_PARAM_FORMAT_COL_NAME, false);
  auto param_val_column = catalog::Column(varbinary_type, varbinary_type_size,
                                          QUERY_PARAM_VAL_COL_NAME, false);

  // Physical statistics
  auto reads_column =
      catalog::Column(integer_type, integer_type_size, "reads", true);
  reads_column.AddConstraint(not_null_constraint);
  auto updates_column =
      catalog::Column(integer_type, integer_type_size, "updates", true);
  updates_column.AddConstraint(not_null_constraint);
  auto deletes_column =
      catalog::Column(integer_type, integer_type_size, "deletes", true);
  deletes_column.AddConstraint(not_null_constraint);
  auto inserts_column =
      catalog::Column(integer_type, integer_type_size, "inserts", true);
  inserts_column.AddConstraint(not_null_constraint);
  auto latency_column =
      catalog::Column(integer_type, integer_type_size, "latency", true);
  latency_column.AddConstraint(not_null_constraint);
  auto cpu_time_column =
      catalog::Column(integer_type, integer_type_size, "cpu_time", true);

  // MAX_INT only tracks the number of seconds since epoch until 2037
  auto timestamp_column =
      catalog::Column(integer_type, integer_type_size, "time_stamp", true);
  timestamp_column.AddConstraint(not_null_constraint);

  std::unique_ptr<catalog::Schema> database_schema(new catalog::Schema(
      {name_column, database_id_column, num_param_column, param_type_column,
       param_format_column, param_val_column, reads_column, updates_column,
       deletes_column, inserts_column, latency_column, cpu_time_column,
       timestamp_column}));
  return database_schema;
}

//===--------------------------------------------------------------------===//
// FUNCTION
//===--------------------------------------------------------------------===//

void Catalog::AddFunction(
    const std::string &name,
    const std::vector<type::Type::TypeId> &argument_types,
    const type::Type::TypeId return_type,
    type::Value (*func_ptr)(const std::vector<type::Value> &)) {
  PL_ASSERT(functions_.count(name) == 0);
  functions_[name] = FunctionData{name, argument_types, return_type, func_ptr};
}

const FunctionData Catalog::GetFunction(const std::string &name) {
  if (functions_.count(name) == 0) {
    throw Exception("function " + name + " not found.");
  }
  return functions_[name];
}

void Catalog::RemoveFunction(const std::string &name) {
  functions_.erase(name);
}

void Catalog::InitializeFunctions() {
  /**
   * string functions
   */
  AddFunction("ascii", {type::Type::VARCHAR}, type::Type::INTEGER,
              expression::StringFunctions::Ascii);
  AddFunction("chr", {type::Type::INTEGER}, type::Type::VARCHAR,
              expression::StringFunctions::Chr);
  AddFunction("substr",
              {type::Type::VARCHAR, type::Type::INTEGER, type::Type::INTEGER},
              type::Type::VARCHAR, expression::StringFunctions::Substr);
  AddFunction("concat", {type::Type::VARCHAR, type::Type::VARCHAR},
              type::Type::VARCHAR, expression::StringFunctions::Concat);
  AddFunction("char_length", {type::Type::VARCHAR}, type::Type::INTEGER,
              expression::StringFunctions::CharLength);
  AddFunction("octet_length", {type::Type::VARCHAR}, type::Type::INTEGER,
              expression::StringFunctions::OctetLength);
  AddFunction("repeat", {type::Type::VARCHAR, type::Type::INTEGER},
              type::Type::VARCHAR, expression::StringFunctions::Repeat);
  AddFunction("replace",
              {type::Type::VARCHAR, type::Type::VARCHAR, type::Type::VARCHAR},
              type::Type::VARCHAR, expression::StringFunctions::Replace);
  AddFunction("ltrim", {type::Type::VARCHAR, type::Type::VARCHAR},
              type::Type::VARCHAR, expression::StringFunctions::LTrim);
  AddFunction("rtrim", {type::Type::VARCHAR, type::Type::VARCHAR},
              type::Type::VARCHAR, expression::StringFunctions::RTrim);
  AddFunction("btrim", {type::Type::VARCHAR, type::Type::VARCHAR},
              type::Type::VARCHAR, expression::StringFunctions::BTrim);

  /**
   * date functions
   */
  AddFunction("extract", {type::Type::INTEGER, type::Type::TIMESTAMP},
              type::Type::DECIMAL, expression::DateFunctions::Extract);
}
}
}
