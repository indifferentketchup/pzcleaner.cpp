#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>
#include <string>

class Statement {
public:
    Statement(sqlite3* db, const char* sql, const std::string& name) : db_(db), name_(name) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(name_ + ": " + sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool step() {
        int rc = sqlite3_step(statement_);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(name_ + ": " + sqlite3_errmsg(db_));
        }
        return false;
    }

    void reset() { sqlite3_reset(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3* db_;
    const std::string& name_;
    sqlite3_stmt* statement_ = nullptr;
};

class Database {
public:
    Database(std::string name, const std::string& bytes) : name_(std::move(name)) {
        sqlite3_open(":memory:", &db_);
        if (bytes.empty()) {
            return;
        }
        auto* buffer = static_cast<unsigned char*>(sqlite3_malloc64(bytes.size()));
        if (buffer == nullptr) {
            throw std::runtime_error(name_ + " is too big to open");
        }
        std::copy(bytes.begin(), bytes.end(), buffer);
        int rc = sqlite3_deserialize(db_, "main", buffer, static_cast<sqlite3_int64>(bytes.size()),
                                     static_cast<sqlite3_int64>(bytes.size()),
                                     SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        if (rc != SQLITE_OK) {
            throw std::runtime_error(name_ + " could not be opened");
        }
    }
    ~Database() { sqlite3_close(db_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Statement prepare(const char* sql) { return Statement(db_, sql, name_); }

    void exec(const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error(name_ + ": " + sqlite3_errmsg(db_));
        }
    }

    bool hasTable(const char* table) {
        Statement statement = prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
        sqlite3_bind_text(statement.get(), 1, table, -1, SQLITE_STATIC);
        return statement.step();
    }

    std::string bytes() {
        sqlite3_int64 size = 0;
        unsigned char* data = sqlite3_serialize(db_, "main", &size, 0);
        if (data == nullptr) {
            throw std::runtime_error(name_ + " could not be saved");
        }
        std::string out(reinterpret_cast<char*>(data), static_cast<std::size_t>(size));
        sqlite3_free(data);
        return out;
    }

private:
    std::string name_;
    sqlite3* db_ = nullptr;
};
