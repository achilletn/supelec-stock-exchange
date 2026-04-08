#pragma once
#include "httplib.h"
#include <string>

// Extracts the authenticated username from the session cookie, or returns "".
std::string getSessionUser(const httplib::Request& req);

// Returns true if the request carries a valid admin session cookie.
bool isAdminSession(const httplib::Request& req);

// Registers the auth routes: POST /api/login, POST /api/logout, GET /api/check-session
// and POST /api/register.
void registerAuthRoutes(httplib::Server& svr);