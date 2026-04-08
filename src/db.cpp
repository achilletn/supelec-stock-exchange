#include "db.h"
#include "market.h"
#include <iostream>

sqlite3* dbOpen() {
    sqlite3* db = nullptr;
    if (sqlite3_open("bourse.db", &db) != SQLITE_OK)
        std::cerr << "[DB] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
    return db;
}

int dbGetUserId(sqlite3* db, const std::string& username) {
    int id = -1;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT id FROM users WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

// TODO SECURITY: Replace with bcrypt/argon2. Never store passwords in plaintext.
static bool verifyPassword(const std::string& input, const std::string& stored) {
    return input == stored;
}

bool dbVerifyLogin(const std::string& username, const std::string& password) {
    sqlite3* db = dbOpen();
    bool valid = false;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT password FROM users WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string stored = (const char*)sqlite3_column_text(stmt, 0);
            valid = verifyPassword(password, stored);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return valid;
}

bool dbRegisterUser(const std::string& username, const std::string& password, const std::string& phone) {
    sqlite3* db = dbOpen();
    bool ok = false;
    sqlite3_stmt* stmt;
    // TODO SECURITY: hash the password before storing.
    const char* sql = "INSERT INTO users (username, phone, password, status) VALUES (?, ?, ?, 'pending');";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, phone.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, password.c_str(), -1, SQLITE_STATIC);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

double dbGetPortfolioValue(sqlite3* db, int userId) {
    double total = 0.0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT symbol, quantity FROM portfolios WHERE user_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, userId);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string sym = (const char*)sqlite3_column_text(stmt, 0);
            double qty     = sqlite3_column_double(stmt, 1);
            if (sym == "USD") {
                total += qty;
            } else {
                std::lock_guard<std::mutex> lock(g_marketMutex);
                auto it = g_market.find(sym);
                if (it != g_market.end())
                    total += qty * it->second.price;
            }
        }
    }
    sqlite3_finalize(stmt);
    return total;
}