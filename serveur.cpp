#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>

using json = nlohmann::json;

// TODO SECURITY: Utiliser des variables d'environnement au lieu de les mettre en dur.
static const std::string ADMIN_USER = "goudale";
static const std::string ADMIN_PASS = "forceaumecdubush";
static const std::string ADMIN_COOKIE = "admin_session";
 
std::atomic<bool> isGameRunning{true};

struct ActionItem { std::string nom; double prix; double variation_24h; };
std::map<std::string, ActionItem> marcheMondial;
std::mutex marcheMutex;

// ── Helpers DB ────────────────────────────────────────────────────────────

static sqlite3* openDatabase() {
    sqlite3* db = nullptr;
    sqlite3_open("bourse.db", &db);
    return db;
}

static int getUserId(sqlite3* db, const std::string& username) {
    int id = -1;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT id FROM utilisateurs WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

static std::string getCookieUser(const httplib::Request& req) {
    if (!req.has_header("Cookie")) return "";
    const std::string cookies = req.get_header_value("Cookie");
    size_t pos = cookies.find("auth_user=");
    if (pos == std::string::npos) return "";
    size_t start = pos + 10;
    size_t end = cookies.find(";", start);
    return cookies.substr(start, end - start);
}

// TODO SECURITY: Implémenter bcrypt/argon2 ici. Ne jamais stocker en clair.
static bool verifyPassword(const std::string& inputPassword, const std::string& storedHash) {
    return inputPassword == storedHash; // À remplacer par bcrypt_check()
}

static std::string hashPassword(const std::string& plainPassword) {
    return plainPassword; // À remplacer par bcrypt_hash()
}

static bool verifyLogin(const std::string& user, const std::string& pass) {
    sqlite3* db = openDatabase();
    bool valid = false;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT password FROM utilisateurs WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string storedPass = (const char*)sqlite3_column_text(stmt, 0);
            valid = verifyPassword(pass, storedPass);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return valid;
}

static bool registerUser(const std::string& user, const std::string& pass, const std::string& tel) {
    sqlite3* db = openDatabase();
    bool ok = false;
    sqlite3_stmt* stmt;
    // On insère avec le statut 'pending' par défaut
    if (sqlite3_prepare_v2(db, "INSERT INTO utilisateurs (username, telephone, password, statut) VALUES (?, ?, ?, 'pending');", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, tel.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, pass.c_str(), -1, SQLITE_STATIC); // Pense au hashage !
        
        if (sqlite3_step(stmt) == SQLITE_DONE) ok = true;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

// ── Workers ───────────────────────────────────────────────────────────────

void workerActualiserCours() {
    std::vector<std::string> mesActifs = {"BTC/USD", "ETH/USD", "BNB/USD", "SOL/USD", "XRP/USD", "DOGE/USD", "ADA/USD", "AVAX/USD"};

    while (true) {
        try {
            httplib::Client cli("https://api.binance.com");
            
            // 1. Convertir "BTC/USD" en "BTCUSDT" et construire l'URL Binance
            // Format attendu dans l'URL : %5B%22BTCUSDT%22%2C%22ETHUSDT%22%5D
            std::string symbolsEncoded = "%5B";
            for (size_t i = 0; i < mesActifs.size(); ++i) {
                std::string bSym = mesActifs[i];
                size_t pos = bSym.find("/USD");
                if (pos != std::string::npos) {
                    bSym.replace(pos, 4, "USDT"); // Transforme "BTC/USD" en "BTCUSDT"
                }
                
                symbolsEncoded += "%22" + bSym + "%22";
                if (i < mesActifs.size() - 1) symbolsEncoded += "%2C";
            }
            symbolsEncoded += "%5D";

            std::string path = "/api/v3/ticker/24hr?symbols=" + symbolsEncoded;
            auto res = cli.Get(path.c_str());

            if (res && res->status == 200) {
                auto j = json::parse(res->body);

                if (j.is_array()) {
                    std::lock_guard<std::mutex> lock(marcheMutex);
                    
                    for (const auto& item : j) {
                        std::string bSymbol = item["symbol"].get<std::string>();
                        
                        // 2. Reconvertir "BTCUSDT" en "BTC/USD" pour ton dictionnaire local
                        std::string sym = bSymbol;
                        size_t pos = sym.find("USDT");
                        if (pos != std::string::npos) {
                            sym.replace(pos, 4, "/USD");
                        }

                        // 3. Mettre à jour marcheMondial (traité comme une action)
                        if (item.contains("lastPrice")) {
                            double p = std::stod(item["lastPrice"].get<std::string>());
                            marcheMondial[sym].prix = p;
                            
                            // Binance ne renvoie pas de nom complet ici, on garde le Ticker (ex: "BTC")
                            marcheMondial[sym].nom = sym.substr(0, sym.find("/"));
                            
                            if (item.contains("priceChangePercent")) {
                                marcheMondial[sym].variation_24h = std::stod(item["priceChangePercent"].get<std::string>());
                            }
                            std::cout << "  [OK] " << sym << " mis à jour." << std::endl;
                        }
                    }
                }
            } else if (res) {
                std::cout << "[ERREUR BINANCE] Code : " << res->status << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "  [EXCEPTION] " << e.what() << std::endl;
        }

        // L'API Binance est ultra permissive, on peut rafraîchir toutes les 10 secondes
        std::cout << "[INFO] Cycle fini. Pause de 10s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

static double calculerValeurPortefeuille(sqlite3* db, int uid) {
    double valeur = 0.0;
    sqlite3_stmt* sp;
    if (sqlite3_prepare_v2(db, "SELECT symbole, quantite FROM portefeuilles WHERE user_id = ?;", -1, &sp, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(sp, 1, uid);
        while (sqlite3_step(sp) == SQLITE_ROW) {
            std::string sym = (const char*)sqlite3_column_text(sp, 0);
            double qte = sqlite3_column_double(sp, 1);
            if (sym == "USD") {
                valeur += qte;
            } else {
                std::lock_guard<std::mutex> lock(marcheMutex);
                if (marcheMondial.count(sym)) valeur += qte * marcheMondial[sym].prix;
            }
        }
    }
    sqlite3_finalize(sp);
    return valeur;
}

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    sqlite3* db = openDatabase();
    
    // Initialisation DB
    const char* initSql = R"(
        CREATE TABLE IF NOT EXISTS utilisateurs (
            id INTEGER PRIMARY KEY AUTOINCREMENT, 
            username TEXT UNIQUE, 
            telephone TEXT UNIQUE, 
            password TEXT, 
            statut TEXT DEFAULT 'pending', 
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS actifs (
            symbole TEXT PRIMARY KEY, 
            nom TEXT, 
            type TEXT
        );
        CREATE TABLE IF NOT EXISTS portefeuilles (
            user_id INTEGER, 
            symbole TEXT, 
            quantite REAL, 
            FOREIGN KEY(user_id) REFERENCES utilisateurs(id), 
            UNIQUE(user_id, symbole)
        );
        CREATE TABLE IF NOT EXISTS historique (id INTEGER PRIMARY KEY AUTOINCREMENT, symbole TEXT, prix REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS snapshots (user_id INTEGER PRIMARY KEY, valeur REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(user_id) REFERENCES utilisateurs(id));
        CREATE TABLE IF NOT EXISTS contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT, sujet TEXT, message TEXT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS trades_log (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER, username TEXT, action TEXT, symbole TEXT, quantite REAL, prix REAL, valeur REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(user_id) REFERENCES utilisateurs(id));
        CREATE TABLE IF NOT EXISTS sessions_log (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT, ip TEXT, user_agent TEXT, action TEXT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT);
        INSERT OR IGNORE INTO actifs (symbole, nom, type) VALUES ('USD','Dollar Americain','fiat'),('BTCUSDT','Bitcoin','crypto'),('ETHUSDT','Ethereum','crypto');
        INSERT OR IGNORE INTO config (key, value) VALUES ('game_running', '1');
    )";
    sqlite3_exec(db, initSql, nullptr, 0, nullptr);

    // Charger l'état de la partie
    sqlite3_stmt* cfgStmt;
    if (sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key = 'game_running';", -1, &cfgStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(cfgStmt) == SQLITE_ROW) {
            std::string val = (const char*)sqlite3_column_text(cfgStmt, 0);
            isGameRunning.store(val == "1");
        } else {
            sqlite3_exec(db, "INSERT INTO config (key, value) VALUES ('game_running', '1');", nullptr, 0, nullptr);
        }
    }
    sqlite3_finalize(cfgStmt);
    sqlite3_close(db);

    std::thread(workerActualiserCours).detach();

    // Routine : Snapshots 24h
    std::thread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(24));
            sqlite3* db = openDatabase();
            sqlite3_stmt* su;
            if (sqlite3_prepare_v2(db, "SELECT id FROM utilisateurs;", -1, &su, nullptr) == SQLITE_OK) {
                while (sqlite3_step(su) == SQLITE_ROW) {
                    int uid = sqlite3_column_int(su, 0);
                    double val = calculerValeurPortefeuille(db, uid);
                    sqlite3_stmt* stmtSnap;
                    if (sqlite3_prepare_v2(db, "INSERT INTO snapshots (user_id, valeur) VALUES (?, ?) ON CONFLICT(user_id) DO UPDATE SET valeur = excluded.valeur, timestamp = CURRENT_TIMESTAMP;", -1, &stmtSnap, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int(stmtSnap, 1, uid);
                        sqlite3_bind_double(stmtSnap, 2, val);
                        sqlite3_step(stmtSnap);
                    }
                    sqlite3_finalize(stmtSnap);
                }
            }
            sqlite3_finalize(su);
            sqlite3_close(db);
        }
    }).detach();

    // Routine : Historique
    std::thread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            sqlite3* db = openDatabase();
            std::lock_guard<std::mutex> lock(marcheMutex);
            for (auto& [sym, action] : marcheMondial) {
                if (action.prix <= 0.0) continue;
                sqlite3_stmt* st;
                if (sqlite3_prepare_v2(db, "INSERT INTO historique (symbole, prix) VALUES (?, ?);", -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_double(st, 2, action.prix);
                    sqlite3_step(st);
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }
    }).detach();

    httplib::Server svr;

    svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/favicon.ico", std::ios::binary);
        if (!f) { res.status = 404; return; }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        res.set_content(content, "image/x-icon");
    });

    svr.Get("/api/marche", [](const httplib::Request&, httplib::Response& res) {
        json j = json::array();
        std::lock_guard<std::mutex> lock(marcheMutex);
        for (auto& [key, val] : marcheMondial)
            j.push_back({ {"symbol", key}, {"price", val.prix}, {"variation_24h", val.variation_24h} });
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/check-session", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (!user.empty()) res.set_content("{\"status\":\"connecte\",\"user\":\"" + user + "\"}", "application/json");
        else { res.status = 401; res.set_content("{\"erreur\":\"non connecte\"}", "application/json"); }
    });

    svr.Get("/api/dashboard", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }

        sqlite3* db = openDatabase();
        int uid = getUserId(db, user);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        double valeur = calculerValeurPortefeuille(db, uid);
        double snap = -1.0;
        double usd = 0.0; // NOUVEAU
        
        // NOUVEAU : Récupération du solde USD
        sqlite3_stmt* sUsd;
        if (sqlite3_prepare_v2(db, "SELECT quantite FROM portefeuilles WHERE user_id = ? AND symbole = 'USD';", -1, &sUsd, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sUsd, 1, uid);
            if (sqlite3_step(sUsd) == SQLITE_ROW) usd = sqlite3_column_double(sUsd, 0);
        }
        sqlite3_finalize(sUsd);
        
        sqlite3_stmt* ss;
        if (sqlite3_prepare_v2(db, "SELECT valeur FROM snapshots WHERE user_id = ?;", -1, &ss, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(ss, 1, uid);
            if (sqlite3_step(ss) == SQLITE_ROW) snap = sqlite3_column_double(ss, 0);
        }
        sqlite3_finalize(ss);
        sqlite3_close(db);

        if (snap < 0) snap = 100000.0;

        double pnl_total = (valeur - 100000.0) / 100000.0 * 100.0;
        double pnl_24h_pct = (valeur - snap) / snap * 100.0;
        
        char buf_total[32], buf_24h_pct[32], buf_24h_usd[32], buf_total_usd[32];
        snprintf(buf_total_usd, sizeof(buf_total_usd), "%+.2f", valeur - 100000.0);
        snprintf(buf_total, sizeof(buf_total), "%+.2f%%", pnl_total);
        snprintf(buf_24h_pct, sizeof(buf_24h_pct), "%+.2f%%", pnl_24h_pct);
        snprintf(buf_24h_usd, sizeof(buf_24h_usd), "%+.2f", valeur - snap);

        json j = {
            {"username", user}, {"valeur_totale", valeur}, {"usd", usd},
            {"pnl", std::string(buf_total)}, {"pnl_24h_pct", std::string(buf_24h_pct)},
            {"pnl_24h_usd", std::string(buf_24h_usd)}, {"pnl_total_usd", std::string(buf_total_usd)}
        };
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/api/register", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = req.get_param_value("user");
        std::string pass = req.get_param_value("pass");
        std::string tel  = req.get_param_value("tel"); // <-- Le nouveau champ !

        // On vérifie que tout est rempli
        if (user.size() < 3 || pass.size() < 4 || tel.empty()) {
            res.status = 400; 
            res.set_content("{\"erreur\":\"Informations incomplètes ou trop courtes\"}", "application/json"); 
            return;
        }

        // On passe les 3 paramètres (N'oublie pas d'avoir mis à jour ta fonction registerUser() comme vu avant)
        if (registerUser(user, pass, tel)) {
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            res.status = 409; 
            res.set_content("{\"erreur\":\"Ce pseudo ou numéro de téléphone est déjà pris\"}", "application/json");
        }
    });

    svr.Post("/api/login", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = req.get_param_value("user");
        std::string pass = req.get_param_value("pass");

        // 1. Vérification classique du mot de passe
        if (verifyLogin(user, pass)) {
            
            // 2. Vérification du statut d'approbation
            sqlite3* db = openDatabase();
            sqlite3_stmt* stmtStatut;
            std::string statut = "";
            
            if (sqlite3_prepare_v2(db, "SELECT statut FROM utilisateurs WHERE username = ?;", -1, &stmtStatut, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmtStatut, 1, user.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(stmtStatut) == SQLITE_ROW) {
                    statut = (const char*)sqlite3_column_text(stmtStatut, 0);
                }
            }
            sqlite3_finalize(stmtStatut);
            
            // 3. Blocage si le compte est en attente
            if (statut == "pending") {
                sqlite3_close(db);
                res.status = 403; // Interdit (en attente)
                res.set_content("{\"erreur\":\"Votre compte est en attente de validation par un membre du staff.\"}", "application/json");
                return;
            }

            // 4. Si tout est OK, on connecte le joueur (Log session + Cookie)
            res.set_header("Set-Cookie", "auth_user=" + user + "; Path=/; HttpOnly; SameSite=Strict");
            
            sqlite3_stmt* ls;
            if (sqlite3_prepare_v2(db, "INSERT INTO sessions_log (username, ip, user_agent, action) VALUES (?, ?, ?, 'login');", -1, &ls, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ls, 1, user.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(ls, 2, req.remote_addr.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(ls, 3, req.has_header("User-Agent") ? req.get_header_value("User-Agent").c_str() : "", -1, SQLITE_STATIC);
                sqlite3_step(ls);
            }
            sqlite3_finalize(ls);
            sqlite3_close(db);    
            
            res.set_content("{\"status\":\"ok\"}", "application/json");
            
        } else {
            // Mauvais mot de passe ou pseudo
            res.status = 401;
            res.set_content("{\"erreur\":\"Identifiants incorrects\"}", "application/json");
        }
    });

    svr.Post("/api/logout", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", "auth_user=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        res.set_content("{\"status\":\"deconnecte\"}", "application/json");
    });

    // GET /api/historique
    svr.Get("/api/historique", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("symbole")) { res.status = 400; return; }

        std::string symbole = req.get_param_value("symbole");
        std::string periode = req.has_param("periode") ? req.get_param_value("periode") : "24h";
        std::string depuis  = req.has_param("depuis")  ? req.get_param_value("depuis")  : "";

        std::string modTemps = "-24 hours", fmtDate = "'%H:%M'";
        if (periode == "1h") { modTemps = "-1 hour"; fmtDate = "'%H:%M:%S'"; }
        else if (periode == "7d") { modTemps = "-7 days"; fmtDate = "'%d/%m %H:00'"; }

        sqlite3* db = openDatabase();
        sqlite3_stmt* stmt;
        json j = json::array();

        std::string sql;
        if (!depuis.empty()) {
            sql = "SELECT prix, strftime(" + fmtDate + ", timestamp), timestamp FROM historique "
                  "WHERE symbole = ? AND timestamp > ? ORDER BY timestamp ASC;";
        } else {
            sql = "SELECT prix, strftime(" + fmtDate + ", timestamp), timestamp FROM historique "
                  "WHERE symbole = ? AND timestamp >= datetime('now', '" + modTemps + "') ORDER BY timestamp ASC;";
        }

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, symbole.c_str(), -1, SQLITE_STATIC);
            if (!depuis.empty()) sqlite3_bind_text(stmt, 2, depuis.c_str(), -1, SQLITE_STATIC);
            
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                j.push_back({
                    {"price", sqlite3_column_double(stmt, 0)},
                    {"time", (const char*)sqlite3_column_text(stmt, 1)},
                    {"timestamp", (const char*)sqlite3_column_text(stmt, 2)}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/classement
    svr.Get("/api/classement", [](const httplib::Request&, httplib::Response& res) {
        sqlite3* db = openDatabase();
        json j = json::array();

        sqlite3_stmt* stmtUsers;
        if (sqlite3_prepare_v2(db, "SELECT id, username FROM utilisateurs;", -1, &stmtUsers, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmtUsers) == SQLITE_ROW) {
                int uid = sqlite3_column_int(stmtUsers, 0);
                std::string user = (const char*)sqlite3_column_text(stmtUsers, 1);
                
                double valeur = calculerValeurPortefeuille(db, uid);
                double snap = -1.0;
                
                // NOUVEAU : Récupération du solde USD pour chaque joueur
                double usd = 0.0;
                sqlite3_stmt* sp;
                if (sqlite3_prepare_v2(db, "SELECT quantite FROM portefeuilles WHERE user_id = ? AND symbole = 'USD';", -1, &sp, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(sp, 1, uid);
                    if (sqlite3_step(sp) == SQLITE_ROW) usd = sqlite3_column_double(sp, 0);
                }
                sqlite3_finalize(sp);
                
                sqlite3_stmt* stmtSnap;
                if (sqlite3_prepare_v2(db, "SELECT valeur FROM snapshots WHERE user_id = ?;", -1, &stmtSnap, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmtSnap, 1, uid);
                    if (sqlite3_step(stmtSnap) == SQLITE_ROW) snap = sqlite3_column_double(stmtSnap, 0);
                }
                sqlite3_finalize(stmtSnap);
                
                if (snap < 0) snap = 100000.0;

                double pnlTotal = (valeur - 100000.0) / 100000.0 * 100.0;
                double pnl24hPct = (valeur - snap) / snap * 100.0;
                double pnl24hUsd = valeur - snap;

                char bufTotal[32], buf24p[32], buf24u[32], bufTotUsd[32];
                snprintf(bufTotal, sizeof(bufTotal), "%+.2f%%", pnlTotal);
                snprintf(buf24p, sizeof(buf24p), "%+.2f%%", pnl24hPct);
                snprintf(buf24u, sizeof(buf24u), "%+.2f", pnl24hUsd);
                snprintf(bufTotUsd, sizeof(bufTotUsd), "%+.2f", valeur - 100000.0);

                j.push_back({
                    {"username", user},
                    {"valeur_totale", valeur},
                    {"usd", usd}, // <-- Ajout de {"usd", usd}
                    {"pnl", std::string(bufTotal)},
                    {"pnl_total_usd", std::string(bufTotUsd)},
                    {"pnl_24h_pct", std::string(buf24p)},
                    {"pnl_24h_usd", std::string(buf24u)}
                });
            }
        }
        sqlite3_finalize(stmtUsers);
        sqlite3_close(db);

        std::sort(j.begin(), j.end(), [](const json& a, const json& b) {
            return a["valeur_totale"].get<double>() > b["valeur_totale"].get<double>();
        });
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/trade
    svr.Post("/api/trade", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }
        
        if (!isGameRunning.load()) {
            res.status = 503;
            res.set_content("{\"erreur\":\"La partie est actuellement suspendue par l'administrateur.\"}", "application/json");
            return;
        }

        std::string symbole = req.get_param_value("symbole");
        std::string action  = req.get_param_value("action");
        double quantite = 0.0;

        try { quantite = std::stod(req.get_param_value("quantite")); }
        catch (...) { res.status = 400; res.set_content("{\"erreur\":\"Quantite invalide\"}", "application/json"); return; }

        for (char c : symbole) {
            if (!std::isalnum(c)&& c != '/') {
                res.status = 400; res.set_content("{\"erreur\":\"Symbole invalide\"}", "application/json"); return;
            }
        }

        if (action != "achat" && action != "vente") {
            res.status = 400; res.set_content("{\"erreur\":\"Action invalide\"}", "application/json"); return;
        }

        double prix = 0.0;
        {
            std::lock_guard<std::mutex> lock(marcheMutex);
            auto it = marcheMondial.find(symbole);
            if (it == marcheMondial.end()) {
                res.status = 404; res.set_content("{\"erreur\":\"Actif introuvable\"}", "application/json"); return;
            }
            prix = it->second.prix;
        }
        // 1. NOUVEAU CALCUL AVEC FRAIS
        double valeurTrade = prix * quantite;
        double frais = valeurTrade * 0.005; // 0.5%
        double coutAchat = valeurTrade + frais;
        double gainVente = valeurTrade - frais;

        sqlite3* db = openDatabase();
        int uid = getUserId(db, user);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, 0, nullptr);
        std::string msg;

        auto rollback = [&]() {
            sqlite3_exec(db, "ROLLBACK;", nullptr, 0, nullptr);
            sqlite3_close(db);
        };

        // LA VRAIE FONCTION AU LIEU DES "..."
        auto getQty = [&](const std::string& sym) -> double {
            double q = 0.0;
            sqlite3_stmt* s;
            if (sqlite3_prepare_v2(db, "SELECT quantite FROM portefeuilles WHERE user_id = ? AND symbole = ?;", -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, uid);
                sqlite3_bind_text(s, 2, sym.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(s) == SQLITE_ROW) q = sqlite3_column_double(s, 0);
            }
            sqlite3_finalize(s);
            return q;
        };

        if (action == "achat") {
            // Vérifier avec coutAchat au lieu de valeurTrade
            if (getQty("USD") < coutAchat) { 
                rollback();
                res.status = 403; res.set_content("{\"erreur\":\"Fonds USD insuffisants\"}", "application/json"); return;
            }
            
            sqlite3_stmt* debitUsd;
            if (sqlite3_prepare_v2(db, "UPDATE portefeuilles SET quantite = quantite - ? WHERE user_id = ? AND symbole = 'USD';", -1, &debitUsd, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(debitUsd, 1, coutAchat); // <-- Débiter coutAchat
                sqlite3_bind_int(debitUsd, 2, uid);
                sqlite3_step(debitUsd);
            }
            sqlite3_finalize(debitUsd);

            sqlite3_stmt* creditActif;
            if (sqlite3_prepare_v2(db, "INSERT INTO portefeuilles (user_id, symbole, quantite) VALUES (?, ?, ?) ON CONFLICT(user_id, symbole) DO UPDATE SET quantite = quantite + excluded.quantite;", -1, &creditActif, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(creditActif, 1, uid);
                sqlite3_bind_text(creditActif, 2, symbole.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_double(creditActif, 3, quantite);
                sqlite3_step(creditActif);
            }
            sqlite3_finalize(creditActif);
            msg = "Achat de " + std::to_string(quantite) + " " + symbole + " reussi !";

        } else { // Vente
            if (getQty(symbole) < quantite) {
                rollback();
                res.status = 403; res.set_content("{\"erreur\":\"Vous ne possedez pas assez de cet actif\"}", "application/json"); return;
            }
            
            sqlite3_stmt* debitActif;
            if (sqlite3_prepare_v2(db, "UPDATE portefeuilles SET quantite = quantite - ? WHERE user_id = ? AND symbole = ?;", -1, &debitActif, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(debitActif, 1, quantite);
                sqlite3_bind_int(debitActif, 2, uid);
                sqlite3_bind_text(debitActif, 3, symbole.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(debitActif);
            }
            sqlite3_finalize(debitActif);

            sqlite3_stmt* creditUsd;
            if (sqlite3_prepare_v2(db, "UPDATE portefeuilles SET quantite = quantite + ? WHERE user_id = ? AND symbole = 'USD';", -1, &creditUsd, nullptr) == SQLITE_OK) {
                sqlite3_bind_double(creditUsd, 1, gainVente); // <-- Créditer gainVente
                sqlite3_bind_int(creditUsd, 2, uid);
                sqlite3_step(creditUsd);
            }
            sqlite3_finalize(creditUsd);
            msg = "Vente reussie !";
        }

        sqlite3_stmt* logTrade;
        if (sqlite3_prepare_v2(db, "INSERT INTO trades_log (user_id, username, action, symbole, quantite, prix, valeur) VALUES (?, ?, ?, ?, ?, ?, ?);", -1, &logTrade, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(logTrade, 1, uid);
            sqlite3_bind_text(logTrade, 2, user.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(logTrade, 3, action.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(logTrade, 4, symbole.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_double(logTrade, 5, quantite);
            sqlite3_bind_double(logTrade, 6, prix);
            
            // On log la valeur nette (incluant les frais) dans l'historique
            sqlite3_bind_double(logTrade, 7, action == "achat" ? coutAchat : gainVente); 
            sqlite3_step(logTrade);
        }
        sqlite3_finalize(logTrade);

        sqlite3_exec(db, "COMMIT;", nullptr, 0, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\",\"message\":\"" + msg + "\"}", "application/json");
    });

    // GET /api/portefeuille
    svr.Get("/api/portefeuille", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }

        sqlite3* db = openDatabase();
        int uid = getUserId(db, user);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        json j = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT symbole, quantite FROM portefeuilles WHERE user_id = ? AND quantite > 0;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string sym = (const char*)sqlite3_column_text(stmt, 0);
                double qte = sqlite3_column_double(stmt, 1);
                double prix = 0.0;
                double valeur = 0.0;
                
                if (sym == "USD") {
                    prix = 1.0;
                    valeur = qte;
                } else {
                    std::lock_guard<std::mutex> lock(marcheMutex);
                    if (marcheMondial.count(sym)) {
                        prix = marcheMondial[sym].prix;
                        valeur = qte * prix;
                    }
                }
                j.push_back({ {"symbole", sym}, {"quantite", qte}, {"prix_unitaire", prix}, {"valeur", valeur} });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/contact (Le frontend ayant été nettoyé, cette route n'est plus appelée, 
    // mais je la laisse au propre au cas où tu remettes un formulaire).
    svr.Post("/api/contact", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }
        
        std::string sujet = req.get_param_value("sujet");
        std::string message = req.get_param_value("message");
        if (message.empty()) { res.status = 400; return; }

        sqlite3* db = openDatabase();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "INSERT INTO contacts (username, sujet, message) VALUES (?, ?, ?);", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, sujet.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ── Helper Admin ──────────────────────────────────────────────────────────
    auto isAdmin = [](const httplib::Request& req) -> bool {
        if (!req.has_header("Cookie")) return false;
        const std::string c = req.get_header_value("Cookie");
        return c.find(std::string(ADMIN_COOKIE) + "=1") != std::string::npos;
    };
    
    // ── Espace Admin ──────────────────────────────────────────────────────────
    
    svr.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/admin.html", std::ios::binary);
        if (!f) { res.status = 404; return; }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        res.set_content(content, "text/html");
    });
    
    svr.Post("/api/admin/login", [](const httplib::Request& req, httplib::Response& res) {
        std::string u = req.get_param_value("user");
        std::string p = req.get_param_value("pass");
        if (u == ADMIN_USER && p == ADMIN_PASS) {
            res.set_header("Set-Cookie", std::string(ADMIN_COOKIE) + "=1; Path=/; HttpOnly; SameSite=Strict");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            res.status = 401; res.set_content("{\"erreur\":\"Accès refusé\"}", "application/json");
        }
    });
    
    svr.Get("/api/admin/check", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (isAdmin(req)) res.set_content("{\"ok\":true}", "application/json");
        else { res.status = 401; res.set_content("{\"ok\":false}", "application/json"); }
    });
    
    svr.Post("/api/admin/logout", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", std::string(ADMIN_COOKIE) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    svr.Get("/api/admin/overview", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        sqlite3* db = openDatabase();
    
        int nbUsers = 0, nbTrades = 0;
        double volume = 0.0;
    
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM utilisateurs;", -1, &s, nullptr) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) nbUsers = sqlite3_column_int(s, 0);
        }
        sqlite3_finalize(s);
    
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(valeur),0) FROM trades_log;", -1, &s, nullptr) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) {
                nbTrades = sqlite3_column_int(s, 0);
                volume = sqlite3_column_double(s, 1);
            }
        }
        sqlite3_finalize(s);
    
        json recent = json::array();
        if (sqlite3_prepare_v2(db, "SELECT id, username, action, symbole, quantite, prix, valeur, timestamp FROM trades_log ORDER BY timestamp DESC LIMIT 10;", -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                recent.push_back({
                    {"id", sqlite3_column_int(s, 0)},
                    {"username", (const char*)sqlite3_column_text(s, 1)},
                    {"action", (const char*)sqlite3_column_text(s, 2)},
                    {"symbole", (const char*)sqlite3_column_text(s, 3)},
                    {"quantite", sqlite3_column_double(s, 4)},
                    {"prix", sqlite3_column_double(s, 5)},
                    {"valeur", sqlite3_column_double(s, 6)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 7)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
    
        json j = { {"nb_users", nbUsers}, {"nb_trades", nbTrades}, {"volume_total", volume}, {"game_running", isGameRunning.load()}, {"recent_trades", recent} };
        res.set_content(j.dump(), "application/json");
    });
    
    svr.Get("/api/admin/game-state", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        json j = {{"running", isGameRunning.load()}};
        res.set_content(j.dump(), "application/json");
    });
    
    svr.Post("/api/admin/game-state", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        std::string action = req.get_param_value("action");
        bool newState = (action == "start");
        isGameRunning.store(newState);
    
        sqlite3* db = openDatabase();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "INSERT INTO config (key, value) VALUES ('game_running', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, newState ? "1" : "0", -1, SQLITE_STATIC);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    
        json j = {{"running", newState}};
        res.set_content(j.dump(), "application/json");
    });
    
    svr.Get("/api/admin/users", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        sqlite3* db = openDatabase();
        json j = json::array();
        
        // NOUVEAU : On récupère explicitement le statut et le téléphone
        sqlite3_stmt* su;
        const char* sql = "SELECT id, username, created_at, statut, telephone FROM utilisateurs ORDER BY id ASC;";
        
        if (sqlite3_prepare_v2(db, sql, -1, &su, nullptr) == SQLITE_OK) {
            while (sqlite3_step(su) == SQLITE_ROW) {
                int uid = sqlite3_column_int(su, 0);
                std::string user = (const char*)sqlite3_column_text(su, 1);
                const char* cat = (const char*)sqlite3_column_text(su, 2);
                // On récupère les nouvelles valeurs
                const char* sts = (const char*)sqlite3_column_text(su, 3);
                const char* tel = (const char*)sqlite3_column_text(su, 4);
                
                double valeur = calculerValeurPortefeuille(db, uid);
                double usd = 0.0;
                
                sqlite3_stmt* sp;
                if (sqlite3_prepare_v2(db, "SELECT quantite FROM portefeuilles WHERE user_id = ? AND symbole = 'USD';", -1, &sp, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(sp, 1, uid);
                    if (sqlite3_step(sp) == SQLITE_ROW) usd = sqlite3_column_double(sp, 0);
                }
                sqlite3_finalize(sp);

                j.push_back({
                    {"id", uid}, 
                    {"username", user}, 
                    {"usd", usd},
                    {"valeur_totale", valeur}, 
                    {"pnl_pct", (valeur - 100000.0) / 1000.0},
                    {"created_at", cat ? cat : ""},
                    {"statut", sts ? sts : "pending"}, // On envoie le statut au JS
                    {"telephone", tel ? tel : ""}      // On envoie le tel au JS
                });
            }
        }
        sqlite3_finalize(su);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });
    
    // 🛡️ CORRIGÉ : Faille d'injection SQL supprimée !
    svr.Post("/api/admin/set-balance", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        int uid = std::stoi(req.get_param_value("user_id"));
        double amount = std::stod(req.get_param_value("amount"));
        
        sqlite3* db = openDatabase();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "INSERT INTO portefeuilles (user_id, symbole, quantite) VALUES (?, 'USD', ?) ON CONFLICT(user_id, symbole) DO UPDATE SET quantite = excluded.quantite;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            sqlite3_bind_double(stmt, 2, amount);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    // 🛡️ CORRIGÉ : Faille d'injection SQL supprimée !
    svr.Post("/api/admin/reset-user", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        int uid = std::stoi(req.get_param_value("user_id"));
        
        sqlite3* db = openDatabase();
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, 0, nullptr);
        
        auto execSimpleUid = [&](const char* query) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, uid);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        };

        execSimpleUid("DELETE FROM portefeuilles WHERE user_id = ?;");
        execSimpleUid("DELETE FROM snapshots WHERE user_id = ?;");
        
        // Réinsertion de l'argent de départ
        sqlite3_stmt* stmtUsd;
        if (sqlite3_prepare_v2(db, "INSERT INTO portefeuilles (user_id, symbole, quantite) VALUES (?, 'USD', 100000.0);", -1, &stmtUsd, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmtUsd, 1, uid);
            sqlite3_step(stmtUsd);
        }
        sqlite3_finalize(stmtUsd);

        sqlite3_exec(db, "COMMIT;", nullptr, 0, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    svr.Post("/api/admin/delete-user", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        int uid = std::stoi(req.get_param_value("user_id"));
        
        sqlite3* db = openDatabase();
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, 0, nullptr);
        
        for (const char* query : {
            "DELETE FROM portefeuilles WHERE user_id = ?;",
            "DELETE FROM snapshots WHERE user_id = ?;",
            "DELETE FROM trades_log WHERE user_id = ?;",
            "DELETE FROM utilisateurs WHERE id = ?;"
        }) {
            sqlite3_stmt* st;
            if (sqlite3_prepare_v2(db, query, -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(st, 1, uid);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
        sqlite3_exec(db, "COMMIT;", nullptr, 0, nullptr);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    svr.Get("/api/admin/trades", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        sqlite3* db = openDatabase();
        json j = json::array();
        
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db, "SELECT id, username, action, symbole, quantite, prix, valeur, timestamp FROM trades_log ORDER BY timestamp DESC LIMIT 1000;", -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                j.push_back({
                    {"id", sqlite3_column_int(s, 0)},
                    {"username", (const char*)sqlite3_column_text(s, 1)},
                    {"action", (const char*)sqlite3_column_text(s, 2)},
                    {"symbole", (const char*)sqlite3_column_text(s, 3)},
                    {"quantite", sqlite3_column_double(s, 4)},
                    {"prix", sqlite3_column_double(s, 5)},
                    {"valeur", sqlite3_column_double(s, 6)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 7)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });
    
    // 🛡️ CORRIGÉ : Faille d'injection SQL supprimée !
    svr.Post("/api/admin/delete-trade", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        int tid = std::stoi(req.get_param_value("trade_id"));
        
        sqlite3* db = openDatabase();
        sqlite3_stmt* st;
        if (sqlite3_prepare_v2(db, "DELETE FROM trades_log WHERE id = ?;", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, tid);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });
    
    svr.Get("/api/admin/sessions", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        sqlite3* db = openDatabase();
        json j = json::array();
        
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db, "SELECT username, ip, user_agent, action, timestamp FROM sessions_log ORDER BY timestamp DESC LIMIT 2000;", -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char* ua = (const char*)sqlite3_column_text(s, 2);
                j.push_back({
                    {"username", (const char*)sqlite3_column_text(s, 0)},
                    {"ip", (const char*)sqlite3_column_text(s, 1)},
                    {"user_agent", ua ? ua : ""},
                    {"action", (const char*)sqlite3_column_text(s, 3)},
                    {"timestamp", (const char*)sqlite3_column_text(s, 4)}
                });
            }
        }
        sqlite3_finalize(s);
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/admin/approve-user
    svr.Post("/api/admin/approve-user", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = openDatabase();
        
        // On passe le statut à 'approved' ET on lui donne ses 100 000$ de départ
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, 0, nullptr);
        std::string sql1 = "UPDATE utilisateurs SET statut = 'approved' WHERE id = " + std::to_string(uid) + ";";
        std::string sql2 = "INSERT OR IGNORE INTO portefeuilles (user_id, symbole, quantite) VALUES (" + std::to_string(uid) + ", 'USD', 100000.0);";
        
        sqlite3_exec(db, sql1.c_str(), nullptr, 0, nullptr);
        sqlite3_exec(db, sql2.c_str(), nullptr, 0, nullptr);
        sqlite3_exec(db, "COMMIT;", nullptr, 0, nullptr);
        sqlite3_close(db);
        
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // 🛡️ GESTION DU PORTEFEUILLE PAR L'ADMIN
    svr.Post("/api/admin/portfolio", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        
        // 1. Vérification de la présence des paramètres
        if (!req.has_param("user_id") || !req.has_param("symbole") || !req.has_param("quantite")) {
            res.status = 400;
            res.set_content("{\"erreur\":\"Parametres manquants (user_id, symbole, quantite)\"}", "application/json");
            return;
        }

        int uid = 0;
        double quantite = 0.0;
        try {
            uid = std::stoi(req.get_param_value("user_id"));
            quantite = std::stod(req.get_param_value("quantite"));
        } catch (...) {
            res.status = 400; res.set_content("{\"erreur\":\"Format de nombre invalide\"}", "application/json"); return;
        }
        
        // On met le symbole en majuscules pour éviter les doublons (ex: btc/usd vs BTC/USD)
        std::string symbole = req.get_param_value("symbole");
        for (auto & c: symbole) c = toupper(c);

        sqlite3* db = openDatabase();
        
        if (quantite <= 0.0) {
            // 2A. Si la quantité est 0, on supprime l'actif du portefeuille
            sqlite3_stmt* stmtDel;
            if (sqlite3_prepare_v2(db, "DELETE FROM portefeuilles WHERE user_id = ? AND symbole = ?;", -1, &stmtDel, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmtDel, 1, uid);
                sqlite3_bind_text(stmtDel, 2, symbole.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmtDel);
            }
            sqlite3_finalize(stmtDel);
        } else {
            // 2B. Si > 0, on l'ajoute ou on écrase l'ancienne quantité (Force Set)
            sqlite3_stmt* stmtUpsert;
            const char* sql = "INSERT INTO portefeuilles (user_id, symbole, quantite) VALUES (?, ?, ?) ON CONFLICT(user_id, symbole) DO UPDATE SET quantite = excluded.quantite;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmtUpsert, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmtUpsert, 1, uid);
                sqlite3_bind_text(stmtUpsert, 2, symbole.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_double(stmtUpsert, 3, quantite);
                sqlite3_step(stmtUpsert);
            }
            sqlite3_finalize(stmtUpsert);
        }
        
        sqlite3_close(db);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // 🛡️ LECTURE DU PORTEFEUILLE D'UN JOUEUR (ADMIN)
    svr.Get("/api/admin/user-portfolio", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (!isAdmin(req)) { res.status = 401; return; }
        if (!req.has_param("user_id")) { res.status = 400; return; }
        
        int uid = std::stoi(req.get_param_value("user_id"));
        sqlite3* db = openDatabase();
        json j = json::array();
        
        sqlite3_stmt* stmt;
        // On exclut l'USD car il est géré par le bouton "Liquidités"
        if (sqlite3_prepare_v2(db, "SELECT symbole, quantite FROM portefeuilles WHERE user_id = ? AND symbole != 'USD';", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                j.push_back({
                    {"symbole", (const char*)sqlite3_column_text(stmt, 0)},
                    {"quantite", sqlite3_column_double(stmt, 1)}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        
        res.set_content(j.dump(), "application/json");
    });

    std::cout << "Serveur lancé sur http://localhost:8080" << std::endl;
    svr.listen("0.0.0.0", 8080);
    
    return 0;
}