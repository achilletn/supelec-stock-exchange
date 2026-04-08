// ── Config ────────────────────────────────────────────────────────────────────
let gameRunning = true;

// In-memory data caches
let allUsers    = [];
let allTrades   = [];
let allSessions = [];

// Pagination
const PAGE_SIZE  = 30;
let tradePage    = 1;
let sessionPage  = 1;

// ── Auth ──────────────────────────────────────────────────────────────────────

function doLogin() {
    const params = new URLSearchParams();
    params.append('user', document.getElementById('adm-user').value);
    params.append('pass', document.getElementById('adm-pass').value);

    fetch('/api/admin/login', { method: 'POST', body: params })
        .then(r => { if (!r.ok) throw new Error(); return r.json(); })
        .then(() => {
            document.getElementById('login-screen').style.display = 'none';
            document.getElementById('admin-screen').style.display = 'block';
            initAdmin();
        })
        .catch(() => {
            document.getElementById('login-err').textContent = '// ACCESS DENIED — Invalid credentials';
        });
}

function doLogout() {
    fetch('/api/admin/logout', { method: 'POST' }).then(() => location.reload());
}

document.getElementById('adm-pass').addEventListener('keydown', e => {
    if (e.key === 'Enter') doLogin();
});
document.getElementById('adm-user').addEventListener('keydown', e => {
    if (e.key === 'Enter') document.getElementById('adm-pass').focus();
});

// Auto-check session on load
fetch('/api/admin/check')
    .then(r => { if (!r.ok) throw new Error(); })
    .then(() => {
        document.getElementById('login-screen').style.display = 'none';
        document.getElementById('admin-screen').style.display = 'block';
        initAdmin();
    })
    .catch(() => {});

// ── Init ──────────────────────────────────────────────────────────────────────

function initAdmin() {
    updateClock();
    setInterval(updateClock, 1000);
    loadAll();
    setInterval(loadAll, 15000);
}

function loadAll() {
    loadOverview();
    loadGameState();
    loadUsers();
    loadTrades();
    loadSessions();
    loadContacts();
}

// ── Clock ─────────────────────────────────────────────────────────────────────

function updateClock() {
    document.getElementById('header-clock').textContent = new Date().toLocaleTimeString('fr-FR');
}

// ── Navigation ────────────────────────────────────────────────────────────────

function goTab(name) {
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
    document.getElementById('tab-' + name).classList.add('active');
    event.currentTarget.classList.add('active');
}

// ── Overview ──────────────────────────────────────────────────────────────────

function loadOverview() {
    fetch('/api/admin/overview')
        .then(r => r.json())
        .then(d => {
            document.getElementById('ov-users').textContent  = d.user_count;
            document.getElementById('ov-trades').textContent = d.trade_count;

            const vol = d.total_volume > 1e6
                ? (d.total_volume / 1e6).toFixed(1) + 'M'
                : Math.round(d.total_volume).toLocaleString('fr-FR');
            document.getElementById('ov-volume').textContent = vol;
            document.getElementById('ov-state').textContent  = d.game_running ? 'ON' : 'OFF';
            document.getElementById('ov-state').style.color  = d.game_running ? 'var(--green)' : 'var(--red)';

            const tbody = document.getElementById('recent-trades-body');
            if (!d.recent_trades || d.recent_trades.length === 0) {
                tbody.innerHTML = '<tr><td colspan="7" class="dim" style="padding:20px;text-align:center;">No trades yet</td></tr>';
                return;
            }
            tbody.innerHTML = d.recent_trades.map(t => `
                <tr>
                  <td class="dim mono">${t.timestamp}</td>
                  <td style="color:var(--amber)">${escHtml(t.username)}</td>
                  <td><span class="tag tag-${t.action === 'buy' ? 'buy' : 'sell'}">${t.action.toUpperCase()}</span></td>
                  <td class="mono">${t.symbol}</td>
                  <td class="mono">${fmtNum(t.quantity, 6)}</td>
                  <td class="mono">${fmtNum(t.price, 2)} $</td>
                  <td class="mono ${t.action === 'buy' ? 'neg' : 'pos'}">${fmtNum(t.value, 2)} $</td>
                </tr>`).join('');
        })
        .catch(() => {});
}

// ── Game state ────────────────────────────────────────────────────────────────

function loadGameState() {
    fetch('/api/admin/game-state')
        .then(r => r.json())
        .then(d => applyGameState(d.running));
}

function applyGameState(running) {
    gameRunning = running;
    const dot = document.getElementById('state-dot');
    const txt = document.getElementById('state-text');
    const sub = document.getElementById('state-sub');
    const btn = document.getElementById('btn-toggle');

    if (running) {
        dot.className   = 'state-dot';
        txt.className   = 'state-text running';
        txt.textContent = 'GAME RUNNING';
        sub.textContent = 'Trades are allowed';
        btn.className   = 'btn-toggle-game stop';
        btn.textContent = '⏹ STOP GAME';
    } else {
        dot.className   = 'state-dot stopped';
        txt.className   = 'state-text stopped';
        txt.textContent = 'GAME STOPPED';
        sub.textContent = 'All trades are blocked';
        btn.className   = 'btn-toggle-game start';
        btn.textContent = '▶ START GAME';
    }
}

function toggleGameState() {
    const action = gameRunning ? 'stop' : 'start';
    const msg    = gameRunning
        ? 'Stopping the game will immediately block all trades. Confirm?'
        : 'Starting the game will allow trades again. Confirm?';

    openConfirm(msg, () => {
        const params = new URLSearchParams();
        params.append('action', action);
        fetch('/api/admin/game-state', { method: 'POST', body: params })
            .then(r => r.json())
            .then(d => {
                applyGameState(d.running);
                showToast(d.running ? 'Game started ▶' : 'Game stopped ⏹');
            })
            .catch(() => showToast('Server error', true));
    });
}

// ── Users ─────────────────────────────────────────────────────────────────────

function loadUsers() {
    fetch('/api/admin/users')
        .then(r => r.json())
        .then(data => {
            allUsers = data;
            renderUsers(data);
        });
}

function renderUsers(data) {
    document.getElementById('users-count').textContent = `${data.length} player(s)`;
    const tbody = document.getElementById('users-body');

    if (!data.length) {
        tbody.innerHTML = '<tr><td colspan="7" class="dim" style="padding:20px;text-align:center;">No players</td></tr>';
        return;
    }

    // Pending accounts appear first
    data.sort((a, b) => (a.status === 'pending' ? -1 : 1));

    tbody.innerHTML = data.map(u => {
        const isPending   = u.status === 'pending';
        const statusBadge = isPending
            ? '<span class="tag tag-usd">PENDING</span>'
            : '<span class="tag tag-buy">ACTIVE</span>';
        const usdDisplay  = isPending ? '—' : `${fmtNum(u.usd, 2)} $`;
        const valDisplay  = isPending ? '—' : `${fmtNum(u.portfolio_value, 2)} $`;

        const actions = isPending
            ? `<button class="btn-sm btn-sm-green" onclick="approveUser(${u.id}, '${escHtml(u.username)}')">✔ Approve</button>
               <button class="btn-sm btn-sm-red"   onclick="deleteUser(${u.id}, '${escHtml(u.username)}')">✖ Reject</button>`
            : `<button class="btn-sm btn-sm-amber" onclick="openBalanceModal(${u.id},'${escHtml(u.username)}',${u.usd || 0})">Balance</button>
               <button class="btn-sm btn-sm-blue"  onclick="openPortfolioModal(${u.id},'${escHtml(u.username)}')">Assets</button>
               <button class="btn-sm btn-sm-red"   onclick="resetUser(${u.id},'${escHtml(u.username)}')">Reset</button>
               <button class="btn-sm btn-sm-red"   onclick="deleteUser(${u.id},'${escHtml(u.username)}')">Delete</button>`;

        return `<tr>
          <td class="dim">${u.id}</td>
          <td style="color:var(--amber);font-weight:bold;">${escHtml(u.username)}</td>
          <td class="mono" style="color:var(--text-dim);">${escHtml(u.phone || '—')}</td>
          <td>${statusBadge}</td>
          <td class="mono">${usdDisplay}</td>
          <td class="mono">${valDisplay}</td>
          <td style="display:flex;gap:6px;flex-wrap:wrap;">${actions}</td>
        </tr>`;
    }).join('');
}

function approveUser(uid, username) {
    openConfirm(`Approve <strong>${escHtml(username)}</strong> and grant 100,000 $ starting balance?`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/approve-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Account approved ✓'); loadUsers(); })
            .catch(() => showToast('Approval failed', true));
    });
}

function filterUsers(q) {
    const f = q.toLowerCase();
    renderUsers(allUsers.filter(u => u.username.toLowerCase().includes(f)));
}

// ── Trades ────────────────────────────────────────────────────────────────────

function loadTrades() {
    fetch('/api/admin/trades')
        .then(r => r.json())
        .then(data => {
            allTrades = data;
            tradePage = 1;
            renderTrades(data);
        });
}

function renderTrades(data) {
    document.getElementById('trades-count').textContent = `${data.length} trade(s)`;
    const start = (tradePage - 1) * PAGE_SIZE;
    const slice = data.slice(start, start + PAGE_SIZE);
    const tbody = document.getElementById('trades-body');

    if (!slice.length) {
        tbody.innerHTML = '<tr><td colspan="9" class="dim" style="padding:20px;text-align:center;">No trades</td></tr>';
        renderPagination('trades-pagination', data.length, tradePage, p => { tradePage = p; renderTrades(data); });
        return;
    }

    tbody.innerHTML = slice.map(t => `
        <tr>
          <td class="dim">${t.id}</td>
          <td class="dim mono">${t.timestamp}</td>
          <td style="color:var(--amber)">${escHtml(t.username)}</td>
          <td><span class="tag tag-${t.action === 'buy' ? 'buy' : 'sell'}">${t.action.toUpperCase()}</span></td>
          <td class="mono">${t.symbol}</td>
          <td class="mono">${fmtNum(t.quantity, 6)}</td>
          <td class="mono">${fmtNum(t.price, 2)} $</td>
          <td class="mono">${fmtNum(t.value, 2)} $</td>
          <td><button class="btn-sm btn-sm-red" onclick="deleteTrade(${t.id})">✕</button></td>
        </tr>`).join('');

    renderPagination('trades-pagination', data.length, tradePage, p => { tradePage = p; renderTrades(data); });
}

function filterTrades(q) {
    const f = q.toLowerCase();
    tradePage = 1;
    renderTrades(allTrades.filter(t =>
        t.username.toLowerCase().includes(f) || t.symbol.toLowerCase().includes(f)
    ));
}

// ── Sessions ──────────────────────────────────────────────────────────────────

function loadSessions() {
    fetch('/api/admin/sessions')
        .then(r => r.json())
        .then(data => {
            allSessions = data;
            sessionPage = 1;
            renderSessions(data);
        });
}

function renderSessions(data) {
    document.getElementById('sessions-count').textContent = `${data.length} entry(ies)`;
    const start = (sessionPage - 1) * PAGE_SIZE;
    const slice = data.slice(start, start + PAGE_SIZE);
    const tbody = document.getElementById('sessions-body');

    tbody.innerHTML = slice.map(s => `
        <tr>
          <td class="mono dim">${s.timestamp}</td>
          <td style="color:var(--amber)">${escHtml(s.username)}</td>
          <td class="mono" style="color:var(--blue)">${s.ip}</td>
          <td class="dim" style="font-size:10px;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;"
              title="${escHtml(s.user_agent ?? '')}">${escHtml(s.user_agent ?? '—')}</td>
          <td><span class="tag ${s.action === 'login' ? 'tag-buy' : 'tag-sell'}">${s.action.toUpperCase()}</span></td>
        </tr>`).join('');

    renderPagination('sessions-pagination', data.length, sessionPage, p => { sessionPage = p; renderSessions(data); });
}

function filterSessions(q) {
    const f = q.toLowerCase();
    sessionPage = 1;
    renderSessions(allSessions.filter(s =>
        s.ip.toLowerCase().includes(f) || s.username.toLowerCase().includes(f)
    ));
}

// ── Contacts ──────────────────────────────────────────────────────────────────

function loadContacts() {
    fetch('/api/admin/contacts')
        .then(r => r.json())
        .then(data => {
            const tbody = document.getElementById('contacts-body');
            if (!data.length) {
                tbody.innerHTML = '<tr><td colspan="4" class="dim" style="padding:20px;text-align:center;">No messages</td></tr>';
                return;
            }
            tbody.innerHTML = data.map(c => `
                <tr>
                  <td class="dim mono">${c.timestamp}</td>
                  <td style="color:var(--amber)">${escHtml(c.username)}</td>
                  <td style="color:var(--text-dim)">${escHtml(c.subject)}</td>
                  <td style="font-size:11px;max-width:400px;word-break:break-word;">${escHtml(c.message)}</td>
                </tr>`).join('');
        });
}

// ── User actions ──────────────────────────────────────────────────────────────

let pendingBalanceUserId = null;

function openBalanceModal(uid, username, currentUsd) {
    pendingBalanceUserId = uid;
    document.getElementById('modal-balance-user').textContent = username;
    document.getElementById('modal-balance-val').value = currentUsd.toFixed(2);
    document.getElementById('modal-balance').classList.add('open');
}

function confirmBalance() {
    const val = parseFloat(document.getElementById('modal-balance-val').value);
    if (isNaN(val) || val < 0) { showToast('Invalid value', true); return; }

    const params = new URLSearchParams();
    params.append('user_id', pendingBalanceUserId);
    params.append('amount',  val);

    fetch('/api/admin/set-balance', { method: 'POST', body: params })
        .then(r => { if (!r.ok) throw new Error(); })
        .then(() => { closeModal('modal-balance'); showToast('Balance updated ✓'); loadUsers(); })
        .catch(() => showToast('Error', true));
}

function resetUser(uid, username) {
    openConfirm(`Reset <strong>${escHtml(username)}</strong>'s portfolio to 100,000 $ and clear all assets?`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/reset-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Portfolio reset ✓'); loadUsers(); })
            .catch(() => showToast('Error', true));
    });
}

function deleteUser(uid, username) {
    openConfirm(`<span style="color:var(--red)">PERMANENT DELETION</span> of account <strong>${escHtml(username)}</strong> and all their data. Irreversible.`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/delete-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Account deleted'); loadUsers(); })
            .catch(() => showToast('Error', true));
    });
}

function deleteTrade(tid) {
    openConfirm("Delete this trade from history? (portfolios are not modified)", () => {
        const params = new URLSearchParams();
        params.append('trade_id', tid);
        fetch('/api/admin/delete-trade', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Trade deleted'); loadTrades(); })
            .catch(() => showToast('Error', true));
    });
}

// ── Portfolio modal ───────────────────────────────────────────────────────────

let pendingPortfolioUserId = null;

async function openPortfolioModal(uid, username) {
    pendingPortfolioUserId = uid;
    document.getElementById('modal-portfolio-user').textContent = username;
    document.getElementById('modal-portfolio-list').innerHTML =
        '<tr><td colspan="3" class="dim" style="text-align:center;">Loading...</td></tr>';
    document.getElementById('modal-portfolio-new-sym').value = '';
    document.getElementById('modal-portfolio-new-qty').value = '';
    document.getElementById('modal-portfolio').classList.add('open');
    await fetchPortfolioList();
}

async function fetchPortfolioList() {
    try {
        const r    = await fetch(`/api/admin/user-portfolio?user_id=${pendingPortfolioUserId}`);
        const data = await r.json();
        const tbody = document.getElementById('modal-portfolio-list');

        if (data.length === 0) {
            tbody.innerHTML = '<tr><td colspan="3" class="dim" style="text-align:center;padding:15px;">Empty portfolio</td></tr>';
            return;
        }

        tbody.innerHTML = data.map((item, i) => `
            <tr>
              <td class="mono" style="color:var(--text);">${item.symbol}</td>
              <td>
                <input id="asset-qty-${i}" type="number" step="0.000001" value="${item.quantity}"
                       style="width:100%; background:transparent; border:1px solid var(--border); color:var(--green); padding:4px; font-family:monospace;">
              </td>
              <td style="text-align:right; display:flex; gap:6px; justify-content:flex-end;">
                <button class="btn-sm btn-sm-green" onclick="adminUpdateAsset('edit', '${item.symbol}', 'asset-qty-${i}')">✓</button>
                <button class="btn-sm btn-sm-red"   onclick="adminUpdateAsset('delete', '${item.symbol}', null)">✕</button>
              </td>
            </tr>`).join('');
    } catch (e) {
        document.getElementById('modal-portfolio-list').innerHTML =
            '<tr><td colspan="3" class="dim" style="color:var(--red);">Network error</td></tr>';
    }
}

async function adminUpdateAsset(action, symbol = null, inputId = null) {
    let finalSymbol   = symbol;
    let finalQuantity = 0;

    if (action === 'new') {
        finalSymbol   = document.getElementById('modal-portfolio-new-sym').value.trim().toUpperCase();
        finalQuantity = parseFloat(document.getElementById('modal-portfolio-new-qty').value);
        if (!finalSymbol) { showToast('Symbol required', true); return; }
    } else if (action === 'edit') {
        finalQuantity = parseFloat(document.getElementById(inputId).value);
    }
    // action === 'delete' → finalQuantity stays 0, which triggers deletion server-side

    if (isNaN(finalQuantity) || finalQuantity < 0) { showToast('Invalid quantity', true); return; }

    const params = new URLSearchParams();
    params.append('user_id',  pendingPortfolioUserId);
    params.append('symbol',   finalSymbol);
    params.append('quantity', finalQuantity);

    try {
        const r = await fetch('/api/admin/portfolio', { method: 'POST', body: params });
        if (!r.ok) throw new Error();

        showToast(finalQuantity === 0 ? 'Asset removed ✓' : 'Updated ✓');

        if (action === 'new') {
            document.getElementById('modal-portfolio-new-sym').value = '';
            document.getElementById('modal-portfolio-new-qty').value = '';
        }
        await fetchPortfolioList();
        loadUsers();
    } catch {
        showToast('Server error', true);
    }
}

// ── UI helpers ────────────────────────────────────────────────────────────────

function openConfirm(msg, callback) {
    document.getElementById('confirm-text').innerHTML = msg;
    document.getElementById('confirm-ok-btn').onclick = () => { closeModal('confirm-modal'); callback(); };
    document.getElementById('confirm-modal').classList.add('open');
}

function closeModal(id) {
    document.getElementById(id).classList.remove('open');
}

function showToast(msg, isError = false) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.className   = 'show' + (isError ? ' err' : '');
    clearTimeout(t._timer);
    t._timer = setTimeout(() => { t.className = ''; }, 3000);
}

function renderPagination(containerId, total, current, onPage) {
    const pages = Math.ceil(total / PAGE_SIZE);
    const el    = document.getElementById(containerId);
    if (pages <= 1) { el.innerHTML = ''; return; }
    let html = '';
    for (let i = 1; i <= pages; i++)
        html += `<button class="page-btn ${i === current ? 'active' : ''}" onclick="(${onPage})(${i})">${i}</button>`;
    el.innerHTML = html;
}

function fmtNum(n, dec = 2) {
    if (n === null || n === undefined) return '—';
    return Number(n).toLocaleString('fr-FR', { minimumFractionDigits: dec, maximumFractionDigits: dec });
}

function escHtml(s) {
    return String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}