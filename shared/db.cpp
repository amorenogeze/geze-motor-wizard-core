#include "db.h"

#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <ctime>

namespace wizard {

namespace {

std::string now_datetime() {
    time_t t = time(nullptr);
    struct tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

}  // namespace

Database::Database(const std::string& path) : path_(path) {}

Database::~Database() {
    disconnect();
}

void Database::connect() {
    if (db_) return;

    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("failed to open database " + path_ + ": " + err);
    }

    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=2000;");
    exec("PRAGMA foreign_keys=ON;");
}

void Database::disconnect() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

DataTypeIds Database::seed_static_data() {
    exec("INSERT OR IGNORE INTO Status (Id, status) VALUES (1, 'pending');");
    exec("INSERT OR IGNORE INTO Status (Id, status) VALUES (2, 'running');");
    exec("INSERT OR IGNORE INTO Status (Id, status) VALUES (3, 'completed');");
    exec("INSERT OR IGNORE INTO Status (Id, status) VALUES (4, 'aborted');");
    exec("INSERT OR IGNORE INTO Status (Id, status) VALUES (5, 'error');");

    status_id_running_ = 2;
    status_id_stopped_ = 3;
    exec(R"SQL(
    INSERT INTO Data_Type (id, name, display_name, description, data_unit, data_max, data_min)
        VALUES (1, 'position', 'Position', 'Encoder position, wraps at 100', 'counts', 1000, 500)
        ON CONFLICT(id) DO UPDATE SET
            name=excluded.name, display_name=excluded.display_name,
            description=excluded.description, data_unit=excluded.data_unit,
            data_max=excluded.data_max, data_min=excluded.data_min;
    
    INSERT INTO Data_Type (id, name, display_name, description, data_unit, data_max, data_min)
        VALUES (2, 'velocity', 'Velocity', 'Motor shaft velocity', 'counts/s', 2000, 0)
        ON CONFLICT(id) DO UPDATE SET
            name=excluded.name, display_name=excluded.display_name,
            description=excluded.description, data_unit=excluded.data_unit,
            data_max=excluded.data_max, data_min=excluded.data_min;
    
    INSERT INTO Data_Type (id, name, display_name, description, data_unit, data_max, data_min)
        VALUES (3, 'current', 'Current', 'Motor phase current', 'mA', 600, 400)
        ON CONFLICT(id) DO UPDATE SET
            name=excluded.name, display_name=excluded.display_name,
            description=excluded.description, data_unit=excluded.data_unit,
            data_max=excluded.data_max, data_min=excluded.data_min;
    )SQL");

    DataTypeIds ids;

    auto resolve = [&](const char* name) -> int64_t {
        sqlite3_stmt* stmt = prepare("SELECT id FROM Data_Type WHERE name = ?;");
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        int64_t id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        if (id < 0)
            throw std::runtime_error(std::string("Data_Type not found: ") + name);
        return id;
    };

    ids.position = resolve("position");  // id=1
    ids.velocity = resolve("velocity");  // id=2
    ids.current  = resolve("current");   // id=3
    return ids;
}

int64_t Database::ensure_device(const std::string& device_serial) {
    int64_t serial_int = 0;
    try { serial_int = std::stoll(device_serial); } catch (...) {}

    {
        sqlite3_stmt* stmt = prepare(
            "SELECT Id FROM Device WHERE Device_Serial = ?;");
        sqlite3_bind_int64(stmt, 1, serial_int);
        int64_t id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        if (id >= 0) return id;
    }

    sqlite3_stmt* stmt = prepare(
        "INSERT INTO Device (Device_Serial) VALUES (?);");
    sqlite3_bind_int64(stmt, 1, serial_int);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return last_insert_rowid();
}

int64_t Database::open_session() {
    exec("INSERT OR IGNORE INTO Connection_Type (Id, Type) VALUES (1, 'CANopen');");
    exec("INSERT OR IGNORE INTO Rol (Id, Rol_Name) VALUES (1, 'admin');");
    exec("INSERT OR IGNORE INTO Users (Id, Username, Password, Rol_Id) "
         "VALUES (1, 'wizard-engine', 'n/a', 1);");

    sqlite3_stmt* stmt = prepare(R"SQL(
        INSERT INTO Session
            (Start_Date, Session_Status, Connection_Type, User_Id)
        VALUES (?, 1, 1, 1);
    )SQL");
    std::string now = now_datetime();
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return last_insert_rowid();
}

void Database::close_session(int64_t session_id) {
    sqlite3_stmt* stmt = prepare(
        "UPDATE Session SET End_Date = ?, Session_Status = 2 WHERE Id = ?;");
    std::string now = now_datetime();
    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t Database::open_run(int64_t session_id, int64_t device_id,
                             const std::string& config) {
    std::cout << "[DB] open_run: session_id=" << session_id
              << " device_id=" << device_id << "\n";

    sqlite3_stmt* stmt = prepare(R"SQL(
        INSERT INTO Runs
            (Session_Id, Config, Start_Time, Device_Id, Run_Status)
        VALUES (?, ?, strftime('%s','now'), ?, ?);
    )SQL");
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, config.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, device_id);
    sqlite3_bind_int64(stmt, 4, status_id_running_);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE)
        std::cerr << "[DB] open_run FAILED: " << sqlite3_errmsg(db_) << "\n";
    sqlite3_finalize(stmt);

    int64_t id = last_insert_rowid();
    std::cout << "[DB] open_run: inserted Run id=" << id << "\n";
    return id;
}

void Database::close_run(int64_t run_id) {
    sqlite3_stmt* stmt = prepare(R"SQL(
        UPDATE Runs
        SET End_Time = strftime('%s','now'), Run_Status = ?
        WHERE Id = ?;
    )SQL");
    sqlite3_bind_int64(stmt, 1, status_id_stopped_);
    sqlite3_bind_int64(stmt, 2, run_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Database::insert_data(int64_t run_id, int64_t data_type_id,
                             uint64_t timestamp_us, int32_t value) {
    time_t t = static_cast<time_t>(timestamp_us / 1000000);
    struct tm tm_buf{};
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
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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
