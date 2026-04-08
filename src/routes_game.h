#pragma once
#include "httplib.h"

// Registers player-facing API routes:
//   GET  /api/market
//   GET  /api/dashboard
//   GET  /api/leaderboard
//   GET  /api/history
//   GET  /api/portfolio
//   POST /api/trade
//   POST /api/contact
void registerGameRoutes(httplib::Server& svr);