#pragma once
#include "httplib.h"

// Registers all admin API routes under /api/admin/*
// and serves GET /admin -> htdocs/admin/index.html
void registerAdminRoutes(httplib::Server& svr);