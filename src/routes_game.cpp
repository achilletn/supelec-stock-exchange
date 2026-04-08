#include "routes_game.h"
#include "auth.h"
#include "db.h"
#include "market.h"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <cstdio>

using json = nlohmann::json;

void registerGameRoutes(httplib::Server& svr) {

    // GET /api/market — current prices for all tracked assets
    svr.Get("/api/market", [](const httplib::Request&, httplib::Response& res) {
        json j = json::array();
        std::lock_guard<std::mutex> lock(g_marketMutex);
        for (auto& [symbol, asset] : g_market)
            j.push_back({{"symbol", symbol}, {"price", asset.price}, {"change24h", asset.change24h}});
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/dashboard — summary for the authenticated player
    svr.Get("/api/dashboard", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = getSessionUser(req);
        if (username.empty()) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        const int uid = dbGetUserId(db, username);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        const double portfolioValue = dbGetPortfolioValue(db, uid);

        // USD cash balance
        double usd = 0.0;
        sqlite3_stmt* stmtUsd;
        if (sqlite3_prepare_v2(db,
            "SELECT quantity FROM portfolios WHERE user_id = ? AND symbol = 'USD';",
            -1, &stmtUsd, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmtUsd, 1, uid);
            if (sqlite3_step(stmtUsd) == SQLITE_ROW)
                usd = sqlite3_column_double(stmtUsd, 0);
        }
        sqlite3_finalize(stmtUsd);

        // 24h snapshot baseline
        double snapshot = 100000.0;
        sqlite3_stmt* stmtSnap;
        if (sqlite3_prepare_v2(db,
            "SELECT value FROM snapshots WHERE user_id = ?;",
            -1, &stmtSnap, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmtSnap, 1, uid);
            if (sqlite3_step(stmtSnap) == SQLITE_ROW)
                snapshot = sqlite3_column_double(stmtSnap, 0);
        }
        sqlite3_finalize(stmtSnap);
        sqlite3_close(db);

        const double pnlTotalPct  = (portfolioValue - 100000.0) / 100000.0 * 100.0;
        const double pnl24hPct    = (portfolioValue - snapshot) / snapshot * 100.0;

        char bufTotalPct[32], bufTotalUsd[32], buf24hPct[32], buf24hUsd[32];
        snprintf(bufTotalPct, sizeof(bufTotalPct), "%+.2f%%", pnlTotalPct);
        snprintf(bufTotalUsd, sizeof(bufTotalUsd), "%+.2f",   portfolioValue - 100000.0);
        snprintf(buf24hPct,   sizeof(buf24hPct),   "%+.2f%%", pnl24hPct);
        snprintf(buf24hUsd,   sizeof(buf24hUsd),   "%+.2f",   portfolioValue - snapshot);

        json j = {
            {"username",       username},
            {"portfolio_value", portfolioValue},
            {"usd",            usd},
            {"pnl",            std::string(bufTotalPct)},
            {"pnl_total_usd",  std::string(bufTotalUsd)},
            {"pnl_24h_pct",    std::string(buf24hPct)},
            {"pnl_24h_usd",    std::string(buf24hUsd)}
        };
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/leaderboard — all players ranked by portfolio value
    svr.Get("/api/leaderboard", [](const httplib::Request&, httplib::Response& res) {
        sqlite3* db = dbOpen();
        json j = json::array();

        sqlite3_stmt* stmtUsers;
        if (sqlite3_prepare_v2(db, "SELECT id, username FROM users;", -1, &stmtUsers, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmtUsers) == SQLITE_ROW) {
                const int uid          = sqlite3_column_int(stmtUsers, 0);
                const std::string user = (const char*)sqlite3_column_text(stmtUsers, 1);

                const double portfolioValue = dbGetPortfolioValue(db, uid);

                double usd = 0.0;
                sqlite3_stmt* stmtUsd;
                if (sqlite3_prepare_v2(db,
                    "SELECT quantity FROM portfolios WHERE user_id = ? AND symbol = 'USD';",
                    -1, &stmtUsd, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmtUsd, 1, uid);
                    if (sqlite3_step(stmtUsd) == SQLITE_ROW)
                        usd = sqlite3_column_double(stmtUsd, 0);
                }
                sqlite3_finalize(stmtUsd);

                double snapshot = 100000.0;
                sqlite3_stmt* stmtSnap;
                if (sqlite3_prepare_v2(db,
                    "SELECT value FROM snapshots WHERE user_id = ?;",
                    -1, &stmtSnap, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmtSnap, 1, uid);
                    if (sqlite3_step(stmtSnap) == SQLITE_ROW)
                        snapshot = sqlite3_column_double(stmtSnap, 0);
                }
                sqlite3_finalize(stmtSnap);

                const double pnlTotalPct = (portfolioValue - 100000.0) / 100000.0 * 100.0;
                const double pnl24hPct   = (portfolioValue - snapshot) / snapshot * 100.0;
                const double pnl24hUsd   = portfolioValue - snapshot;

                char bufTotalPct[32], bufTotalUsd[32], buf24hPct[32], buf24hUsd[32];
                snprintf(bufTotalPct, sizeof(bufTotalPct), "%+.2f%%", pnlTotalPct);
                snprintf(bufTotalUsd, sizeof(bufTotalUsd), "%+.2f",   portfolioValue - 100000.0);
                snprintf(buf24hPct,   sizeof(buf24hPct),   "%+.2f%%", pnl24hPct);
                snprintf(buf24hUsd,   sizeof(buf24hUsd),   "%+.2f",   pnl24hUsd);

                j.push_back({
                    {"username",        user},
                    {"portfolio_value", portfolioValue},
                    {"usd",             usd},
                    {"pnl",             std::string(bufTotalPct)},
                    {"pnl_total_usd",   std::string(bufTotalUsd)},
                    {"pnl_24h_pct",     std::string(buf24hPct)},
                    {"pnl_24h_usd",     std::string(buf24hUsd)}
                });
            }
        }
        sqlite3_finalize(stmtUsers);
        sqlite3_close(db);

        std::sort(j.begin(), j.end(), [](const json& a, const json& b) {
            return a["portfolio_value"].get<double>() > b["portfolio_value"].get<double>();
        });
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/history?symbol=BTC/USD&period=24h[&since=<timestamp>]
    svr.Get("/api/history", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("symbol")) { res.status = 400; return; }

        const std::string symbol = req.get_param_value("symbol");
        const std::string period = req.has_param("period") ? req.get_param_value("period") : "24h";
        const std::string since  = req.has_param("since")  ? req.get_param_value("since")  : "";

        std::string timeModifier = "-24 hours";
        std::string dateFmt      = "'%H:%M'";
        if (period == "1h") { timeModifier = "-1 hour";  dateFmt = "'%H:%M:%S'"; }
        else if (period == "7d") { timeModifier = "-7 days"; dateFmt = "'%d/%m %H:00'"; }

        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        json j = json::array();

        std::string sql;
        if (!since.empty()) {
            sql = "SELECT price, strftime(" + dateFmt + ", timestamp), timestamp"
                  " FROM price_history WHERE symbol = ? AND timestamp > ? ORDER BY timestamp ASC;";
        } else {
            sql = "SELECT price, strftime(" + dateFmt + ", timestamp), timestamp"
                  " FROM price_history WHERE symbol = ?"
                  " AND timestamp >= datetime('now', '" + timeModifier + "') ORDER BY timestamp ASC;";
        }

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_STATIC);
            if (!since.empty())
                sqlite3_bind_text(stmt, 2, since.c_str(), -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                j.push_back({
                    {"price",     sqlite3_column_double(stmt, 0)},
                    {"time",      (const char*)sqlite3_column_text(stmt, 1)},
                    {"timestamp", (const char*)sqlite3_column_text(stmt, 2)}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/portfolio — authenticated player's holdings
    svr.Get("/api/portfolio", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = getSessionUser(req);
        if (username.empty()) { res.status = 401; return; }

        sqlite3* db = dbOpen();
        const int uid = dbGetUserId(db, username);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "SELECT symbol, quantity FROM portfolios WHERE user_id = ? AND quantity > 0;",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const std::string sym = (const char*)sqlite3_column_text(stmt, 0);
                const double qty      = sqlite3_column_double(stmt, 1);
                double price = 0.0;
                double value = 0.0;

                if (sym == "USD") {
                    price = 1.0;
                    value = qty;
                } else {
                    std::lock_guard<std::mutex> lock(g_marketMutex);
                    auto it = g_market.find(sym);
                    if (it != g_market.end()) {
                        price = it->second.price;
                        value = qty * price;
                    }
                }
                j.push_back({{"symbol", sym}, {"quantity", qty}, {"unit_price", price}, {"value", value}});
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/trade — buy or sell an asset
    svr.Post("/api/trade", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = getSessionUser(req);
        if (username.empty()) { res.status = 401; return; }

        if (!g_gameRunning.load()) {
            res.status = 503;
            res.set_content("{\"error\":\"Trading is currently suspended by the administrator.\"}", "application/json");
            return;
        }

        const std::string symbol = req.get_param_value("symbol");
        const std::string action = req.get_param_value("action");
        double quantity = 0.0;

        try { quantity = std::stod(req.get_param_value("quantity")); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid quantity\"}", "application/json");
            return;
        }

        for (char c : symbol) {
            if (!std::isalnum(c) && c != '/') {
                res.status = 400;
                res.set_content("{\"error\":\"Invalid symbol\"}", "application/json");
                return;
            }
        }

        if (action != "buy" && action != "sell") {
            res.status = 400;
            res.set_content("{\"error\":\"Invalid action — expected 'buy' or 'sell'\"}", "application/json");
            return;
        }

        double price = 0.0;
        {
            std::lock_guard<std::mutex> lock(g_marketMutex);
            auto it = g_market.find(symbol);
            if (it == g_market.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"Asset not found\"}", "application/json");
                return;
            }
            price = it->second.price;
        }
        const double tradeValue = price * quantity;

        sqlite3* db = dbOpen();
        const int uid = dbGetUserId(db, username);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

        // Helper: get current quantity of a symbol for this user
        auto getQty = [&](const std::string& sym) -> double {
            double q = 0.0;
            sqlite3_stmt* s;
            if (sqlite3_prepare_v2(db,
                "SELECT quantity FROM portfolios WHERE user_id = ? AND symbol = ?;",
                -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, uid);
                sqlite3_bind_text(s, 2, sym.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(s) == SQLITE_ROW) q = sqlite3_column_double(s, 0);
            }
            sqlite3_finalize(s);
            return q;
        };

        auto rollback = [&]() {
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(db);
        };

        std::string message;

        if (action == "buy") {
            if (getQty("USD") < tradeValue) {
                rollback();
                res.status = 403;
                res.set_content("{\"error\":\"Insufficient USD balance\"}", "application/json");
                return;
            }

            sqlite3_stmt* debitUsd;
            if (sqlite3_prepare_v2(db,
                "UPDATE portfolios SET quantity = quantity - ? WHERE user_id = ? AND symbol = 'USD';",
                -1, &debitUsd, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(debitUsd, 1, tradeValue);
                sqlite3_bind_int(debitUsd, 2, uid);
                sqlite3_step(debitUsd);
            }
            sqlite3_finalize(debitUsd);

            sqlite3_stmt* creditAsset;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO portfolios (user_id, symbol, quantity) VALUES (?, ?, ?)"
                " ON CONFLICT(user_id, symbol) DO UPDATE SET quantity = quantity + excluded.quantity;",
                -1, &creditAsset, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(creditAsset, 1, uid);
                sqlite3_bind_text(creditAsset, 2, symbol.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_double(creditAsset, 3, quantity);
                sqlite3_step(creditAsset);
            }
            sqlite3_finalize(creditAsset);
            message = "Bought " + std::to_string(quantity) + " " + symbol;

        } else { // sell
            if (getQty(symbol) < quantity) {
                rollback();
                res.status = 403;
                res.set_content("{\"error\":\"Not enough of this asset in portfolio\"}", "application/json");
                return;
            }

            sqlite3_stmt* debitAsset;
            if (sqlite3_prepare_v2(db,
                "UPDATE portfolios SET quantity = quantity - ? WHERE user_id = ? AND symbol = ?;",
                -1, &debitAsset, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(debitAsset, 1, quantity);
                sqlite3_bind_int(debitAsset, 2, uid);
                sqlite3_bind_text(debitAsset, 3, symbol.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(debitAsset);
            }
            sqlite3_finalize(debitAsset);

            sqlite3_stmt* creditUsd;
            if (sqlite3_prepare_v2(db,
                "UPDATE portfolios SET quantity = quantity + ? WHERE user_id = ? AND symbol = 'USD';",
                -1, &creditUsd, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(creditUsd, 1, tradeValue);
                sqlite3_bind_int(creditUsd, 2, uid);
                sqlite3_step(creditUsd);
            }
            sqlite3_finalize(creditUsd);
            message = "Sold successfully";
        }

        // Log the trade
        sqlite3_stmt* logStmt;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO trades_log (user_id, username, action, symbol, quantity, price, value)"
            " VALUES (?, ?, ?, ?, ?, ?, ?);",
            -1, &logStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(logStmt, 1, uid);
            sqlite3_bind_text(logStmt, 2, username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(logStmt, 3, action.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(logStmt, 4, symbol.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_double(logStmt, 5, quantity);
            sqlite3_bind_double(logStmt, 6, price);
            sqlite3_bind_double(logStmt, 7, tradeValue);
            sqlite3_step(logStmt);
        }
        sqlite3_finalize(logStmt);

        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\",\"message\":\"" + message + "\"}", "application/json");
    });

    // POST /api/contact — player sends a message to the admins
    svr.Post("/api/contact", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = getSessionUser(req);
        if (username.empty()) { res.status = 401; return; }

        const std::string subject = req.get_param_value("subject");
        const std::string message = req.get_param_value("message");
        if (message.empty()) { res.status = 400; return; }

        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO contacts (username, subject, message) VALUES (?, ?, ?);",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, subject.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
}