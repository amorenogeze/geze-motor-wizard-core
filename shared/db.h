#pragma once

#include <cstdint>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace wizard {

constexpr const char* kDefaultDbPath = "/tmp/door_tuning_wizard.db";
constexpr const char* kGatewaySocketPath  = "/tmp/wizard-backend.sock";
constexpr const char* kUiCommandSocketPath = "/tmp/wizard-ui.sock";

// Ids of the telemetry channels in Data_Type, resolved by name at startup.
// -1 means the row is not present yet.
struct DataTypeIds {
    int64_t position = -1;
    int64_t speed = -1;
    int64_t current  = -1;
};

// Writes telemetry into a database owned and provisioned by the UI backend.
// This class never creates tables and never creates the database file.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // Opens the existing database. False if it is missing or unopenable.
    bool connect();
    void disconnect();

    // connect() plus a check that the tables this engine uses exist.
    // Safe to call in a loop while waiting for the backend to provision.
    bool validate();

    // Reason the last connect()/validate() returned false.
    const std::string& last_error() const { return last_error_; }

    // Channel ids, by name. Fields stay -1 for rows not yet seeded.
    DataTypeIds resolve_data_types();

    // Most recent run with no End_Time, or -1 if none is open.
    int64_t current_run_id();

    void insert_data(int64_t run_id, int64_t data_type_id,
                     uint64_t timestamp_us, int32_t value);

    int64_t last_insert_rowid() const;

private:
    void          exec(const std::string& sql);
    sqlite3_stmt* prepare(const std::string& sql);

    std::string path_;
    sqlite3*    db_ = nullptr;
    std::string last_error_;
    uint64_t    insert_failures_ = 0;
};

}  // namespace wizard
