#include "db.h"

#include <ctime>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>

namespace wizard {

Database::Database(const std::string& path) : path_(path) {}

Database::~Database() {
    disconnect();
}

// Opens the database WITHOUT SQLITE_OPEN_CREATE: the UI backend owns
// provisioning, so a missing file must stay missing rather than becoming an
// empty database the engine then fails against.
bool Database::connect() {
    if (db_) return true;

    const int rc = sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = "cannot open " + path_ + ": " +
                      std::string(db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    try {
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA busy_timeout=2000;");
        exec("PRAGMA foreign_keys=ON;");
    } catch (const std::exception& e) {
        last_error_ = e.what();
        disconnect();
        return false;
    }

    last_error_.clear();
    return true;
}

void Database::disconnect() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// True once the database exists and carries the tables this engine uses.
// Safe to call repeatedly from a wait loop.
bool Database::validate() {
    if (!connect()) return false;

    for (const char* table : {"Data_Type", "Data", "Runs"}) {
        sqlite3_stmt* stmt = nullptr;
        try {
            stmt = prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;");
        } catch (const std::exception& e) {
            last_error_ = e.what();
            disconnect();
            return false;
        }

        sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
        const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);

        if (!found) {
            last_error_ = std::string("missing table: ") + table;
            disconnect();
            return false;
        }
    }

    last_error_.clear();
    return true;
}

// Ids are resolved by name because another component owns the Data_Type rows
// and therefore decides the numbering. Returns -1 for any row not present yet.
DataTypeIds Database::resolve_data_types() {
    auto resolve = [&](const char* name) -> int64_t {
        sqlite3_stmt* stmt = prepare("SELECT id FROM Data_Type WHERE name = ?;");
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        int64_t id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    };

    DataTypeIds ids;
    if (!db_) return ids;

    ids.position = resolve("position");
    ids.velocity = resolve("velocity");
    ids.current  = resolve("current");
    return ids;
}

// The most recent run the backend has opened and not yet closed.
// -1 means there is nothing to attach telemetry to.
int64_t Database::current_run_id() {
    if (!db_) return -1;

    sqlite3_stmt* stmt = prepare(
        "SELECT Id FROM Runs WHERE End_Time IS NULL ORDER BY Id DESC LIMIT 1;");
    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

void Database::insert_data(int64_t run_id, int64_t data_type_id,
                           uint64_t timestamp_us, int32_t value) {
    if (!db_) return;

    const time_t t = static_cast<time_t>(timestamp_us / 1000000);
    struct tm    tm_buf{};
    gmtime_r(&t, &tm_buf);
    char ts_buf[32];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    sqlite3_stmt* stmt = prepare(R"SQL(
        INSERT INTO Data (Run_Id, Data_Type_Id, Value, Timestamp)
        VALUES (?, ?, ?, ?);
    )SQL");
    sqlite3_bind_int64(stmt, 1, run_id);
    sqlite3_bind_int64(stmt, 2, data_type_id);
    sqlite3_bind_int(stmt,   3, value);
    sqlite3_bind_text(stmt,  4, ts_buf, -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Telemetry runs at several hundred messages a second, so a failing insert
    // must not log per message — but it must not be silent either, which is
    // how foreign-key violations went unnoticed before.
    if (rc != SQLITE_DONE) {
        ++insert_failures_;
        if (insert_failures_ == 1 || insert_failures_ % 1000 == 0)
            std::cerr << "insert_data failed (" << insert_failures_
                      << " total, run_id=" << run_id
                      << " data_type_id=" << data_type_id << "): "
                      << sqlite3_errmsg(db_) << "\n";
    }
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + msg);
    }
}

sqlite3_stmt* Database::prepare(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(
            "sqlite prepare failed: " + std::string(sqlite3_errmsg(db_)));
    return stmt;
}

int64_t Database::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(db_);
}

}  // namespace wizard
