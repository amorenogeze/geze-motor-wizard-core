#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace wizard {

// -----------------------------------------------------------------------
// Server configuration — single source of truth for all runtime paths.
// -----------------------------------------------------------------------

constexpr const char* kGatewaySocketPath   = "/tmp/wizard-backend.sock";
constexpr const char* kUiCommandSocketPath = "/tmp/wizard-ui-command.sock";
constexpr const char* kDefaultDbPath       = "/var/lib/wizard-core/door_tuning_wizard.db";

// -----------------------------------------------------------------------
// Data type IDs — kept in sync with what seed_data_types() inserts into
// the Data_Type table. wizard-engine resolves them once at connect() and
// caches them here so inserts never need a lookup.
// -----------------------------------------------------------------------

struct DataTypeIds {
    int64_t position = -1;
    int64_t velocity = -1;
    int64_t current  = -1;
};

// -----------------------------------------------------------------------
// Database
//
// Owns the SQLite connection used by wizard-engine to persist telemetry.
// Written only by wizard-engine; read by the UI from the same file (WAL
// mode — safe for one writer + concurrent readers on the same board).
//
// Typical lifecycle:
//   Database db(path);
//   db.connect();
//   db.seed_static_data();             // once — idempotent
//   int64_t session_id = db.open_session();
//   int64_t run_id     = db.open_run(session_id, device_id);
//   db.insert_data(run_id, data_type_ids.position, timestamp_us, value);
//   db.close_run(run_id);
//   db.close_session(session_id);
//   db.disconnect();
// -----------------------------------------------------------------------

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens the SQLite connection and enables WAL mode. Throws on failure.
    void connect();

    // Closes the SQLite connection cleanly.
    void disconnect();

    // Seeds Status and Data_Type rows if not already present. Safe to call
    // on every startup — uses INSERT OR IGNORE, never duplicates rows.
    // Returns the resolved Data_Type IDs for position/velocity/current.
    DataTypeIds seed_static_data();

    // Inserts or retrieves the Device row for the given serial number.
    // Returns the Device.Id.
    int64_t ensure_device(const std::string& device_serial);

    // Opens a new Session row. Returns its Id.
    int64_t open_session();

    // Closes a Session (sets End_Date).
    void close_session(int64_t session_id);

    // Opens a new Runs row. Returns its Id.
    // config: JSON string with the run configuration (use "{}" if unknown).
    int64_t open_run(int64_t session_id, int64_t device_id,
                     const std::string& config = "{}");

    // Closes a Runs row (sets End_Time and Run_Status to stopped).
    void close_run(int64_t run_id);

    // Inserts one telemetry sample into Data.
    // data_type_id: one of the ids returned by seed_static_data().
    // timestamp_us: microseconds since epoch.
    // value: raw integer value (encoder counts, milliamps, etc.).
    void insert_data(int64_t run_id, int64_t data_type_id,
                     uint64_t timestamp_us, int32_t value);

private:
    void exec(const std::string& sql);
    sqlite3_stmt* prepare(const std::string& sql);
    int64_t last_insert_rowid() const;

    std::string path_;
    sqlite3*    db_ = nullptr;

    // Cached status IDs resolved by seed_static_data().
    int64_t status_id_running_ = -1;
    int64_t status_id_stopped_ = -1;
};

}  // namespace wizard

