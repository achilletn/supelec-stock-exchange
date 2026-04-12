#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <random>
#include <openssl/evp.h>
#include <openssl/rand.h>

using json = nlohmann::json;

static std::string ADMIN_USER;
static std::string ADMIN_PASS;
static std::string TWELVEDATA_API_KEY;
static const std::string ADMIN_COOKIE = "admin_session";
 
std::atomic<bool> isGameRunning{true};
std::atomic<long long> lastMarketUpdateMs{0};

// ── Brute-force protection ────────────────────────────────────────────────
static const int BF_MAX_ATTEMPTS = 5;    // tentatives avant blocage
static const int BF_WINDOW_SEC   = 300;  // fenêtre de comptage (5 min)
static const int BF_BLOCK_SEC    = 900;  // durée de blocage (15 min)

struct BruteForceEntry {
    int attempts = 0;
    std::chrono::steady_clock::time_point first_attempt;
    std::chrono::steady_clock::time_point blocked_until;
};
static std::map<std::string, BruteForceEntry> bruteForceMap;
static std::mutex bruteforceMutex;

static bool isRateLimited(const std::string& ip) {
    std::lock_guard<std::mutex> lock(bruteforceMutex);
    auto it = bruteForceMap.find(ip);
    if (it == bruteForceMap.end()) return false;
    auto& e = it->second;
    auto now = std::chrono::steady_clock::now();
    if (now < e.blocked_until) return true;
    // Fenêtre expirée → on repart à zéro
    if (std::chrono::duration_cast<std::chrono::seconds>(now - e.first_attempt).count() > BF_WINDOW_SEC)
        bruteForceMap.erase(it);
    return false;
}

static void recordFailedAttempt(const std::string& ip) {
    std::lock_guard<std::mutex> lock(bruteforceMutex);
    auto now = std::chrono::steady_clock::now();
    auto& e = bruteForceMap[ip];
    // Réinitialise si la fenêtre est expirée
    if (e.attempts > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(now - e.first_attempt).count() > BF_WINDOW_SEC)
        e = BruteForceEntry{};
    if (e.attempts == 0) e.first_attempt = now;
    e.attempts++;
    if (e.attempts >= BF_MAX_ATTEMPTS) {
        e.blocked_until = now + std::chrono::seconds(BF_BLOCK_SEC);
        std::cout << "[SECURITY] IP " << ip << " bloquée (bruteforce, "
                  << BF_MAX_ATTEMPTS << " échecs)." << std::endl;
    }
}

static void clearFailedAttempts(const std::string& ip) {
    std::lock_guard<std::mutex> lock(bruteforceMutex);
    bruteForceMap.erase(ip);
}

struct ActionItem {
    std::string nom;
    double prix        = 0.0;
    double variation_24h = 0.0;
    double open        = 0.0;
    double high        = 0.0;
    double low         = 0.0;
    double close       = 0.0;   // == prix, alias lisible
};
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

// Extrait la valeur d'un cookie par son nom
static std::string getCookieValue(const httplib::Request& req, const std::string& name) {
    if (!req.has_header("Cookie")) return "";
    const std::string cookies = req.get_header_value("Cookie");
    std::string search = name + "=";
    size_t pos = cookies.find(search);
    if (pos == std::string::npos) return "";
    size_t start = pos + search.size();
    size_t end = cookies.find(";", start);
    return cookies.substr(start, end - start);
}

// Résout le token de session en nom d'utilisateur (lookup DB)
static std::string getCookieUser(const httplib::Request& req) {
    std::string token = getCookieValue(req, "auth_session");
    if (token.empty()) return "";

    sqlite3* db = openDatabase();
    std::string username;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT username FROM user_sessions WHERE token = ? AND expires_at > datetime('now');",
        -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            username = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return username;
}

// ── Password hashing (PBKDF2-SHA256 via OpenSSL) ─────────────────────────

static std::string toHex(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    for (size_t i = 0; i < len; i++)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return ss.str();
}

static std::vector<unsigned char> fromHex(const std::string& hex) {
    std::vector<unsigned char> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes.push_back((unsigned char)std::stoi(hex.substr(i, 2), nullptr, 16));
    return bytes;
}

static std::string generateToken() {
    unsigned char buf[32];
    RAND_bytes(buf, 32);
    return toHex(buf, 32);
}

// Format stocké : pbkdf2:<iterations>:<salt_hex>:<hash_hex>
static std::string hashPassword(const std::string& plain) {
    const int ITERATIONS = 100000;
    const int SALT_LEN   = 16;
    const int HASH_LEN   = 32;
    unsigned char salt[SALT_LEN], hash[HASH_LEN];
    RAND_bytes(salt, SALT_LEN);
    PKCS5_PBKDF2_HMAC(plain.c_str(), (int)plain.size(),
                      salt, SALT_LEN, ITERATIONS, EVP_sha256(),
                      HASH_LEN, hash);
    return "pbkdf2:" + std::to_string(ITERATIONS) + ":" +
           toHex(salt, SALT_LEN) + ":" + toHex(hash, HASH_LEN);
}

static bool verifyPassword(const std::string& plain, const std::string& stored) {
    // Compatibilité migration : anciens mots de passe en clair
    if (stored.find("pbkdf2:") != 0)
        return plain == stored;

    // Parser pbkdf2:<iter>:<salt_hex>:<hash_hex>
    std::istringstream ss(stored);
    std::string tag, iterStr, saltHex, hashHex;
    std::getline(ss, tag,     ':');
    std::getline(ss, iterStr, ':');
    std::getline(ss, saltHex, ':');
    std::getline(ss, hashHex);
    if (saltHex.empty() || hashHex.empty()) return false;

    int iterations = std::stoi(iterStr);
    auto salt      = fromHex(saltHex);
    auto expected  = fromHex(hashHex);
    const int HASH_LEN = 32;
    if ((int)expected.size() != HASH_LEN) return false;

    unsigned char computed[HASH_LEN];
    PKCS5_PBKDF2_HMAC(plain.c_str(), (int)plain.size(),
                      salt.data(), (int)salt.size(),
                      iterations, EVP_sha256(), HASH_LEN, computed);

    // Comparaison en temps constant pour éviter les timing attacks
    unsigned char diff = 0;
    for (int i = 0; i < HASH_LEN; i++) diff |= computed[i] ^ expected[i];
    return diff == 0;
}

static bool verifyLogin(const std::string& user, const std::string& pass) {
    sqlite3* db = openDatabase();
    bool valid = false;
    bool needsUpgrade = false;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT password FROM utilisateurs WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, user.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string stored = (const char*)sqlite3_column_text(stmt, 0);
            valid = verifyPassword(pass, stored);
            needsUpgrade = valid && stored.find("pbkdf2:") != 0;
        }
    }
    sqlite3_finalize(stmt);

    // Migration automatique des anciens mots de passe en clair
    if (needsUpgrade) {
        std::string newHash = hashPassword(pass);
        sqlite3_stmt* upd;
        if (sqlite3_prepare_v2(db, "UPDATE utilisateurs SET password = ? WHERE username = ?;", -1, &upd, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, newHash.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(upd, 2, user.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(upd);
        }
        sqlite3_finalize(upd);
        std::cout << "[SECURITY] Mot de passe de '" << user << "' migré vers PBKDF2." << std::endl;
    }

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
        sqlite3_bind_text(stmt, 3, hashPassword(pass).c_str(), -1, SQLITE_TRANSIENT);
        
        if (sqlite3_step(stmt) == SQLITE_DONE) ok = true;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

// ── Simulation de marché ──────────────────────────────────────────────────

static std::mt19937 rng_sim(std::chrono::steady_clock::now().time_since_epoch().count());
static double randn() {
    static std::normal_distribution<double> dist(0.0, 1.0);
    return dist(rng_sim);
}

struct SimActif { double prix_initial; double sigma; double drift; };
static const std::vector<std::string> SIM_SYMBOLES = {
    // Big tech
    "AAPL","MSFT","NVDA","TSLA","GOOGL","AMZN","META","NFLX",
    // Semiconducteurs
    "AMD","INTC","QCOM","AVGO","TSM","ASML","MU","AMAT",
    // Finance
    "JPM","GS","BAC","V","MA","BRK","AXP","BLK",
    // Santé & pharma
    "JNJ","PFE","LLY","ABBV","MRK","UNH","BMY","GILD",
    // Industrie & énergie
    "XOM","CVX","NEE","CAT","BA","GE","RTX","HON",
    // Consommation & retail
    "WMT","COST","TGT","NKE","SBUX","MCD","DIS","PYPL"
};
static const std::map<std::string, SimActif> SIM_CONFIG = {
    // Big tech
    {"AAPL",  {210.0, 0.25, 0.07}},
    {"MSFT",  {420.0, 0.28, 0.09}},
    {"NVDA",  {120.0, 0.55, 0.18}},
    {"TSLA",  {250.0, 0.65, 0.05}},
    {"GOOGL", {175.0, 0.30, 0.08}},
    {"AMZN",  {195.0, 0.35, 0.09}},
    {"META",  {600.0, 0.40, 0.11}},
    {"NFLX",  {950.0, 0.45, 0.06}},
    // Semiconducteurs
    {"AMD",   {180.0, 0.50, 0.12}},
    {"INTC",  { 35.0, 0.35, 0.02}},
    {"QCOM",  {170.0, 0.35, 0.07}},
    {"AVGO",  {185.0, 0.30, 0.10}},
    {"TSM",   {130.0, 0.32, 0.09}},
    {"ASML",  {850.0, 0.30, 0.11}},
    {"MU",    { 90.0, 0.50, 0.08}},
    {"AMAT",  {200.0, 0.40, 0.10}},
    // Finance
    {"JPM",   {215.0, 0.22, 0.08}},
    {"GS",    {520.0, 0.25, 0.07}},
    {"BAC",   { 42.0, 0.28, 0.06}},
    {"V",     {290.0, 0.20, 0.09}},
    {"MA",    {480.0, 0.22, 0.09}},
    {"BRK",   {440.0, 0.18, 0.07}},
    {"AXP",   {265.0, 0.28, 0.08}},
    {"BLK",   {920.0, 0.25, 0.08}},
    // Santé & pharma
    {"JNJ",   {155.0, 0.18, 0.05}},
    {"PFE",   { 28.0, 0.30, 0.03}},
    {"LLY",   {800.0, 0.32, 0.14}},
    {"ABBV",  {175.0, 0.25, 0.07}},
    {"MRK",   {130.0, 0.22, 0.06}},
    {"UNH",   {530.0, 0.22, 0.09}},
    {"BMY",   { 58.0, 0.28, 0.04}},
    {"GILD",  { 90.0, 0.25, 0.05}},
    // Industrie & énergie
    {"XOM",   {110.0, 0.28, 0.05}},
    {"CVX",   {155.0, 0.27, 0.05}},
    {"NEE",   { 72.0, 0.22, 0.06}},
    {"CAT",   {350.0, 0.28, 0.08}},
    {"BA",    {190.0, 0.42, 0.03}},
    {"GE",    {170.0, 0.30, 0.07}},
    {"RTX",   {120.0, 0.25, 0.07}},
    {"HON",   {225.0, 0.22, 0.07}},
    // Consommation & retail
    {"WMT",   { 95.0, 0.18, 0.06}},
    {"COST",  {890.0, 0.20, 0.09}},
    {"TGT",   {145.0, 0.30, 0.05}},
    {"NKE",   { 92.0, 0.28, 0.06}},
    {"SBUX",  { 95.0, 0.30, 0.05}},
    {"MCD",   {295.0, 0.18, 0.07}},
    {"DIS",   { 95.0, 0.32, 0.04}},
    {"PYPL",  { 70.0, 0.45, 0.04}},
};

static std::string toSQLiteDateTime(time_t t) {
    struct tm* ti = gmtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ti);
    return std::string(buf);
}

// Génère l'historique simulé à chaque démarrage (efface et recrée)
static void genererHistoriqueSimule(sqlite3* db) {
    std::cout << "[SIM] Purge de l'historique existant..." << std::endl;
    sqlite3_exec(db, "DELETE FROM historique;", nullptr, 0, nullptr);

    std::cout << "[SIM] Génération de l'historique simulé (5 ans)..." << std::endl;

    sqlite3_stmt* st;
    time_t now = time(nullptr);
    // Périodes : {début (secondes avant now), pas en secondes}
    struct PeriodeDef { long long debut; int pas; };
    const std::vector<PeriodeDef> periodes = {
        {5LL*365*86400, 86400  },   // -5Y → -1Y  : 1 point/jour
        {1LL*365*86400, 21600  },   // -1Y → -30J : 1 point/6h
        {30LL*86400,    3600   },   // -30J → -7J : 1 point/h
        {7LL*86400,     1800   },   // -7J → -1J  : 1 point/30min
        {1LL*86400,     300    },   // -1J → -3H  : 1 point/5min
        {3LL*3600,      30     },   // -3H → now  : 1 point/30sec
    };

    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, 0, nullptr);

    for (const auto& sym : SIM_SYMBOLES) {
        const auto& cfg = SIM_CONFIG.at(sym);
        // Partir d'un prix initial plus bas pour que la courbe ait du relief
        double prix = cfg.prix_initial * 0.35;

        time_t cursor = now - 5LL*365*86400;
        int pointsIdx = 0;

        for (int p = 0; p < (int)periodes.size(); p++) {
            time_t fin = (p + 1 < (int)periodes.size())
                ? now - periodes[p+1].debut
                : now;
            int pas = periodes[p].pas;
            double dt = (double)pas / (252.0 * 86400.0); // fraction d'année

            while (cursor < fin) {
                // GBM : S(t+dt) = S(t) * exp((mu - σ²/2)*dt + σ*sqrt(dt)*Z)
                double z = randn();
                prix *= std::exp((cfg.drift - 0.5*cfg.sigma*cfg.sigma)*dt
                                 + cfg.sigma * std::sqrt(dt) * z);
                if (prix < 1.0) prix = 1.0;

                std::string ts = toSQLiteDateTime(cursor);
                if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO historique (symbole, prix, timestamp) VALUES (?, ?, ?);",
                    -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
                    sqlite3_bind_double(st, 2, prix);
                    sqlite3_bind_text(st, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(st);
                }
                sqlite3_finalize(st);

                cursor += pas;
                pointsIdx++;
            }
        }
        // Initialiser marcheMondial avec le dernier prix généré
        {
            std::lock_guard<std::mutex> lock(marcheMutex);
            marcheMondial[sym].prix  = prix;
            marcheMondial[sym].close = prix;
            marcheMondial[sym].nom   = sym;
        }
        std::cout << "[SIM] " << sym << " généré (" << pointsIdx << " points), prix actuel = " << prix << " $" << std::endl;
    }

    sqlite3_exec(db, "COMMIT;", nullptr, 0, nullptr);
    std::cout << "[SIM] Historique généré avec succès." << std::endl;
}

// Rafraîchit variation_24h, open, high, low depuis l'historique
static void refreshStatsMarcheSimule() {
    sqlite3* db = openDatabase();
    sqlite3_stmt* st;
    for (const auto& sym : SIM_SYMBOLES) {
        double prix_24h = 0.0, open_v = 0.0, high_v = 0.0, low_v = 0.0;

        if (sqlite3_prepare_v2(db,
            "SELECT prix FROM historique WHERE symbole = ? AND timestamp <= datetime('now','-24 hours') ORDER BY timestamp DESC LIMIT 1;",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) prix_24h = sqlite3_column_double(st, 0);
        }
        sqlite3_finalize(st);

        if (sqlite3_prepare_v2(db,
            "SELECT prix FROM historique WHERE symbole = ? AND timestamp >= datetime('now','-24 hours') ORDER BY timestamp ASC LIMIT 1;",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) open_v = sqlite3_column_double(st, 0);
        }
        sqlite3_finalize(st);

        if (sqlite3_prepare_v2(db,
            "SELECT MIN(prix), MAX(prix) FROM historique WHERE symbole = ? AND timestamp >= datetime('now','-24 hours');",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                low_v  = sqlite3_column_double(st, 0);
                high_v = sqlite3_column_double(st, 1);
            }
        }
        sqlite3_finalize(st);

        std::lock_guard<std::mutex> lock(marcheMutex);
        auto& a = marcheMondial[sym];
        if (prix_24h > 0.0) a.variation_24h = (a.prix - prix_24h) / prix_24h * 100.0;
        if (open_v  > 0.0) a.open = open_v;
        if (high_v  > 0.0) a.high = high_v;
        if (low_v   > 0.0) a.low  = low_v;
    }
    sqlite3_close(db);
}

// Worker simulation : random walk toutes les 5s, refresh stats toutes les 60s
void workerSimulerCours() {
    const double DT = 5.0 / (252.0 * 86400.0); // pas de 5s en fraction d'année
    int tick = 0;
    // Attendre que marcheMondial soit initialisé
    std::this_thread::sleep_for(std::chrono::seconds(1));
    while (true) {
        {
            std::lock_guard<std::mutex> lock(marcheMutex);
            for (const auto& sym : SIM_SYMBOLES) {
                if (!SIM_CONFIG.count(sym)) continue;
                const auto& cfg = SIM_CONFIG.at(sym);
                auto& a = marcheMondial[sym];
                if (a.prix <= 0.0) a.prix = cfg.prix_initial;

                double z = randn();
                a.prix *= std::exp((cfg.drift - 0.5*cfg.sigma*cfg.sigma)*DT
                                   + cfg.sigma * std::sqrt(DT) * z);
                if (a.prix < 0.5) a.prix = 0.5;
                a.close = a.prix;
                a.nom   = sym;
            }
        }
        lastMarketUpdateMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        // Refresh stats toutes les 60s (tick = 12 × 5s)
        if (++tick >= 12) {
            tick = 0;
            refreshStatsMarcheSimule();
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// ── Workers ───────────────────────────────────────────────────────────────

/* [API DÉSACTIVÉE — conservée pour réactivation future]
void workerActualiserCours() {
    const std::vector<std::string> mesActifs = {
        "AAPL", "MSFT", "NVDA", "TSLA", "GOOGL", "AMZN", "META", "NFLX"
    };
    std::string symbols;
    for (size_t i = 0; i < mesActifs.size(); ++i)
        symbols += mesActifs[i] + (i < mesActifs.size() - 1 ? "," : "");
    while (true) {
        try {
            httplib::Client cli("https://api.twelvedata.com");
            cli.set_connection_timeout(10); cli.set_read_timeout(10);
            std::string path = "/quote?symbol=" + symbols + "&apikey=" + TWELVEDATA_API_KEY;
            auto res = cli.Get(path.c_str());
            if (res && res->status == 200) {
                auto j = json::parse(res->body);
                if (j.contains("code") && j["code"] == 429) {
                    std::cout << "[QUOTA] Limite TwelveData atteinte." << std::endl;
                } else {
                    std::lock_guard<std::mutex> lock(marcheMutex);
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        const std::string& sym = it.key();
                        const auto& data = it.value();
                        if (!data.is_object() || !data.contains("close")) continue;
                        try {
                            double prix = std::stod(data["close"].get<std::string>());
                            if (prix <= 0.0) continue;
                            marcheMondial[sym].prix  = prix; marcheMondial[sym].close = prix;
                            marcheMondial[sym].nom   = data.value("name", sym);
                            if (data.contains("percent_change"))
                                marcheMondial[sym].variation_24h = std::stod(data["percent_change"].get<std::string>());
                            if (data.contains("open") && data["open"].is_string())
                                marcheMondial[sym].open = std::stod(data["open"].get<std::string>());
                            if (data.contains("high") && data["high"].is_string())
                                marcheMondial[sym].high = std::stod(data["high"].get<std::string>());
                            if (data.contains("low") && data["low"].is_string())
                                marcheMondial[sym].low  = std::stod(data["low"].get<std::string>());
                        } catch (...) {}
                    }
                    lastMarketUpdateMs.store(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  [EXCEPTION] " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(65));
    }
}
*/

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
    // Chargement des credentials admin depuis les variables d'environnement
    const char* envUser   = std::getenv("ADMIN_USER");
    const char* envPass   = std::getenv("ADMIN_PASS");
    const char* envApiKey = std::getenv("TWELVEDATA_API_KEY");
    if (!envUser || !envPass || std::string(envUser).empty() || std::string(envPass).empty()) {
        std::cerr << "[FATAL] Les variables d'environnement ADMIN_USER et ADMIN_PASS doivent être définies." << std::endl;
        return 1;
    }
    // API key optionnelle (mode simulation actif)
    ADMIN_USER = envUser;
    ADMIN_PASS = envPass;
    if (envApiKey && !std::string(envApiKey).empty())
        TWELVEDATA_API_KEY = envApiKey;
    else
        std::cout << "[INFO] TWELVEDATA_API_KEY non définie — mode simulation activé." << std::endl;

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
            prix_moyen REAL DEFAULT 0,
            FOREIGN KEY(user_id) REFERENCES utilisateurs(id),
            UNIQUE(user_id, symbole)
        );
        CREATE TABLE IF NOT EXISTS historique (id INTEGER PRIMARY KEY AUTOINCREMENT, symbole TEXT, prix REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS snapshots (user_id INTEGER PRIMARY KEY, valeur REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(user_id) REFERENCES utilisateurs(id));
        CREATE TABLE IF NOT EXISTS daily_history (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER, date TEXT, pnl_day_pct REAL, rank INTEGER, FOREIGN KEY(user_id) REFERENCES utilisateurs(id), UNIQUE(user_id, date));
        CREATE TABLE IF NOT EXISTS contacts (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT, sujet TEXT, message TEXT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS trades_log (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER, username TEXT, action TEXT, symbole TEXT, quantite REAL, prix REAL, valeur REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(user_id) REFERENCES utilisateurs(id));
        CREATE TABLE IF NOT EXISTS sessions_log (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT, ip TEXT, user_agent TEXT, action TEXT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);
        CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS user_sessions (
            token TEXT PRIMARY KEY,
            username TEXT NOT NULL,
            expires_at DATETIME NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS admin_sessions (
            token TEXT PRIMARY KEY,
            expires_at DATETIME NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        INSERT OR IGNORE INTO actifs (symbole, nom, type) VALUES ('USD','Dollar Americain','fiat'),('BTCUSDT','Bitcoin','crypto'),('ETHUSDT','Ethereum','crypto');
        INSERT OR IGNORE INTO config (key, value) VALUES ('game_running', '1');
    )";
    sqlite3_exec(db, initSql, nullptr, 0, nullptr);
    // Migrations (ignore errors if columns/tables already exist)
    sqlite3_exec(db, "ALTER TABLE portefeuilles ADD COLUMN prix_moyen REAL DEFAULT 0;", nullptr, 0, nullptr);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS daily_history (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER, date TEXT, pnl_day_pct REAL, rank INTEGER, FOREIGN KEY(user_id) REFERENCES utilisateurs(id), UNIQUE(user_id, date));", nullptr, 0, nullptr);

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

    // Pré-charger les derniers prix connus depuis l'historique (résistance au rate-limit)
    {
        sqlite3_stmt* st;
        const char* sqlPrix = R"(
            SELECT h.symbole, h.prix
            FROM historique h
            INNER JOIN (SELECT symbole, MAX(id) AS max_id FROM historique GROUP BY symbole) latest
            ON h.id = latest.max_id;
        )";
        if (sqlite3_prepare_v2(db, sqlPrix, -1, &st, nullptr) == SQLITE_OK) {
            std::lock_guard<std::mutex> lock(marcheMutex);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string sym  = (const char*)sqlite3_column_text(st, 0);
                double      prix = sqlite3_column_double(st, 1);
                if (prix > 0.0) {
                    marcheMondial[sym].prix  = prix;
                    marcheMondial[sym].close = prix;
                    marcheMondial[sym].nom   = sym;
                    std::cout << "[INIT] " << sym << " = " << prix << " $ (historique)" << std::endl;
                }
            }
            sqlite3_finalize(st);
        }

        // Initialiser lastMarketUpdateMs depuis le timestamp de la dernière entrée
        sqlite3_stmt* stTs;
        if (sqlite3_prepare_v2(db,
            "SELECT CAST(strftime('%s', MAX(timestamp)) AS INTEGER) FROM historique;",
            -1, &stTs, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stTs) == SQLITE_ROW && sqlite3_column_type(stTs, 0) != SQLITE_NULL) {
                long long ts = sqlite3_column_int64(stTs, 0) * 1000LL;
                lastMarketUpdateMs.store(ts);
                std::cout << "[INIT] lastMarketUpdateMs = " << ts << " (depuis historique)" << std::endl;
            }
            sqlite3_finalize(stTs);
        }
    }

    // Générer l'historique simulé (purge + recréation à chaque démarrage)
    genererHistoriqueSimule(db);
    sqlite3_close(db);

    // Pré-calculer high/low/open/variation immédiatement (sinon on attend 60s)
    refreshStatsMarcheSimule();

    // Démarrer le worker de simulation (remplace workerActualiserCours)
    std::thread(workerSimulerCours).detach();

    // Routine : Snapshots 24h + enregistrement daily_history
    std::thread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(24));
            sqlite3* db = openDatabase();

            // 1. Calculer la valeur de chaque joueur et récupérer son snapshot actuel
            struct UserDay { int uid; double valeur; double snap; };
            std::vector<UserDay> users;
            sqlite3_stmt* su;
            if (sqlite3_prepare_v2(db, "SELECT u.id, COALESCE(s.valeur, 100000.0) FROM utilisateurs u LEFT JOIN snapshots s ON s.user_id = u.id;", -1, &su, nullptr) == SQLITE_OK) {
                while (sqlite3_step(su) == SQLITE_ROW) {
                    int uid = sqlite3_column_int(su, 0);
                    double snap = sqlite3_column_double(su, 1);
                    double val = calculerValeurPortefeuille(db, uid);
                    users.push_back({uid, val, snap});
                }
            }
            sqlite3_finalize(su);

            // 2. Trier par valeur décroissante pour le classement
            std::sort(users.begin(), users.end(), [](const UserDay& a, const UserDay& b) { return a.valeur > b.valeur; });

            // 3. Récupérer la date du jour (UTC)
            time_t now = time(nullptr);
            char dateStr[16];
            strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", gmtime(&now));

            // 4. Enregistrer le PnL du jour et le rang dans daily_history
            for (int i = 0; i < (int)users.size(); i++) {
                auto& u = users[i];
                double pnl_day = u.snap > 0 ? (u.valeur - u.snap) / u.snap * 100.0 : 0.0;
                int rank = i + 1;
                sqlite3_stmt* stmtHist;
                if (sqlite3_prepare_v2(db,
                    "INSERT INTO daily_history (user_id, date, pnl_day_pct, rank) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(user_id, date) DO UPDATE SET pnl_day_pct = excluded.pnl_day_pct, rank = excluded.rank;",
                    -1, &stmtHist, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmtHist, 1, u.uid);
                    sqlite3_bind_text(stmtHist, 2, dateStr, -1, SQLITE_STATIC);
                    sqlite3_bind_double(stmtHist, 3, pnl_day);
                    sqlite3_bind_int(stmtHist, 4, rank);
                    sqlite3_step(stmtHist);
                }
                sqlite3_finalize(stmtHist);

                // 5. Mettre à jour le snapshot
                sqlite3_stmt* stmtSnap;
                if (sqlite3_prepare_v2(db, "INSERT INTO snapshots (user_id, valeur) VALUES (?, ?) ON CONFLICT(user_id) DO UPDATE SET valeur = excluded.valeur, timestamp = CURRENT_TIMESTAMP;", -1, &stmtSnap, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(stmtSnap, 1, u.uid);
                    sqlite3_bind_double(stmtSnap, 2, u.valeur);
                    sqlite3_step(stmtSnap);
                }
                sqlite3_finalize(stmtSnap);
            }

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

    // Routine : Nettoyage des sessions expirées (toutes les heures)
    std::thread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(1));
            sqlite3* db = openDatabase();
            sqlite3_exec(db, "DELETE FROM user_sessions  WHERE expires_at <= datetime('now');", nullptr, 0, nullptr);
            sqlite3_exec(db, "DELETE FROM admin_sessions WHERE expires_at <= datetime('now');", nullptr, 0, nullptr);
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
            j.push_back({
                {"symbol",       key},
                {"price",        val.prix},
                {"variation_24h",val.variation_24h},
                {"open",         val.open},
                {"high",         val.high},
                {"low",          val.low},
                {"close",        val.close}
            });
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/stats?symbole=XXX — stats étendues + volatilité calculée depuis l'historique
    svr.Get("/api/stats", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("symbole")) { res.status = 400; return; }
        const std::string sym = req.get_param_value("symbole");

        // 1. Données en mémoire
        double open = 0, high = 0, low = 0, close = 0, variation = 0;
        {
            std::lock_guard<std::mutex> lock(marcheMutex);
            if (marcheMondial.count(sym)) {
                auto& v = marcheMondial[sym];
                open = v.open; high = v.high; low = v.low;
                close = v.close; variation = v.variation_24h;
            }
        }

        // 2. Volatilité : écart-type des prix sur les 24 dernières heures
        double volatilite = -1.0;
        sqlite3* db = openDatabase();
        if (db) {
            sqlite3_stmt* st;
            const char* sql = "SELECT prix FROM historique WHERE symbole = ? AND timestamp >= datetime('now', '-24 hours') ORDER BY timestamp ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, sym.c_str(), -1, SQLITE_STATIC);
                std::vector<double> prix;
                while (sqlite3_step(st) == SQLITE_ROW)
                    prix.push_back(sqlite3_column_double(st, 0));
                sqlite3_finalize(st);
                if (prix.size() >= 2) {
                    double mean = 0;
                    for (double p : prix) mean += p;
                    mean /= prix.size();
                    double variance = 0;
                    for (double p : prix) variance += (p - mean) * (p - mean);
                    volatilite = std::sqrt(variance / prix.size()) / mean * 100.0;
                }
            }
            sqlite3_close(db);
        }

        auto fmt = [](double v) -> json {
            return v != 0.0 ? json(v) : json(nullptr);
        };

        json j = {
            {"symbol",     sym},
            {"open",       fmt(open)},
            {"high",       fmt(high)},
            {"low",        fmt(low)},
            {"close",      fmt(close)},
            {"variation",  variation},
            {"volatilite", volatilite >= 0 ? json(volatilite) : json(nullptr)}
        };
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/sync-status", [](const httplib::Request&, httplib::Response& res) {
        json j = { {"last_update_ms", lastMarketUpdateMs.load()}, {"cycle_ms", 65000} };
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
        const std::string ip = req.remote_addr;

        if (isRateLimited(ip)) {
            res.status = 429;
            res.set_content("{\"erreur\":\"Trop de tentatives échouées. Réessayez dans 15 minutes.\"}", "application/json");
            return;
        }

        std::string user = req.get_param_value("user");
        std::string pass = req.get_param_value("pass");

        // 1. Vérification classique du mot de passe
        if (verifyLogin(user, pass)) {
            clearFailedAttempts(ip);
            
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

            // 4. Si tout est OK : créer un token de session sécurisé (7 jours)
            std::string token = generateToken();
            sqlite3_stmt* stok;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO user_sessions (token, username, expires_at) VALUES (?, ?, datetime('now', '+7 days'));",
                -1, &stok, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stok, 1, token.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stok, 2, user.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stok);
            }
            sqlite3_finalize(stok);

            res.set_header("Set-Cookie", "auth_session=" + token + "; Path=/; HttpOnly; SameSite=Strict");

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
            recordFailedAttempt(ip);
            res.status = 401;
            res.set_content("{\"erreur\":\"Identifiants incorrects\"}", "application/json");
        }
    });

    svr.Post("/api/logout", [](const httplib::Request& req, httplib::Response& res) {
        std::string token = getCookieValue(req, "auth_session");
        if (!token.empty()) {
            sqlite3* db = openDatabase();
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, "DELETE FROM user_sessions WHERE token = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
        res.set_header("Set-Cookie", "auth_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        res.set_content("{\"status\":\"deconnecte\"}", "application/json");
    });

    // GET /api/historique
    svr.Get("/api/historique", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("symbole")) { res.status = 400; return; }

        std::string symbole = req.get_param_value("symbole");
        std::string periode = req.has_param("periode") ? req.get_param_value("periode") : "24h";

        // Whitelist stricte sur la période — pas de concaténation dans le SQL
        if (periode != "1h" && periode != "3h" && periode != "24h" && periode != "7d" &&
            periode != "1m" && periode != "4m" && periode != "1y" && periode != "5y")
            periode = "24h";

        // Requêtes SQL avec bucketing temporel pour un axe X uniforme
        // Buckets calibrés pour ~160-180 points par fenêtre
        static const char* SQL_1H =   // 60 min  / 30sec  = 120 pts
            "SELECT AVG(prix),"
            " strftime('%H:%M:', timestamp) || printf('%02d', (CAST(strftime('%S', timestamp) AS INTEGER) / 30) * 30),"
            " strftime('%Y-%m-%d %H:%M:', timestamp) || printf('%02d', (CAST(strftime('%S', timestamp) AS INTEGER) / 30) * 30)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-1 hour')"
            " GROUP BY strftime('%Y-%m-%d %H:%M', timestamp), (CAST(strftime('%S', timestamp) AS INTEGER) / 30)"
            " ORDER BY 3 ASC;";

        static const char* SQL_3H =   // 180 min / 1-min  = 180 pts
            "SELECT AVG(prix), strftime('%H:%M', timestamp),"
            " strftime('%Y-%m-%d %H:%M', timestamp) || ':00'"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-3 hours')"
            " GROUP BY strftime('%Y-%m-%d %H:%M', timestamp)"
            " ORDER BY 3 ASC;";

        static const char* SQL_24H =  // 24h    / 8-min   = 180 pts
            "SELECT AVG(prix),"
            " strftime('%H:', timestamp) || printf('%02d', (CAST(strftime('%M', timestamp) AS INTEGER) / 8) * 8),"
            " strftime('%Y-%m-%d %H:', timestamp) || printf('%02d', (CAST(strftime('%M', timestamp) AS INTEGER) / 8) * 8) || ':00'"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-24 hours')"
            " GROUP BY strftime('%Y-%m-%d %H', timestamp), (CAST(strftime('%M', timestamp) AS INTEGER) / 8)"
            " ORDER BY 3 ASC;";

        static const char* SQL_7D =   // 7j     / 1h      = 168 pts
            "SELECT AVG(prix), strftime('%d/%m %H:00', timestamp),"
            " strftime('%Y-%m-%d %H:00:00', timestamp)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-7 days')"
            " GROUP BY strftime('%Y-%m-%d %H', timestamp)"
            " ORDER BY 3 ASC;";

        static const char* SQL_1M =   // 30j    / 4h      = 180 pts
            "SELECT AVG(prix),"
            " strftime('%d/%m ', timestamp) || printf('%02d:00', (CAST(strftime('%H', timestamp) AS INTEGER) / 4) * 4),"
            " strftime('%Y-%m-%d ', timestamp) || printf('%02d:00:00', (CAST(strftime('%H', timestamp) AS INTEGER) / 4) * 4)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-1 month')"
            " GROUP BY strftime('%Y-%m-%d', timestamp), (CAST(strftime('%H', timestamp) AS INTEGER) / 4)"
            " ORDER BY 3 ASC;";

        static const char* SQL_4M =   // 120j   / 16h     = ~180 pts
            "SELECT AVG(prix),"
            " strftime('%d/%m ', timestamp) || printf('%02d:00', (CAST(strftime('%H', timestamp) AS INTEGER) / 16) * 16),"
            " strftime('%Y-%m-%d ', timestamp) || printf('%02d:00:00', (CAST(strftime('%H', timestamp) AS INTEGER) / 16) * 16)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-4 months')"
            " GROUP BY strftime('%Y-%m-%d', timestamp), (CAST(strftime('%H', timestamp) AS INTEGER) / 16)"
            " ORDER BY 3 ASC;";

        static const char* SQL_1Y =   // 365j   / 2j      = ~182 pts
            "SELECT AVG(prix), strftime('%d/%m', MIN(timestamp)),"
            " strftime('%Y', timestamp) || printf('-%03d', (CAST(strftime('%j', timestamp) AS INTEGER) / 2) * 2)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-1 year')"
            " GROUP BY strftime('%Y', timestamp), (CAST(strftime('%j', timestamp) AS INTEGER) / 2)"
            " ORDER BY 3 ASC;";

        static const char* SQL_5Y =   // 5*365j / 10j     = ~182 pts
            "SELECT AVG(prix), strftime('%d/%m', MIN(timestamp)),"
            " strftime('%Y', timestamp) || printf('-%03d', (CAST(strftime('%j', timestamp) AS INTEGER) / 10) * 10)"
            " FROM historique WHERE symbole = ?"
            " AND timestamp >= datetime('now', '-5 years')"
            " GROUP BY strftime('%Y', timestamp), (CAST(strftime('%j', timestamp) AS INTEGER) / 10)"
            " ORDER BY 3 ASC;";

        const char* sql;
        if      (periode == "1h")  sql = SQL_1H;
        else if (periode == "3h")  sql = SQL_3H;
        else if (periode == "7d")  sql = SQL_7D;
        else if (periode == "1m")  sql = SQL_1M;
        else if (periode == "4m")  sql = SQL_4M;
        else if (periode == "1y")  sql = SQL_1Y;
        else if (periode == "5y")  sql = SQL_5Y;
        else                       sql = SQL_24H;

        sqlite3* db = openDatabase();
        sqlite3_stmt* stmt;
        json j = json::array();

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, symbole.c_str(), -1, SQLITE_STATIC);
            
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
        double frais = valeurTrade * 0.002; // 0.5%
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
            // Update prix_moyen: (old_qty * old_prix_moyen + new_qty * prix) / (old_qty + new_qty)
            if (sqlite3_prepare_v2(db,
                "INSERT INTO portefeuilles (user_id, symbole, quantite, prix_moyen) VALUES (?, ?, ?, ?) "
                "ON CONFLICT(user_id, symbole) DO UPDATE SET "
                "prix_moyen = (quantite * COALESCE(prix_moyen,0) + excluded.quantite * excluded.prix_moyen) / (quantite + excluded.quantite), "
                "quantite = quantite + excluded.quantite;",
                -1, &creditActif, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(creditActif, 1, uid);
                sqlite3_bind_text(creditActif, 2, symbole.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_double(creditActif, 3, quantite);
                sqlite3_bind_double(creditActif, 4, prix);
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

        // Récupérer les données du portefeuille (hors USD)
        json actifs = json::array();
        double usd = 0.0;
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT symbole, quantite, COALESCE(prix_moyen,0) FROM portefeuilles WHERE user_id = ? AND quantite > 0;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string sym = (const char*)sqlite3_column_text(stmt, 0);
                double qte = sqlite3_column_double(stmt, 1);
                double px_moyen = sqlite3_column_double(stmt, 2);

                if (sym == "USD") {
                    usd = qte;
                    continue;
                }

                double prix = 0.0, valeur = 0.0, var24h = 0.0;
                {
                    std::lock_guard<std::mutex> lock(marcheMutex);
                    if (marcheMondial.count(sym)) {
                        prix = marcheMondial[sym].prix;
                        var24h = marcheMondial[sym].variation_24h;
                        valeur = qte * prix;
                    }
                }

                double pnl_alltime_pct = (px_moyen > 0) ? (prix - px_moyen) / px_moyen * 100.0 : 0.0;
                double pnl_alltime_usd = (px_moyen > 0) ? (prix - px_moyen) * qte : 0.0;

                char buf_var24h[32], buf_alltime[32], buf_alltime_usd[32];
                snprintf(buf_var24h, sizeof(buf_var24h), "%+.2f%%", var24h);
                snprintf(buf_alltime, sizeof(buf_alltime), "%+.2f%%", pnl_alltime_pct);
                snprintf(buf_alltime_usd, sizeof(buf_alltime_usd), "%+.2f", pnl_alltime_usd);

                actifs.push_back({
                    {"symbole", sym}, {"quantite", qte}, {"prix_unitaire", prix}, {"valeur", valeur},
                    {"variation_24h", std::string(buf_var24h)},
                    {"pnl_alltime_pct", std::string(buf_alltime)},
                    {"pnl_alltime_usd", std::string(buf_alltime_usd)}
                });
            }
        }
        sqlite3_finalize(stmt);

        // Calcul du PnL global
        double valeurTotale = calculerValeurPortefeuille(db, uid);
        double snap = 100000.0;
        sqlite3_stmt* ss;
        if (sqlite3_prepare_v2(db, "SELECT valeur FROM snapshots WHERE user_id = ?;", -1, &ss, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(ss, 1, uid);
            if (sqlite3_step(ss) == SQLITE_ROW) snap = sqlite3_column_double(ss, 0);
        }
        sqlite3_finalize(ss);

        double pnl24hPct = (valeurTotale - snap) / snap * 100.0;
        double pnl24hUsd = valeurTotale - snap;
        double pnlTotalPct = (valeurTotale - 100000.0) / 100000.0 * 100.0;
        double pnlTotalUsd = valeurTotale - 100000.0;

        char buf24p[32], buf24u[32], bufTp[32], bufTu[32];
        snprintf(buf24p, sizeof(buf24p), "%+.2f%%", pnl24hPct);
        snprintf(buf24u, sizeof(buf24u), "%+.2f", pnl24hUsd);
        snprintf(bufTp, sizeof(bufTp), "%+.2f%%", pnlTotalPct);
        snprintf(bufTu, sizeof(bufTu), "%+.2f", pnlTotalUsd);

        json j = {
            {"usd", usd},
            {"pnl_24h_pct", std::string(buf24p)}, {"pnl_24h_usd", std::string(buf24u)},
            {"pnl_total_pct", std::string(bufTp)}, {"pnl_total_usd", std::string(bufTu)},
            {"actifs", actifs}
        };
        sqlite3_close(db);
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/trades/historique — historique personnel des trades (100 derniers)
    svr.Get("/api/trades/historique", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }

        sqlite3* db = openDatabase();
        int uid = getUserId(db, user);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        json trades = json::array();
        sqlite3_stmt* stmt;
        const char* sql =
            "SELECT action, symbole, quantite, prix, valeur, "
            "strftime('%d/%m/%Y %H:%M', timestamp) "
            "FROM trades_log WHERE user_id = ? "
            "ORDER BY id DESC LIMIT 100;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string action  = (const char*)sqlite3_column_text(stmt, 0);
                std::string symbole = (const char*)sqlite3_column_text(stmt, 1);
                double quantite     = sqlite3_column_double(stmt, 2);
                double prix         = sqlite3_column_double(stmt, 3);
                double valeur       = sqlite3_column_double(stmt, 4);
                std::string ts      = (const char*)sqlite3_column_text(stmt, 5);
                trades.push_back({
                    {"action", action}, {"symbole", symbole},
                    {"quantite", quantite}, {"prix", prix},
                    {"valeur", valeur}, {"timestamp", ts}
                });
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        res.set_content(trades.dump(), "application/json");
    });

    // GET /api/portefeuille/historique
    svr.Get("/api/portefeuille/historique", [](const httplib::Request& req, httplib::Response& res) {
        std::string user = getCookieUser(req);
        if (user.empty()) { res.status = 401; return; }

        sqlite3* db = openDatabase();
        int uid = getUserId(db, user);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        // Jour actuel : PnL 24h en cours
        double valeurActuelle = calculerValeurPortefeuille(db, uid);
        double snapActuel = 100000.0;
        sqlite3_stmt* ss;
        if (sqlite3_prepare_v2(db, "SELECT valeur FROM snapshots WHERE user_id = ?;", -1, &ss, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(ss, 1, uid);
            if (sqlite3_step(ss) == SQLITE_ROW) snapActuel = sqlite3_column_double(ss, 0);
        }
        sqlite3_finalize(ss);

        double pnlAujourdhui = snapActuel > 0 ? (valeurActuelle - snapActuel) / snapActuel * 100.0 : 0.0;

        // Rang actuel
        int rangActuel = 1;
        sqlite3_stmt* sr;
        if (sqlite3_prepare_v2(db, "SELECT id FROM utilisateurs;", -1, &sr, nullptr) == SQLITE_OK) {
            while (sqlite3_step(sr) == SQLITE_ROW) {
                int other = sqlite3_column_int(sr, 0);
                if (other != uid && calculerValeurPortefeuille(db, other) > valeurActuelle) rangActuel++;
            }
        }
        sqlite3_finalize(sr);

        time_t now = time(nullptr);
        char todayStr[16];
        strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", gmtime(&now));

        json hist = json::array();
        char bufPnl[32];
        snprintf(bufPnl, sizeof(bufPnl), "%+.2f%%", pnlAujourdhui);
        hist.push_back({{"date", std::string(todayStr)}, {"pnl_day_pct", std::string(bufPnl)}, {"rank", rangActuel}, {"today", true}});

        // Historique passé
        sqlite3_stmt* sh;
        if (sqlite3_prepare_v2(db, "SELECT date, pnl_day_pct, rank FROM daily_history WHERE user_id = ? ORDER BY date DESC;", -1, &sh, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sh, 1, uid);
            while (sqlite3_step(sh) == SQLITE_ROW) {
                std::string date = (const char*)sqlite3_column_text(sh, 0);
                double pnl = sqlite3_column_double(sh, 1);
                int rank = sqlite3_column_int(sh, 2);
                char buf[32];
                snprintf(buf, sizeof(buf), "%+.2f%%", pnl);
                hist.push_back({{"date", date}, {"pnl_day_pct", std::string(buf)}, {"rank", rank}, {"today", false}});
            }
        }
        sqlite3_finalize(sh);

        sqlite3_close(db);
        res.set_content(hist.dump(), "application/json");
    });

    // GET /api/portefeuille/joueur?username=X  (vue publique du portefeuille d'un joueur)
    svr.Get("/api/portefeuille/joueur", [](const httplib::Request& req, httplib::Response& res) {
        std::string viewer = getCookieUser(req);
        if (viewer.empty()) { res.status = 401; return; }

        std::string target = req.get_param_value("username");
        if (target.empty()) { res.status = 400; return; }

        sqlite3* db = openDatabase();
        int uid = getUserId(db, target);
        if (uid == -1) { sqlite3_close(db); res.status = 404; return; }

        json actifs = json::array();
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT symbole, quantite, COALESCE(prix_moyen,0) FROM portefeuilles WHERE user_id = ? AND quantite > 0 AND symbole != 'USD';", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string sym = (const char*)sqlite3_column_text(stmt, 0);
                double qte = sqlite3_column_double(stmt, 1);
                double px_moyen = sqlite3_column_double(stmt, 2);
                double prix = 0.0, valeur = 0.0, var24h = 0.0;
                {
                    std::lock_guard<std::mutex> lock(marcheMutex);
                    if (marcheMondial.count(sym)) {
                        prix = marcheMondial[sym].prix;
                        var24h = marcheMondial[sym].variation_24h;
                        valeur = qte * prix;
                    }
                }
                double pnl_alltime_pct = (px_moyen > 0) ? (prix - px_moyen) / px_moyen * 100.0 : 0.0;
                double pnl_alltime_usd = (px_moyen > 0) ? (prix - px_moyen) * qte : 0.0;
                char buf_var24h[32], buf_alltime[32], buf_alltime_usd[32];
                snprintf(buf_var24h, sizeof(buf_var24h), "%+.2f%%", var24h);
                snprintf(buf_alltime, sizeof(buf_alltime), "%+.2f%%", pnl_alltime_pct);
                snprintf(buf_alltime_usd, sizeof(buf_alltime_usd), "%+.2f", pnl_alltime_usd);
                actifs.push_back({
                    {"symbole", sym}, {"quantite", qte}, {"prix_unitaire", prix}, {"valeur", valeur},
                    {"variation_24h", std::string(buf_var24h)},
                    {"pnl_alltime_pct", std::string(buf_alltime)},
                    {"pnl_alltime_usd", std::string(buf_alltime_usd)}
                });
            }
        }
        sqlite3_finalize(stmt);

        // Liquidités
        double usd = 0.0;
        sqlite3_stmt* sUsd;
        if (sqlite3_prepare_v2(db, "SELECT quantite FROM portefeuilles WHERE user_id = ? AND symbole = 'USD';", -1, &sUsd, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sUsd, 1, uid);
            if (sqlite3_step(sUsd) == SQLITE_ROW) usd = sqlite3_column_double(sUsd, 0);
        }
        sqlite3_finalize(sUsd);

        // PnL total
        double valeurTotale = calculerValeurPortefeuille(db, uid);
        double pnlTotalPct = (valeurTotale - 100000.0) / 100000.0 * 100.0;
        double pnlTotalUsd = valeurTotale - 100000.0;
        char bufTp[32], bufTu[32];
        snprintf(bufTp, sizeof(bufTp), "%+.2f%%", pnlTotalPct);
        snprintf(bufTu, sizeof(bufTu), "%+.2f", pnlTotalUsd);

        // Historique journalier
        double snapActuel = 100000.0;
        sqlite3_stmt* ss;
        if (sqlite3_prepare_v2(db, "SELECT valeur FROM snapshots WHERE user_id = ?;", -1, &ss, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(ss, 1, uid);
            if (sqlite3_step(ss) == SQLITE_ROW) snapActuel = sqlite3_column_double(ss, 0);
        }
        sqlite3_finalize(ss);

        double pnlAujourdhui = snapActuel > 0 ? (valeurTotale - snapActuel) / snapActuel * 100.0 : 0.0;

        // Rang actuel
        int rangActuel = 1;
        sqlite3_stmt* sr;
        if (sqlite3_prepare_v2(db, "SELECT id FROM utilisateurs;", -1, &sr, nullptr) == SQLITE_OK) {
            while (sqlite3_step(sr) == SQLITE_ROW) {
                int other = sqlite3_column_int(sr, 0);
                if (other != uid && calculerValeurPortefeuille(db, other) > valeurTotale) rangActuel++;
            }
        }
        sqlite3_finalize(sr);

        time_t now = time(nullptr);
        char todayStr[16];
        strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", gmtime(&now));

        json hist = json::array();
        char bufPnlJ[32];
        snprintf(bufPnlJ, sizeof(bufPnlJ), "%+.2f%%", pnlAujourdhui);
        hist.push_back({{"date", std::string(todayStr)}, {"pnl_day_pct", std::string(bufPnlJ)}, {"rank", rangActuel}, {"today", true}});

        sqlite3_stmt* sh;
        if (sqlite3_prepare_v2(db, "SELECT date, pnl_day_pct, rank FROM daily_history WHERE user_id = ? ORDER BY date DESC;", -1, &sh, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(sh, 1, uid);
            while (sqlite3_step(sh) == SQLITE_ROW) {
                std::string date = (const char*)sqlite3_column_text(sh, 0);
                double pnl = sqlite3_column_double(sh, 1);
                int rank = sqlite3_column_int(sh, 2);
                char buf[32];
                snprintf(buf, sizeof(buf), "%+.2f%%", pnl);
                hist.push_back({{"date", date}, {"pnl_day_pct", std::string(buf)}, {"rank", rank}, {"today", false}});
            }
        }
        sqlite3_finalize(sh);

        sqlite3_close(db);
        res.set_content(json({
            {"username", target}, {"actifs", actifs},
            {"usd", usd},
            {"pnl_total_pct", std::string(bufTp)}, {"pnl_total_usd", std::string(bufTu)},
            {"historique", hist}
        }).dump(), "application/json");
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
        std::string token = getCookieValue(req, ADMIN_COOKIE);
        if (token.empty()) return false;
        sqlite3* db = openDatabase();
        bool ok = false;
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM admin_sessions WHERE token = ? AND expires_at > datetime('now');",
            -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
            ok = (sqlite3_step(stmt) == SQLITE_ROW);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return ok;
    };
    
    // ── Espace Admin ──────────────────────────────────────────────────────────
    
    svr.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("htdocs/admin.html", std::ios::binary);
        if (!f) { res.status = 404; return; }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        res.set_content(content, "text/html");
    });
    
    svr.Post("/api/admin/login", [](const httplib::Request& req, httplib::Response& res) {
        const std::string ip = req.remote_addr;

        if (isRateLimited(ip)) {
            res.status = 429;
            res.set_content("{\"erreur\":\"Trop de tentatives. Réessayez dans 15 minutes.\"}", "application/json");
            return;
        }

        std::string u = req.get_param_value("user");
        std::string p = req.get_param_value("pass");
        if (u == ADMIN_USER && p == ADMIN_PASS) {
            clearFailedAttempts(ip);
            std::string token = generateToken();
            sqlite3* db = openDatabase();
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO admin_sessions (token, expires_at) VALUES (?, datetime('now', '+8 hours'));",
                -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            res.set_header("Set-Cookie", std::string(ADMIN_COOKIE) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            recordFailedAttempt(ip);
            res.status = 401;
            res.set_content("{\"erreur\":\"Accès refusé\"}", "application/json");
        }
    });
    
    svr.Get("/api/admin/check", [&isAdmin](const httplib::Request& req, httplib::Response& res) {
        if (isAdmin(req)) res.set_content("{\"ok\":true}", "application/json");
        else { res.status = 401; res.set_content("{\"ok\":false}", "application/json"); }
    });
    
    svr.Post("/api/admin/logout", [](const httplib::Request& req, httplib::Response& res) {
        std::string token = getCookieValue(req, ADMIN_COOKIE);
        if (!token.empty()) {
            sqlite3* db = openDatabase();
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, "DELETE FROM admin_sessions WHERE token = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
            sqlite3_close(db);
        }
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

        sqlite3_stmt* s1;
        if (sqlite3_prepare_v2(db, "UPDATE utilisateurs SET statut = 'approved' WHERE id = ?;",
                               -1, &s1, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s1, 1, uid);
            sqlite3_step(s1);
        }
        sqlite3_finalize(s1);

        sqlite3_stmt* s2;
        if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO portefeuilles (user_id, symbole, quantite) VALUES (?, 'USD', 100000.0);",
                               -1, &s2, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s2, 1, uid);
            sqlite3_step(s2);
        }
        sqlite3_finalize(s2);

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