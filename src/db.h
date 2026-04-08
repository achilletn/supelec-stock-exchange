#pragma once
#include <sqlite3.h>
#include <string>

sqlite3* dbOpen();

int         dbGetUserId(sqlite3* db, const std::string& username);
bool        dbVerifyLogin(const std::string& username, const std::string& password);
bool        dbRegisterUser(const std::string& username, const std::string& password, const std::string& phone);
double      dbGetPortfolioValue(sqlite3* db, int userId);