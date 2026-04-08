#include "routes_admin.h"
#include "auth.h"
#include "db.h"
#include "market.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <fstream>
#include <cctype>

using json = nlohmann::json;

void registerAdminRoutes(httplib::Server& svr) {

    // Serve the admin SPA
    svr.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/admin/index.html", std::ios::binary);
        if (!f) { res.status = 404; return; }
        res.set_content(std::string(std::istreambuf_iterator<char>(f), {}), "text/html");
    });

    svr.Get("/admin/admin.css", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/admin/admin.css", std::ios::binary);
        if (!f) { res.status = 404; return; }
        res.set_content(std::string(std::istreambuf_iterator<char>(f), {}), "text/css");
    });

    svr.Get("/admin/admin.js", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/admin/admin.js", std::ios::binary);
        if (!f) { res.status = 404; return; }
        res.set_content(std::string(std::istreambuf_iterator<char>(f), {}), "application/javascript");
    });

    // GET /api/admin/overview — global stats + last 10 trades
    svr.Get("/api/admin/overview", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        int userCount = 0, tradeCount = 0;
        double totalVolume = 0.0;

        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users;", -1, &s, nullptr) == SQLITE_OK)
            if (sqlite3_step(s) == SQLITE_ROW) userCount = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);

        if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*), COALESCE(SUM(value),0) FROM trades_log;",
            -1, &s, nullptr) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                tradeCount   = sqlite3_column_int(s, 0);
                totalVolume  = sqlite3_column_double(s, 1);
            }
        }
        sqlite3_finalize(s);

        json recentTrades = json::array();
        if (sqlite3_prepare_v2(db,
            "SELECT id, username, action, symbol, quantity, price, value, timestamp"
            " FROM trades_log ORDER BY timestamp DESC LIMIT 10;",
            -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                recentTrades.push_back({
                    {"id",        sqlite3_column_int(s, 0)},
                    {"username",  (const char*)sqlite3_column_text(s, 1)},
                    {"action",    (const char*)sqlite3_column_text(s, 2)},
                    {"symbol",    (const char*)sqlite3_column_text(s, 3)},
                    {"quantity",  sqlite3_column_double(s, 4)},
                    {"price",     sqlite3_column_double(s, 5)},
                    {"value",     sqlite3_column_double(s, 6)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 7)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);

        json j = {
            {"user_count",    userCount},
            {"trade_count",   tradeCount},
            {"total_volume",  totalVolume},
            {"game_running",  g_gameRunning.load()},
            {"recent_trades", recentTrades}
        };
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/admin/game-state
    svr.Get("/api/admin/game-state", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }
        res.set_content("{\"running\":" + std::string(g_gameRunning.load() ? "true" : "false") + "}",
                        "application/json");
    });

    // POST /api/admin/game-state — start or stop trading
    svr.Post("/api/admin/game-state", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const bool newState = (req.get_param_value("action") == "start");
        g_gameRunning.store(newState);

        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO config (key, value) VALUES ('game_running', ?)"
            " ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, newState ? "1" : "0", -1, SQLITE_STATIC);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        res.set_content("{\"running\":" + std::string(newState ? "true" : "false") + "}",
                        "application/json");
    });

    // GET /api/admin/users — list all users with portfolio values
    svr.Get("/api/admin/users", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        json j = json::array();
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db,
            "SELECT id, username, created_at, status, phone FROM users ORDER BY id ASC;",
            -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const int uid          = sqlite3_column_int(stmt, 0);
                const std::string user = (const char*)sqlite3_column_text(stmt, 1);
                const char* createdAt  = (const char*)sqlite3_column_text(stmt, 2);
                const char* status     = (const char*)sqlite3_column_text(stmt, 3);
                const char* phone      = (const char*)sqlite3_column_text(stmt, 4);

                const double portfolioValue = dbGetPortfolioValue(db, uid);

                double usd = 0.0;
                sqlite3_stmt* sp;
                if (sqlite3_prepare_v2(db,
                    "SELECT quantity FROM portfolios WHERE user_id = ? AND symbol = 'USD';",
                    -1, &sp, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(sp, 1, uid);
                    if (sqlite3_step(sp) == SQLITE_ROW) usd = sqlite3_column_double(sp, 0);
                }
                sqlite3_finalize(sp);

                j.push_back({
                    {"id",             uid},
                    {"username",       user},
                    {"usd",            usd},
                    {"portfolio_value", portfolioValue},
                    {"pnl_pct",        (portfolioValue - 100000.0) / 1000.0},
                    {"created_at",     createdAt ? createdAt : ""},
                    {"status",         status ? status : "pending"},
                    {"phone",          phone  ? phone  : ""}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/admin/approve-user — approve a pending account and grant starting balance
    svr.Post("/api/admin/approve-user", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = dbOpen();
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        sqlite3_stmt* s1;
        if (sqlite3_prepare_v2(db,
            "UPDATE users SET status = 'approved' WHERE id = ?;",
            -1, &s1, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s1, 1, uid);
            sqlite3_step(s1);
        }
        sqlite3_finalize(s1);

        sqlite3_stmt* s2;
        if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO portfolios (user_id, symbol, quantity) VALUES (?, 'USD', 100000.0);",
            -1, &s2, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s2, 1, uid);
            sqlite3_step(s2);
        }
        sqlite3_finalize(s2);

        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // POST /api/admin/set-balance — force-set a user's USD balance
    svr.Post("/api/admin/set-balance", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const int    uid    = std::stoi(req.get_param_value("user_id"));
        const double amount = std::stod(req.get_param_value("amount"));

        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO portfolios (user_id, symbol, quantity) VALUES (?, 'USD', ?)"
            " ON CONFLICT(user_id, symbol) DO UPDATE SET quantity = excluded.quantity;",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            sqlite3_bind_double(stmt, 2, amount);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // POST /api/admin/reset-user — wipe portfolio and restore 100k USD
    svr.Post("/api/admin/reset-user", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = dbOpen();
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        auto execForUser = [&](const char* sql) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, uid);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        };

        execForUser("DELETE FROM portfolios WHERE user_id = ?;");
        execForUser("DELETE FROM snapshots WHERE user_id = ?;");

        sqlite3_stmt* restore;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO portfolios (user_id, symbol, quantity) VALUES (?, 'USD', 100000.0);",
            -1, &restore, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(restore, 1, uid);
            sqlite3_step(restore);
        }
        sqlite3_finalize(restore);

        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // POST /api/admin/delete-user — permanently remove a user and all their data
    svr.Post("/api/admin/delete-user", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = dbOpen();
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        for (const char* sql : {
            "DELETE FROM portfolios  WHERE user_id = ?;",
            "DELETE FROM snapshots   WHERE user_id = ?;",
            "DELETE FROM trades_log  WHERE user_id = ?;",
            "DELETE FROM users       WHERE id = ?;"
        }) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, uid);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // GET /api/admin/trades — full trade history (last 1000)
    svr.Get("/api/admin/trades", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        json j = json::array();
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db,
            "SELECT id, username, action, symbol, quantity, price, value, timestamp"
            " FROM trades_log ORDER BY timestamp DESC LIMIT 1000;",
            -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                j.push_back({
                    {"id",        sqlite3_column_int(s, 0)},
                    {"username",  (const char*)sqlite3_column_text(s, 1)},
                    {"action",    (const char*)sqlite3_column_text(s, 2)},
                    {"symbol",    (const char*)sqlite3_column_text(s, 3)},
                    {"quantity",  sqlite3_column_double(s, 4)},
                    {"price",     sqlite3_column_double(s, 5)},
                    {"value",     sqlite3_column_double(s, 6)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 7)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/admin/delete-trade — remove a trade log entry (does not touch portfolios)
    svr.Post("/api/admin/delete-trade", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        const int tradeId = std::stoi(req.get_param_value("trade_id"));
        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "DELETE FROM trades_log WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, tradeId);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // GET /api/admin/sessions — recent login/logout events
    svr.Get("/api/admin/sessions", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        json j = json::array();
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db,
            "SELECT username, ip, user_agent, action, timestamp"
            " FROM sessions_log ORDER BY timestamp DESC LIMIT 2000;",
            -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char* ua = (const char*)sqlite3_column_text(s, 2);
                j.push_back({
                    {"username",   (const char*)sqlite3_column_text(s, 0)},
                    {"ip",         (const char*)sqlite3_column_text(s, 1)},
                    {"user_agent", ua ? ua : ""},
                    {"action",     (const char*)sqlite3_column_text(s, 3)},
                    {"timestamp",  (const char*)sqlite3_column_text(s, 4)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/admin/contacts — messages sent via /api/contact
    svr.Get("/api/admin/contacts", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        json j = json::array();
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db,
            "SELECT username, subject, message, timestamp FROM contacts ORDER BY timestamp DESC;",
            -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                j.push_back({
                    {"username",  (const char*)sqlite3_column_text(s, 0)},
                    {"subject",   (const char*)sqlite3_column_text(s, 1)},
                    {"message",   (const char*)sqlite3_column_text(s, 2)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 3)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/admin/user-portfolio — assets for a specific user (excluding USD)
    svr.Get("/api/admin/user-portfolio", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }
        if (!req.has_param("user_id")) { res.status = 400; return; }

        const int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = dbOpen();
        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "SELECT symbol, quantity FROM portfolios WHERE user_id = ? AND symbol != 'USD';",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                j.push_back({
                    {"symbol",   (const char*)sqlite3_column_text(stmt, 0)},
                    {"quantity", sqlite3_column_double(stmt, 1)}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/admin/portfolio — force-set (or delete) an asset in a user's portfolio
    svr.Post("/api/admin/portfolio", [](const httplib::Request& req, httplib::Response& res) {
        if (!isAdminSession(req)) { res.status = 401; return; }

        if (!req.has_param("user_id") || !req.has_param("symbol") || !req.has_param("quantity")) {
            res.status = 400;
            res.set_content("{\"error\":\"Missing parameters: user_id, symbol, quantity\"}", "application/json");
            return;
        }

        int uid = 0;
        double quantity = 0.0;
        try {
            uid      = std::stoi(req.get_param_value("user_id"));
            quantity = std::stod(req.get_param_value("quantity"));
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid number format\"}", "application/json");
            return;
        }

        std::string symbol = req.get_param_value("symbol");
        for (auto& c : symbol) c = (char)toupper(c);

        sqlite3* db = dbOpen();

        if (quantity <= 0.0) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db,
                "DELETE FROM portfolios WHERE user_id = ? AND symbol = ?;",
                -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, uid);
                sqlite3_bind_text(stmt, 2, symbol.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        } else {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO portfolios (user_id, symbol, quantity) VALUES (?, ?, ?)"
                " ON CONFLICT(user_id, symbol) DO UPDATE SET quantity = excluded.quantity;",
                -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, uid);
                sqlite3_bind_text(stmt, 2, symbol.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_double(stmt, 3, quantity);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }

        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
}