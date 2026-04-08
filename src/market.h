#pragma once
#include <string>
#include <map>
#include <mutex>
#include <atomic>

struct Asset {
    std::string name;
    double price       = 0.0;
    double change24h   = 0.0;  // percent
};

// Global market state — mutex-protected, updated by the price worker thread.
extern std::map<std::string, Asset> g_market;
extern std::mutex                   g_marketMutex;
extern std::atomic<bool>            g_gameRunning;

// Starts the background threads (price refresh, snapshots, history).
void startWorkers();