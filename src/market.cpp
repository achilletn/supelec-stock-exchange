#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "market.h"
#include "db.h"
#include "httplib.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

std::map<std::string, Asset> g_market;
std::mutex                   g_marketMutex;
std::atomic<bool>            g_gameRunning{true};

// ── Price refresh worker ───────────────────────────────────────────────────

static void workerRefreshPrices() {
    const std::vector<std::string> symbols = {
        "BTC/USD", "ETH/USD", "BNB/USD", "SOL/USD",
        "XRP/USD", "DOGE/USD", "ADA/USD", "AVAX/USD"
    };
    const std::string apiKey = "8688ffbe4237481cb6bf8b24151439b6";

    while (true) {
        try {
            std::string symbolList;
            for (size_t i = 0; i < symbols.size(); ++i)
                symbolList += symbols[i] + (i == symbols.size() - 1 ? "" : ",");

            httplib::Client client("https://api.twelvedata.com");
            auto res = client.Get(("/quote?symbol=" + symbolList + "&apikey=" + apiKey).c_str());

            if (res && res->status == 200) {
                auto j = json::parse(res->body);

                if (j.contains("code") && j["code"] == 429) {
                    std::cout << "[QUOTA] Rate limit hit, forcing pause." << std::endl;
                } else {
                    std::lock_guard<std::mutex> lock(g_marketMutex);
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        const std::string sym = it.key();
                        const auto& data = it.value();
                        if (data.is_object() && data.contains("close")) {
                            Asset& a   = g_market[sym];
                            a.price    = std::stod(data["close"].get<std::string>());
                            a.name     = data.value("name", sym);
                            if (data.contains("percent_change"))
                                a.change24h = std::stod(data["percent_change"].get<std::string>());
                            std::cout << "  [OK] " << sym << " updated." << std::endl;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "  [EXCEPTION] price worker: " << e.what() << std::endl;
        }

        std::cout << "[INFO] Price cycle done. Sleeping 65s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(65));
    }
}

// ── Snapshot worker (daily PnL baseline) ──────────────────────────────────

static void workerSnapshots() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
        sqlite3* db = dbOpen();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT id FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int uid       = sqlite3_column_int(stmt, 0);
                double value  = dbGetPortfolioValue(db, uid);
                sqlite3_stmt* snap;
                const char* sql =
                    "INSERT INTO snapshots (user_id, value) VALUES (?, ?)"
                    " ON CONFLICT(user_id) DO UPDATE SET value = excluded.value, timestamp = CURRENT_TIMESTAMP;";
                if (sqlite3_prepare_v2(db, sql, -1, &snap, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(snap, 1, uid);
                    sqlite3_bind_double(snap, 2, value);
                    sqlite3_step(snap);
                }
                sqlite3_finalize(snap);
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }
}

// ── Price history recorder ─────────────────────────────────────────────────

static void workerRecordHistory() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        sqlite3* db = dbOpen();
        {
            std::lock_guard<std::mutex> lock(g_marketMutex);
            for (auto& [sym, asset] : g_market) {
                if (asset.price <= 0.0) continue;
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db, "INSERT INTO price_history (symbol, price) VALUES (?, ?);",
                                       -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, sym.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_double(stmt, 2, asset.price);
                    sqlite3_step(stmt);
                }
                sqlite3_finalize(stmt);
            }
        }
        sqlite3_close(db);
    }
}

// ── Public entry point ─────────────────────────────────────────────────────

void startWorkers() {
    std::thread(workerRefreshPrices).detach();
    std::thread(workerSnapshots).detach();
    std::thread(workerRecordHistory).detach();
}