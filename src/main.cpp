#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "db.h"
#include "market.h"
#include "auth.h"
#include "routes_game.h"
#include "routes_admin.h"
#include <sqlite3.h>
#include <fstream>
#include <iostream>

static void initDatabase() {
    sqlite3* db = dbOpen();

    // Note: table and column names are now in English.
    //       Run a migration if upgrading from a previous schema version.
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS users (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            username   TEXT UNIQUE NOT NULL,
            phone      TEXT UNIQUE NOT NULL,
            password   TEXT NOT NULL,
            status     TEXT NOT NULL DEFAULT 'pending',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS assets (
            symbol TEXT PRIMARY KEY,
            name   TEXT,
            type   TEXT
        );
        CREATE TABLE IF NOT EXISTS portfolios (
            user_id  INTEGER NOT NULL,
            symbol   TEXT NOT NULL,
            quantity REAL NOT NULL DEFAULT 0,
            FOREIGN KEY(user_id) REFERENCES users(id),
            UNIQUE(user_id, symbol)
        );
        CREATE TABLE IF NOT EXISTS price_history (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            symbol    TEXT NOT NULL,
            price     REAL NOT NULL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS snapshots (
            user_id   INTEGER PRIMARY KEY,
            value     REAL NOT NULL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
        CREATE TABLE IF NOT EXISTS contacts (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            username  TEXT,
            subject   TEXT,
            message   TEXT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS trades_log (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id   INTEGER,
            username  TEXT,
            action    TEXT,
            symbol    TEXT,
            quantity  REAL,
            price     REAL,
            value     REAL,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
        CREATE TABLE IF NOT EXISTS sessions_log (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            username   TEXT,
            ip         TEXT,
            user_agent TEXT,
            action     TEXT,
            timestamp  DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS config (
            key   TEXT PRIMARY KEY,
            value TEXT
        );
        INSERT OR IGNORE INTO assets (symbol, name, type) VALUES
            ('USD',     'US Dollar',  'fiat'),
            ('BTC/USD', 'Bitcoin',    'crypto'),
            ('ETH/USD', 'Ethereum',   'crypto');
        INSERT OR IGNORE INTO config (key, value) VALUES ('game_running', '1');
    )";

    sqlite3_exec(db, schema, nullptr, nullptr, nullptr);

    // Restore game state from DB
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key = 'game_running';",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string val = (const char*)sqlite3_column_text(stmt, 0);
            g_gameRunning.store(val == "1");
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

int main() {
    initDatabase();
    startWorkers();

    httplib::Server svr;

    // Static assets
    svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/favicon.ico", std::ios::binary);
        if (!f) { res.status = 404; return; }
        res.set_content(std::string(std::istreambuf_iterator<char>(f), {}), "image/x-icon");
    });

    registerAuthRoutes(svr);
    registerGameRoutes(svr);
    registerAdminRoutes(svr);

    std::cout << "[INFO] Server running on http://0.0.0.0:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    return 0;
}