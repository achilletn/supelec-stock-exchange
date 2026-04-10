// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────
const API = '';   // même origine
let gameRunning = true;

// Données en mémoire
let allUsers    = [];
let allTrades   = [];
let allSessions = [];

// Pagination
const PAGE_SIZE = 30;
let tradePage   = 1;
let sessionPage = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Auth
// ─────────────────────────────────────────────────────────────────────────────
function doLogin() {
    const u = document.getElementById('adm-user').value;
    const p = document.getElementById('adm-pass').value;
    const params = new URLSearchParams();
    params.append('user', u);
    params.append('pass', p);
    fetch('/api/admin/login', { method: 'POST', body: params })
        .then(r => { if (!r.ok) throw new Error(); return r.json(); })
        .then(() => {
            document.getElementById('login-screen').style.display  = 'none';
            document.getElementById('admin-screen').style.display  = 'block';
            initAdmin();
        })
        .catch(() => {
            document.getElementById('login-err').textContent = '// ACCÈS REFUSÉ — Identifiants incorrects';
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

// Vérif session au chargement
fetch('/api/admin/check')
    .then(r => { if (!r.ok) throw new Error(); })
    .then(() => {
        document.getElementById('login-screen').style.display = 'none';
        document.getElementById('admin-screen').style.display = 'block';
        initAdmin();
    })
    .catch(() => {});

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────
function initAdmin() {
    updateClock();
    setInterval(updateClock, 1000);
    loadAll();
    setInterval(loadAll, 15000);
    const savedTab = localStorage.getItem('adminTab') || 'overview';
    goTab(savedTab);
}

function loadAll() {
    loadOverview();
    loadGameState();
    loadUsers();
    loadTrades();
    loadSessions();
    loadContacts();
}

// ─────────────────────────────────────────────────────────────────────────────
// Clock
// ─────────────────────────────────────────────────────────────────────────────
function updateClock() {
    document.getElementById('header-clock').textContent = new Date().toLocaleTimeString('fr-FR');
}

// ─────────────────────────────────────────────────────────────────────────────
// Navigation
// ─────────────────────────────────────────────────────────────────────────────
function goTab(name) {
    document.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
    document.getElementById('tab-' + name).classList.add('active');
    const navItem = document.querySelector(`.nav-item[onclick*="'${name}'"]`);
    if (navItem) navItem.classList.add('active');
    localStorage.setItem('adminTab', name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Overview
// ─────────────────────────────────────────────────────────────────────────────
function loadOverview() {
    fetch('/api/admin/overview')
        .then(r => r.json())
        .then(d => {
            document.getElementById('ov-users').textContent  = d.nb_users;
            document.getElementById('ov-trades').textContent = d.nb_trades;
            const vol = d.volume_total > 1e6
                ? (d.volume_total / 1e6).toFixed(1) + 'M'
                : Math.round(d.volume_total).toLocaleString('fr-FR');
            document.getElementById('ov-volume').textContent = vol;
            document.getElementById('ov-state').textContent  = d.game_running ? 'ON' : 'OFF';
            document.getElementById('ov-state').style.color  = d.game_running ? 'var(--green)' : 'var(--red)';

            const tbody = document.getElementById('recent-trades-body');
            if (!d.recent_trades || d.recent_trades.length === 0) {
                tbody.innerHTML = '<tr><td colspan="7" class="dim" style="padding:20px;text-align:center;">Aucun trade</td></tr>';
                return;
            }
            tbody.innerHTML = d.recent_trades.map(t => `
                <tr>
                  <td class="dim mono">${t.timestamp}</td>
                  <td style="color:var(--amber)">${escHtml(t.username)}</td>
                  <td><span class="tag tag-${t.action === 'achat' ? 'buy' : 'sell'}">${t.action.toUpperCase()}</span></td>
                  <td class="mono">${t.symbole}</td>
                  <td class="mono">${fmtNum(t.quantite, 6)}</td>
                  <td class="mono">${fmtNum(t.prix, 2)} $</td>
                  <td class="mono ${t.action === 'achat' ? 'neg' : 'pos'}">${fmtNum(t.valeur, 2)} $</td>
                </tr>`).join('');
        })
        .catch(() => {});
}

// ─────────────────────────────────────────────────────────────────────────────
// Game State
// ─────────────────────────────────────────────────────────────────────────────
function loadGameState() {
    fetch('/api/admin/game-state')
        .then(r => r.json())
        .then(d => applyGameState(d.running));
}

function applyGameState(running) {
    gameRunning = running;
    const dot  = document.getElementById('state-dot');
    const txt  = document.getElementById('state-text');
    const sub  = document.getElementById('state-sub');
    const btn  = document.getElementById('btn-toggle');

    if (running) {
        dot.className  = 'state-dot';
        txt.className  = 'state-text running';
        txt.textContent = 'PARTIE EN COURS';
        sub.textContent = 'Les trades sont autorisés';
        btn.className  = 'btn-toggle-game stop';
        btn.textContent = '⏹ ARRÊTER LA PARTIE';
    } else {
        dot.className  = 'state-dot stopped';
        txt.className  = 'state-text stopped';
        txt.textContent = 'PARTIE STOPPÉE';
        sub.textContent = 'Tous les trades sont bloqués';
        btn.className  = 'btn-toggle-game start';
        btn.textContent = '▶ LANCER LA PARTIE';
    }
}

function toggleGameState() {
    const action = gameRunning ? 'stop' : 'start';
    const msg    = gameRunning
        ? 'Arrêter la partie bloquera immédiatement tous les trades. Confirmer ?'
        : 'Relancer la partie autorisera de nouveau les trades. Confirmer ?';
    openConfirm(msg, () => {
        const params = new URLSearchParams();
        params.append('action', action);
        fetch('/api/admin/game-state', { method: 'POST', body: params })
            .then(r => r.json())
            .then(d => { applyGameState(d.running); showToast(d.running ? 'Partie lancée ▶' : 'Partie stoppée ⏹'); })
            .catch(() => showToast('Erreur serveur', true));
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Users
// ─────────────────────────────────────────────────────────────────────────────
function loadUsers() {
    fetch('/api/admin/users')
        .then(r => r.json())
        .then(data => {
            allUsers = data;
            renderUsers(data);
        });
}

function renderUsers(data) {
    document.getElementById('users-count').textContent = data.length + ' joueur(s)';
    const tbody = document.getElementById('users-body');
    if (!data.length) {
        tbody.innerHTML = '<tr><td colspan="7" class="dim" style="padding:20px;text-align:center;">Aucun joueur</td></tr>';
        return;
    }
    
    // On trie : les 'pending' s'affichent tout en haut
    data.sort((a, b) => (a.statut === 'pending' ? -1 : 1));

    tbody.innerHTML = data.map(u => {
        const isPending = (u.statut === 'pending');
        const badgeStatut = isPending 
            ? '<span class="tag tag-usd">EN ATTENTE</span>' 
            : '<span class="tag tag-buy">ACTIF</span>';
            
        // Si en attente, le solde et la valeur totale ne sont pas pertinents
        const usdDisp = isPending ? '—' : `${fmtNum(u.usd, 2)} $`;
        const valDisp = isPending ? '—' : `${fmtNum(u.valeur_totale, 2)} $`;

        // Boutons conditionnels
        let actionsHtml = '';
        if (isPending) {
            actionsHtml = `
                <button class="btn-sm btn-sm-green" onclick="approveUser(${u.id}, '${escHtml(u.username)}')">✔ Approuver</button>
                <button class="btn-sm btn-sm-red" onclick="deleteUser(${u.id}, '${escHtml(u.username)}')">✖ Refuser</button>
            `;
        } else {
            actionsHtml = `
                <button class="btn-sm btn-sm-amber" onclick="openBalanceModal(${u.id},'${escHtml(u.username)}',${u.usd || 0})">Liquidités</button>
                <button class="btn-sm btn-sm-blue" onclick="openPortfolioModal(${u.id},'${escHtml(u.username)}')">Actifs</button>
                <button class="btn-sm btn-sm-red" onclick="resetUser(${u.id},'${escHtml(u.username)}')">Reset</button>
                <button class="btn-sm btn-sm-red" onclick="deleteUser(${u.id},'${escHtml(u.username)}')">Suppr.</button>
            `;
        }

        return `<tr>
          <td class="dim">${u.id}</td>
          <td style="color:var(--amber);font-weight:bold;">${escHtml(u.username)}</td>
          <td class="mono" style="color:var(--text-dim);">${escHtml(u.telephone || '—')}</td>
          <td>${badgeStatut}</td>
          <td class="mono">${usdDisp}</td>
          <td class="mono">${valDisp}</td>
          <td style="display:flex;gap:6px;flex-wrap:wrap;">
            ${actionsHtml}
          </td>
        </tr>`;
    }).join('');
}

// Fonction pour approuver un utilisateur
function approveUser(uid, username) {
    openConfirm(`Approuver le compte de <strong>${escHtml(username)}</strong> et lui attribuer ses 100 000 $ de départ ?`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/approve-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Compte approuvé ! ✓'); loadUsers(); })
            .catch(() => showToast('Erreur lors de l\'approbation', true));
    });
}

function filterUsers(q) {
    const f = q.toLowerCase();
    renderUsers(allUsers.filter(u => u.username.toLowerCase().includes(f)));
}

// ─────────────────────────────────────────────────────────────────────────────
// Trades
// ─────────────────────────────────────────────────────────────────────────────
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
    document.getElementById('trades-count').textContent = data.length + ' trade(s)';
    const start = (tradePage - 1) * PAGE_SIZE;
    const slice = data.slice(start, start + PAGE_SIZE);
    const tbody = document.getElementById('trades-body');

    if (!slice.length) {
        tbody.innerHTML = '<tr><td colspan="9" class="dim" style="padding:20px;text-align:center;">Aucun trade</td></tr>';
        renderPagination('trades-pagination', data.length, tradePage, p => { tradePage = p; renderTrades(data); });
        return;
    }

    tbody.innerHTML = slice.map(t => `
        <tr>
          <td class="dim">${t.id}</td>
          <td class="dim mono">${t.timestamp}</td>
          <td style="color:var(--amber)">${escHtml(t.username)}</td>
          <td><span class="tag tag-${t.action === 'achat' ? 'buy' : 'sell'}">${t.action.toUpperCase()}</span></td>
          <td class="mono">${t.symbole}</td>
          <td class="mono">${fmtNum(t.quantite, 6)}</td>
          <td class="mono">${fmtNum(t.prix, 2)} $</td>
          <td class="mono">${fmtNum(t.valeur, 2)} $</td>
          <td><button class="btn-sm btn-sm-red" onclick="deleteTrade(${t.id})">✕</button></td>
        </tr>`).join('');

    renderPagination('trades-pagination', data.length, tradePage, p => { tradePage = p; renderTrades(data); });
}

function filterTrades(q) {
    const f = q.toLowerCase();
    tradePage = 1;
    renderTrades(allTrades.filter(t =>
        t.username.toLowerCase().includes(f) || t.symbole.toLowerCase().includes(f)
    ));
}

// ─────────────────────────────────────────────────────────────────────────────
// Sessions
// ─────────────────────────────────────────────────────────────────────────────
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
    document.getElementById('sessions-count').textContent = data.length + ' entrée(s)';
    const start = (sessionPage - 1) * PAGE_SIZE;
    const slice = data.slice(start, start + PAGE_SIZE);
    const tbody = document.getElementById('sessions-body');

    tbody.innerHTML = slice.map(s => `
        <tr>
          <td class="mono dim">${s.timestamp}</td>
          <td style="color:var(--amber)">${escHtml(s.username)}</td>
          <td class="mono" style="color:var(--blue)">${s.ip}</td>
          <td class="dim" style="font-size:10px;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;" title="${escHtml(s.user_agent ?? '')}">${escHtml(s.user_agent ?? '—')}</td>
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

// ─────────────────────────────────────────────────────────────────────────────
// Contacts
// ─────────────────────────────────────────────────────────────────────────────
function loadContacts() {
    fetch('/api/admin/contacts')
        .then(r => r.json())
        .then(data => {
            const tbody = document.getElementById('contacts-body');
            if (!data.length) {
                tbody.innerHTML = '<tr><td colspan="4" class="dim" style="padding:20px;text-align:center;">Aucun message</td></tr>';
                return;
            }
            tbody.innerHTML = data.map(c => `
                <tr>
                  <td class="dim mono">${c.timestamp}</td>
                  <td style="color:var(--amber)">${escHtml(c.username)}</td>
                  <td style="color:var(--text-dim)">${escHtml(c.sujet)}</td>
                  <td style="font-size:11px;max-width:400px;word-break:break-word;">${escHtml(c.message)}</td>
                </tr>`).join('');
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Actions utilisateurs
// ─────────────────────────────────────────────────────────────────────────────
let pendingBalanceUserId = null;

function openBalanceModal(uid, username, currentUsd) {
    pendingBalanceUserId = uid;
    document.getElementById('modal-balance-user').textContent = username;
    document.getElementById('modal-balance-val').value = currentUsd.toFixed(2);
    document.getElementById('modal-balance').classList.add('open');
}

function confirmBalance() {
    const val = parseFloat(document.getElementById('modal-balance-val').value);
    if (isNaN(val) || val < 0) { showToast('Valeur invalide', true); return; }
    const params = new URLSearchParams();
    params.append('user_id', pendingBalanceUserId);
    params.append('amount',  val);
    fetch('/api/admin/set-balance', { method: 'POST', body: params })
        .then(r => { if (!r.ok) throw new Error(); })
        .then(() => { closeModal('modal-balance'); showToast('Liquidités mises à jour ✓'); loadUsers(); })
        .catch(() => showToast('Erreur', true));
}

function resetUser(uid, username) {
    openConfirm(`Remettre le portefeuille de <strong>${escHtml(username)}</strong> à 100 000 $ et supprimer tous ses actifs ?`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/reset-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Portefeuille réinitialisé ✓'); loadUsers(); })
            .catch(() => showToast('Erreur', true));
    });
}

function deleteUser(uid, username) {
    openConfirm(`<span style="color:var(--red)">SUPPRESSION DÉFINITIVE</span> du compte <strong>${escHtml(username)}</strong> et de toutes ses données. Action irréversible.`, () => {
        const params = new URLSearchParams();
        params.append('user_id', uid);
        fetch('/api/admin/delete-user', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Compte supprimé'); loadUsers(); })
            .catch(() => showToast('Erreur', true));
    });
}

function deleteTrade(tid) {
    openConfirm('Supprimer ce trade de l\'historique ? (ne modifie pas les portefeuilles)', () => {
        const params = new URLSearchParams();
        params.append('trade_id', tid);
        fetch('/api/admin/delete-trade', { method: 'POST', body: params })
            .then(r => { if (!r.ok) throw new Error(); })
            .then(() => { showToast('Trade supprimé'); loadTrades(); })
            .catch(() => showToast('Erreur', true));
    });
}

let pendingPortfolioUserId = null;

// 1. Ouvre la modale et charge les données du joueur
async function openPortfolioModal(uid, username) {
    pendingPortfolioUserId = uid;
    document.getElementById('modal-portfolio-user').textContent = username;
    document.getElementById('modal-portfolio-list').innerHTML = '<tr><td colspan="3" class="dim" style="text-align:center;">Chargement...</td></tr>';
    document.getElementById('modal-portfolio-new-qty').value = '';

    // Charger la liste des actifs disponibles dans le select
    const sel = document.getElementById('modal-portfolio-new-sym');
    sel.innerHTML = '<option value="">— Choisir un actif —</option>';
    try {
        const r = await fetch('/api/marche');
        const actifs = await r.json();
        actifs.forEach(a => {
            const opt = document.createElement('option');
            opt.value = a.symbol;
            opt.textContent = a.symbol;
            sel.appendChild(opt);
        });
    } catch (e) { /* si /api/marche échoue, le select reste vide */ }

    document.getElementById('modal-portfolio').classList.add('open');
    await fetchPortfolioList();
}

// 2. Récupère et affiche la liste
async function fetchPortfolioList() {
    try {
        const r = await fetch(`/api/admin/user-portfolio?user_id=${pendingPortfolioUserId}`);
        const data = await r.json();
        const tbody = document.getElementById('modal-portfolio-list');
        
        if (data.length === 0) {
            tbody.innerHTML = '<tr><td colspan="3" class="dim" style="text-align:center;padding:15px;">Portefeuille vide</td></tr>';
            return;
        }

        tbody.innerHTML = data.map((item, index) => `
            <tr>
              <td class="mono" style="color:var(--text);">${item.symbole}</td>
              <td>
                <input id="asset-qty-${index}" type="number" step="1" min="1" value="${Math.round(item.quantite)}"
                       style="width:100%; background:transparent; border:1px solid var(--border); color:var(--green); padding:4px; font-family:monospace;">
              </td>
              <td style="text-align: right; display: flex; gap: 6px; justify-content: flex-end;">
                <button class="btn-sm btn-sm-green" onclick="adminUpdateAsset('edit', '${item.symbole}', 'asset-qty-${index}')">✓</button>
                <button class="btn-sm btn-sm-red" onclick="adminUpdateAsset('delete', '${item.symbole}', null)">✕</button>
              </td>
            </tr>
        `).join('');
    } catch (e) {
        console.error(e);
        document.getElementById('modal-portfolio-list').innerHTML = '<tr><td colspan="3" class="dim" style="color:var(--red);">Erreur réseau</td></tr>';
    }
}

// 3. Fonction d'envoi (Ajout / Modification / Suppression)
async function adminUpdateAsset(action, sym = null, inputId = null) {
    let finalSym = sym;
    let finalQty = 0;

    if (action === 'new') {
        finalSym = document.getElementById('modal-portfolio-new-sym').value.trim();
        finalQty = parseInt(document.getElementById('modal-portfolio-new-qty').value, 10);
        if (!finalSym) { showToast('Symbole requis', true); return; }
    } else if (action === 'edit') {
        finalQty = parseInt(document.getElementById(inputId).value, 10);
    } else if (action === 'delete') {
        finalQty = 0; // Quantité 0 = suppression côté serveur
    }

    if (isNaN(finalQty) || finalQty < 0) { showToast('Quantité invalide', true); return; }

    const params = new URLSearchParams();
    params.append('user_id', pendingPortfolioUserId);
    params.append('symbole', finalSym);
    params.append('quantite', finalQty);

    try {
        const r = await fetch('/api/admin/portfolio', { method: 'POST', body: params });
        if (!r.ok) throw new Error();
        
        showToast(finalQty === 0 ? 'Actif supprimé ✓' : 'Mise à jour réussie ✓');
        
        // On vide les champs d'ajout si c'était un nouvel actif
        if (action === 'new') {
            document.getElementById('modal-portfolio-new-sym').value = '';
            document.getElementById('modal-portfolio-new-qty').value = '';
        }
        
        // On rafraîchit la liste dans la modale ET le grand tableau derrière
        await fetchPortfolioList();
        loadUsers(); 
    } catch (e) {
        showToast('Erreur serveur', true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UI Helpers
// ─────────────────────────────────────────────────────────────────────────────
function openConfirm(msg, cb) {
    document.getElementById('confirm-text').innerHTML = msg;
    document.getElementById('confirm-ok-btn').onclick = () => { closeModal('confirm-modal'); cb(); };
    document.getElementById('confirm-modal').classList.add('open');
}

function closeModal(id) {
    document.getElementById(id).classList.remove('open');
}

function showToast(msg, isErr = false) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.className   = 'show' + (isErr ? ' err' : '');
    clearTimeout(t._timer);
    t._timer = setTimeout(() => { t.className = ''; }, 3000);
}

function renderPagination(containerId, total, current, onPage) {
    const pages = Math.ceil(total / PAGE_SIZE);
    const el = document.getElementById(containerId);
    if (pages <= 1) { el.innerHTML = ''; return; }
    let html = '';
    for (let i = 1; i <= pages; i++) {
        html += `<button class="page-btn ${i === current ? 'active' : ''}" onclick="(${onPage})(${i})">${i}</button>`;
    }
    el.innerHTML = html;
}

function fmtNum(n, dec = 2) {
    if (n === null || n === undefined) return '—';
    return Number(n).toLocaleString('fr-FR', { minimumFractionDigits: dec, maximumFractionDigits: dec });
}

function escHtml(s) {
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}