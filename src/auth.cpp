#include "auth.h"
#include "db.h"
#include <sqlite3.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// TODO SECURITY: Use environment variables instead of hardcoded credentials.
static const std::string ADMIN_USERNAME    = "goudale";
static const std::string ADMIN_PASSWORD    = "forceaumecdubush";
static const std::string ADMIN_COOKIE_NAME = "admin_session";

// ── Session helpers ────────────────────────────────────────────────────────

std::string getSessionUser(const httplib::Request& req) {
    if (!req.has_header("Cookie")) return "";
    const std::string cookies = req.get_header_value("Cookie");
    const size_t pos = cookies.find("auth_user=");
    if (pos == std::string::npos) return "";
    const size_t start = pos + 10;
    const size_t end   = cookies.find(';', start);
    return cookies.substr(start, end - start);
}

bool isAdminSession(const httplib::Request& req) {
    if (!req.has_header("Cookie")) return false;
    const std::string cookies = req.get_header_value("Cookie");
    return cookies.find(ADMIN_COOKIE_NAME + "=1") != std::string::npos;
}

// ── Auth routes ────────────────────────────────────────────────────────────

void registerAuthRoutes(httplib::Server& svr) {

    svr.Get("/api/check-session", [](const httplib::Request& req, httplib::Response& res) {
        const std::string user = getSessionUser(req);
        if (!user.empty())
            res.set_content("{\"status\":\"connected\",\"user\":\"" + user + "\"}", "application/json");
        else {
            res.status = 401;
            res.set_content("{\"error\":\"not authenticated\"}", "application/json");
        }
    });

    svr.Post("/api/login", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = req.get_param_value("user");
        const std::string password = req.get_param_value("pass");

        if (!dbVerifyLogin(username, password)) {
            res.status = 401;
            res.set_content("{\"error\":\"Invalid credentials\"}", "application/json");
            return;
        }

        // Check approval status
        sqlite3* db = dbOpen();
        std::string status;
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT status FROM users WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                status = (const char*)sqlite3_column_text(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (status == "pending") {
            sqlite3_close(db);
            res.status = 403;
            res.set_content("{\"error\":\"Your account is pending approval by an administrator.\"}", "application/json");
            return;
        }

        // Set session cookie and log the login event
        res.set_header("Set-Cookie", "auth_user=" + username + "; Path=/; HttpOnly; SameSite=Strict");

        sqlite3_stmt* log;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO sessions_log (username, ip, user_agent, action) VALUES (?, ?, ?, 'login');",
            -1, &log, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(log, 1, username.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(log, 2, req.remote_addr.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(log, 3,
                req.has_header("User-Agent") ? req.get_header_value("User-Agent").c_str() : "",
                -1, SQLITE_STATIC);
            sqlite3_step(log);
        }
        sqlite3_finalize(log);
        sqlite3_close(db);

        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    svr.Post("/api/logout", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", "auth_user=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        res.set_content("{\"status\":\"logged_out\"}", "application/json");
    });

    svr.Post("/api/register", [](const httplib::Request& req, httplib::Response& res) {
        const std::string username = req.get_param_value("user");
        const std::string password = req.get_param_value("pass");
        const std::string phone    = req.get_param_value("tel");

        if (username.size() < 3 || password.size() < 4 || phone.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Incomplete or too-short registration data\"}", "application/json");
            return;
        }

        if (dbRegisterUser(username, password, phone)) {
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            res.status = 409;
            res.set_content("{\"error\":\"Username or phone number already taken\"}", "application/json");
        }
    });

    // ── Admin auth ─────────────────────────────────────────────────────────

    svr.Post("/api/admin/login", [](const httplib::Request& req, httplib::Response& res) {
        if (req.get_param_value("user") == ADMIN_USERNAME &&
            req.get_param_value("pass") == ADMIN_PASSWORD) {
            res.set_header("Set-Cookie", ADMIN_COOKIE_NAME + "=1; Path=/; HttpOnly; SameSite=Strict");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            res.status = 401;
            res.set_content("{\"error\":\"Access denied\"}", "application/json");
        }
    });

    svr.Post("/api/admin/logout", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Set-Cookie", ADMIN_COOKIE_NAME + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    svr.Get("/api/admin/check", [](const httplib::Request& req, httplib::Response& res) {
        if (isAdminSession(req))
            res.set_content("{\"ok\":true}", "application/json");
        else {
            res.status = 401;
            res.set_content("{\"ok\":false}", "application/json");
        }
    });
}