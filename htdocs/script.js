// ── State ────────────────────────────────────────────────────────────────────
let currentUser       = "";
let masterTickInterval = null;
let leaderboardCache  = {};
let activeChart       = null;
let currentSymbol     = "";
let currentPeriod     = "24h";
let lastTickTime      = performance.now();
let tickCount         = 0;

const tickHandlers    = new Set();
const syncClockHandles = {};
const chartCache      = {};
const CHART_REFRESH_EVERY = 3; // refresh graph every N ticks

const currencyFormatter = new Intl.NumberFormat('fr-FR', {
    style: 'currency',
    currency: 'USD',
    currencyDisplay: 'narrowSymbol',
    minimumFractionDigits: 2,
    maximumFractionDigits: 2
});

// ── Master tick ───────────────────────────────────────────────────────────────

function startMasterTick() {
    if (masterTickInterval) return;
    masterTickInterval = setInterval(() => {
        lastTickTime = performance.now();
        tickCount++;
        tickHandlers.forEach(fn => fn(tickCount));
    }, 5000);
}

function stopMasterTick() {
    clearInterval(masterTickInterval);
    masterTickInterval = null;
}

function tickChart(tick) {
    if (currentSymbol && tick % CHART_REFRESH_EVERY === 0)
        loadChart(currentSymbol, currentPeriod);
}

// ── Session ───────────────────────────────────────────────────────────────────

function checkSession() {
    const loader = document.getElementById("loading");
    fetch("/api/check-session")
        .then(res => {
            if (loader) loader.style.display = "none";
            if (res.ok) return res.json();
            throw new Error();
        })
        .then(data => {
            currentUser = data.user;
            startGame();
        })
        .catch(() => {
            if (loader) loader.style.display = "none";
            document.getElementById("login-zone").style.display = "block";
        });
}

function login() {
    const params = new URLSearchParams();
    params.append("user", document.getElementById("user").value);
    params.append("pass", document.getElementById("pass").value);

    fetch("/api/login", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.error || "Invalid credentials");
            return data;
        })
        .then(() => fetch("/api/check-session").then(r => r.json()))
        .then(data => {
            currentUser = data.user;
            startGame();
        })
        .catch(err => {
            document.getElementById("login-error").innerText = err.message;
        });
}

function register() {
    const params = new URLSearchParams();
    params.append("user", document.getElementById("reg-user").value);
    params.append("pass", document.getElementById("reg-pass").value);
    params.append("tel",  document.getElementById("reg-phone").value);
    const msg = document.getElementById("reg-message");

    fetch("/api/register", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.error || "Username or phone already taken.");
            msg.style.color = "orange";
            msg.innerText = "Account created! Waiting for admin approval ⏳";
            document.getElementById("reg-user").value  = "";
            document.getElementById("reg-pass").value  = "";
            document.getElementById("reg-phone").value = "";
        })
        .catch(err => {
            msg.style.color = "red";
            msg.innerText = err.message;
        });
}

function logout() {
    fetch("/api/logout", { method: "POST" }).then(() => {
        localStorage.removeItem("lastTab");
        tickHandlers.clear();
        stopMasterTick();
        location.reload();
    });
}

// ── Game bootstrap ────────────────────────────────────────────────────────────

function startGame() {
    document.getElementById("login-zone").style.display = "none";
    document.getElementById("game-zone").style.display  = "block";

    tickHandlers.add(refreshDashboard);
    refreshDashboard();
    startMasterTick();

    switchTab(localStorage.getItem("lastTab") || "market");
}

// ── Tab routing ───────────────────────────────────────────────────────────────

function switchTab(tabName) {
    localStorage.setItem("lastTab", tabName);
    document.querySelectorAll(".tab-content").forEach(t => (t.style.display = "none"));

    const target = document.getElementById(`tab-${tabName}`);
    if (target) target.style.display = "block";

    const isAbout = tabName === "about";
    document.querySelector(".user-dashboard-header").style.display = isAbout ? "none" : "flex";

    stopMarketLoop();
    stopLeaderboardLoop();
    stopPortfolioLoop();

    if      (tabName === "market")      startMarketLoop();
    else if (tabName === "leaderboard") startLeaderboardLoop();
    else if (tabName === "portfolio")   startPortfolioLoop();
}

// ── Market tab ────────────────────────────────────────────────────────────────

function startMarketLoop() {
    tickHandlers.add(refreshMarketTable);
    tickHandlers.add(tickChart);
    refreshMarketTable();
    startSyncClock("sync-clock");
}

function stopMarketLoop() {
    tickHandlers.delete(refreshMarketTable);
    tickHandlers.delete(tickChart);
    cancelSyncClock("sync-clock");
}

function refreshMarketTable() {
    const tbody = document.getElementById("market-body");
    if (!tbody) return;

    fetch("/api/market")
        .then(res => res.json())
        .then(data => {
            if (data.length === 0) {
                tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;padding:20px;color:#f39c12;">Syncing... 📡</td></tr>`;
                return;
            }
            tbody.innerHTML = "";
            data.forEach(asset => {
                const tr         = document.createElement("tr");
                const changeColor = asset.change24h >= 0 ? "#2ecc71" : "#e74c3c";
                const changeSign  = asset.change24h >= 0 ? "+" : "";
                tr.innerHTML = `
                    <td><strong>${asset.symbol}</strong></td>
                    <td class="price">${asset.price.toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} $</td>
                    <td style="font-family:'Courier New',monospace; font-weight:bold; color:${changeColor};">
                        ${changeSign}${asset.change24h.toFixed(2)}%
                    </td>
                    <td><button class="btn-detail" onclick="openDetail('${asset.symbol}')">Details</button></td>`;
                tbody.appendChild(tr);

                // Live-update the price in the detail panel if it's open
                if (asset.symbol === currentSymbol) {
                    const priceEl = document.getElementById("detail-price");
                    if (priceEl)
                        priceEl.innerText = asset.price.toLocaleString("fr-FR", {
                            minimumFractionDigits: 2, maximumFractionDigits: 2
                        }) + " $";
                }
            });
        })
        .catch(() => {
            tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;color:red;">Network error ❌</td></tr>`;
        });
}

// ── Leaderboard tab ───────────────────────────────────────────────────────────

function startLeaderboardLoop() {
    tickHandlers.add(loadLeaderboard);
    loadLeaderboard();
    startSyncClock("sync-clock-leaderboard");
}

function stopLeaderboardLoop() {
    tickHandlers.delete(loadLeaderboard);
    cancelSyncClock("sync-clock-leaderboard");
}

function loadLeaderboard() {
    fetch("/api/leaderboard")
        .then(res => res.json())
        .then(data => {
            const me = data.find(u => u.username === currentUser);
            if (me) applyDashboardData(me);

            const tbody = document.getElementById("leaderboard-body");
            if (!tbody) return;
            tbody.innerHTML = "";

            data.forEach((player, index) => {
                const tr     = document.createElement("tr");
                const medal  = index === 0 ? "🥇" : index === 1 ? "🥈" : index === 2 ? "🥉" : index + 1;

                const cached = leaderboardCache[player.username];
                if (cached !== undefined) {
                    if      (player.portfolio_value > cached) tr.className = "gain-update";
                    else if (player.portfolio_value < cached) tr.className = "loss-update";
                }
                leaderboardCache[player.username] = player.portfolio_value;

                const colorTotal = player.pnl.startsWith("+") && player.pnl !== "+0.00%"
                    ? "#2ecc71" : player.pnl.startsWith("-") ? "#e74c3c" : "gray";
                const color24h = player.pnl_24h_pct?.startsWith("+") && player.pnl_24h_pct !== "+0.00%"
                    ? "#2ecc71" : player.pnl_24h_pct?.startsWith("-") ? "#e74c3c" : "gray";

                tr.innerHTML = `
                    <td>${medal}</td>
                    <td><strong>${player.username}</strong></td>
                    <td style="font-weight:bold;">${player.portfolio_value.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} $</td>
                    <td style="color:${color24h};font-weight:bold;">
                        ${player.pnl_24h_usd ?? "—"} $<br><span style="font-size:0.85em;">${player.pnl_24h_pct ?? "—"}</span>
                    </td>
                    <td style="color:${colorTotal};font-weight:bold;">
                        ${player.pnl_total_usd ?? "—"} $<br><span style="font-size:0.85em;">${player.pnl}</span>
                    </td>`;
                tbody.appendChild(tr);
            });
        });
}

// ── Dashboard header ──────────────────────────────────────────────────────────

function applyDashboardData(data) {
    document.getElementById("dash-username").innerText = data.username.toUpperCase();
    document.getElementById("dash-wallet").innerText   = currencyFormatter.format(data.portfolio_value);

    const pnlEl = document.getElementById("dash-pnl");
    pnlEl.innerText  = `${data.pnl} (${data.pnl_total_usd ?? "—"} $)`;
    pnlEl.style.color = data.pnl.startsWith("+") && data.pnl !== "+0.00%"
        ? "#2ecc71" : data.pnl.startsWith("-") ? "#e74c3c" : "#888";

    const pnl24El = document.getElementById("dash-pnl24h");
    if (pnl24El && data.pnl_24h_pct !== undefined) {
        const isPos = data.pnl_24h_pct.startsWith("+") && data.pnl_24h_pct !== "+0.00%";
        pnl24El.innerText  = `${data.pnl_24h_pct} (${data.pnl_24h_usd} $)`;
        pnl24El.style.color = isPos ? "#2ecc71" : data.pnl_24h_pct.startsWith("-") ? "#e74c3c" : "#888";
    }

    const usdEl = document.getElementById("dash-usd");
    if (usdEl && data.usd !== undefined)
        usdEl.innerText = currencyFormatter.format(data.usd);
}

function refreshDashboard() {
    if (!currentUser) return;
    fetch("/api/dashboard")
        .then(res => res.json())
        .then(data => applyDashboardData(data));
}

// Clock in dashboard header
setInterval(() => {
    document.getElementById("dash-clock").innerText = new Date().toLocaleTimeString();
}, 1000);

// ── Portfolio tab ─────────────────────────────────────────────────────────────

function startPortfolioLoop() {
    tickHandlers.add(loadPortfolio);
    loadPortfolio();
    startSyncClock("sync-clock-portfolio");
}

function stopPortfolioLoop() {
    tickHandlers.delete(loadPortfolio);
    cancelSyncClock("sync-clock-portfolio");
}

function loadPortfolio() {
    fetch("/api/portfolio")
        .then(res => res.json())
        .then(data => {
            const tbody = document.getElementById("portfolio-body");
            if (!tbody) return;
            tbody.innerHTML = "";
            data.forEach(row => {
                const tr = document.createElement("tr");
                tr.innerHTML = `
                    <td><strong>${row.symbol}</strong></td>
                    <td style="font-family:'Courier New',monospace;">${row.quantity.toLocaleString("en-US", { maximumFractionDigits: 6 })}</td>
                    <td style="font-family:'Courier New',monospace;">${row.unit_price > 0 ? row.unit_price.toLocaleString("en-US", { minimumFractionDigits: 2 }) + " $" : "—"}</td>
                    <td style="font-family:'Courier New',monospace; font-weight:bold;">${currencyFormatter.format(row.value)}</td>`;
                tbody.appendChild(tr);
            });
        });
}

// ── Sync clock (canvas ring) ──────────────────────────────────────────────────

function startSyncClock(canvasId) {
    cancelSyncClock(canvasId);
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx  = canvas.getContext("2d");
    const TICK = 5000;
    let resetting = false;

    function draw(now) {
        const progress = ((now - lastTickTime) % TICK) / TICK;
        ctx.clearRect(0, 0, 28, 28);

        const cx = 14, cy = 14, r = 10;
        const startAngle = -Math.PI / 2;

        // Background ring
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = "rgba(128,128,128,0.2)";
        ctx.lineWidth   = 2.5;
        ctx.stroke();

        if (resetting) {
            ctx.strokeStyle = "#2ecc71";
            ctx.stroke();
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(9, 14); ctx.lineTo(12.5, 17.5); ctx.lineTo(19, 10);
            ctx.stroke();
        } else if (progress <= 0.98) {
            const endAngle = startAngle + progress * 2 * Math.PI;
            const grad = ctx.createLinearGradient(cx - r, cy, cx + r, cy);
            grad.addColorStop(0, "#1877f2");
            grad.addColorStop(1, "#2ecc71");
            ctx.beginPath();
            ctx.arc(cx, cy, r, startAngle, endAngle);
            ctx.strokeStyle = grad;
            ctx.stroke();
            // Leading dot
            ctx.beginPath();
            ctx.arc(cx + r * Math.cos(endAngle), cy + r * Math.sin(endAngle), 2.5, 0, Math.PI * 2);
            ctx.fillStyle = "#2ecc71";
            ctx.fill();
        } else {
            resetting = true;
            setTimeout(() => { resetting = false; }, 400);
        }
        syncClockHandles[canvasId] = requestAnimationFrame(draw);
    }
    syncClockHandles[canvasId] = requestAnimationFrame(draw);
}

function cancelSyncClock(canvasId) {
    if (syncClockHandles[canvasId]) {
        cancelAnimationFrame(syncClockHandles[canvasId]);
        delete syncClockHandles[canvasId];
    }
}

// ── Detail panel ──────────────────────────────────────────────────────────────

function openDetail(symbol) {
    currentSymbol = symbol;
    document.getElementById("detail-title").innerText       = symbol;
    document.getElementById("trade-message").innerText      = "";
    document.getElementById("detail-panel").style.display   = "block";
    loadChart(currentSymbol, currentPeriod);
}

function closeDetail() {
    document.getElementById("detail-panel").style.display = "none";
    if (activeChart) { activeChart.destroy(); activeChart = null; }
    currentSymbol = "";
}

function changePeriod(period) {
    currentPeriod = period;
    document.querySelectorAll(".btn-time").forEach(b => b.classList.remove("active"));
    document.getElementById(`btn-${period}`).classList.add("active");
    loadChart(currentSymbol, currentPeriod);
}

function getCacheKey(symbol, period) { return `${symbol}_${period}`; }

function loadChart(symbol, period) {
    const key   = getCacheKey(symbol, period);
    const cache = chartCache[key];
    const url   = cache
        ? `/api/history?symbol=${symbol}&period=${period}&since=${encodeURIComponent(cache.lastTimestamp)}`
        : `/api/history?symbol=${symbol}&period=${period}`;

    fetch(url)
        .then(res => res.json())
        .then(data => {
            if (!cache) {
                chartCache[key] = {
                    labels:        data.map(d => d.time),
                    prices:        data.map(d => d.price),
                    lastTimestamp: data.length ? data[data.length - 1].timestamp : ""
                };
            } else if (data.length > 0) {
                cache.labels.push(...data.map(d => d.time));
                cache.prices.push(...data.map(d => d.price));
                cache.lastTimestamp = data[data.length - 1].timestamp;
            }

            const c = chartCache[key];
            if (!c.prices.length) return;

            document.getElementById("detail-price").innerText =
                c.prices[c.prices.length - 1].toLocaleString("fr-FR", { minimumFractionDigits: 2 }) + " $";
            drawChart(c.labels, c.prices);
        })
        .catch(err => console.error("Chart load error:", err));
}

function drawChart(labels, prices) {
    const ctx = document.getElementById("chart-canvas").getContext("2d");
    if (activeChart) activeChart.destroy();

    activeChart = new Chart(ctx, {
        type: "line",
        data: {
            labels,
            datasets: [{
                label: "Price",
                data: prices,
                borderColor: "#1877f2",
                backgroundColor: "rgba(24,119,242,0.1)",
                borderWidth: 2,
                pointRadius: 0,
                pointHoverRadius: 6,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: "index", intersect: false },
            plugins: { legend: { display: false } },
            scales: {
                x: { grid: { display: false } },
                y: { grace: "5%", grid: { color: "#f0f0f0" } }
            }
        }
    });
}

// ── Trading ───────────────────────────────────────────────────────────────────

function placeOrder(action) {
    const quantity = document.getElementById("trade-quantity").value;
    const msgBox   = document.getElementById("trade-message");

    if (!quantity || quantity <= 0) {
        msgBox.innerText   = "Please enter a valid quantity.";
        msgBox.style.color = "red";
        return;
    }

    msgBox.innerText   = "Processing... ⏳";
    msgBox.style.color = "orange";

    const params = new URLSearchParams();
    params.append("symbol",   currentSymbol);
    params.append("action",   action);
    params.append("quantity", quantity);

    fetch("/api/trade", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.error || "Transaction failed");
            msgBox.innerText   = `${data.message} ✅`;
            msgBox.style.color = "green";
            document.getElementById("trade-quantity").value = "";
            refreshDashboard();
        })
        .catch(err => {
            msgBox.innerText   = `${err.message} ❌`;
            msgBox.style.color = "red";
        });
}

// ── Keyboard helpers ──────────────────────────────────────────────────────────

function bindEnterKey(currentId, nextId) {
    document.getElementById(currentId).addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            document.getElementById(nextId).focus();
        }
    });
}

bindEnterKey("user",     "pass");
bindEnterKey("reg-user", "reg-pass");

// ── Boot ──────────────────────────────────────────────────────────────────────
checkSession();