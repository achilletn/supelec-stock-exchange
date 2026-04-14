// ── Dark mode ────────────────────────────────────────────────────────────────
if (localStorage.getItem('darkMode') === '1') document.body.classList.add('dark');

// ── Persistance UI (survit au F5) ────────────────────────────────────────────
function sauvegarderUI() {
    try {
        const detailOuvert = document.getElementById("panneau-detail-bg")?.classList.contains("open");
        const tradeOuvert  = document.getElementById("modal-trade-bg")?.classList.contains("open");
        const state = {
            tab:           localStorage.getItem("dernierTab") || "marche",
            joueurCible:   typeof portfolioJoueurCible !== "undefined" ? (portfolioJoueurCible || null) : null,
            detailSymbole: detailOuvert ? symboleActuel : null,
            detailPeriode: periodeActuelle,
            tradeSymbole:  tradeOuvert ? symboleActuel : null,
            tradePrix:     tradeOuvert ? prixActuel : null,
            tradeAction:   tradeOuvert ? tradeActionCourante : null,
        };
        sessionStorage.setItem("uiState", JSON.stringify(state));
    } catch (e) { /* silencieux */ }
}

function restaurerUI() {
    const raw = sessionStorage.getItem("uiState");
    if (!raw) { switchTab('marche'); return; }
    const state = JSON.parse(raw);

    // Profil d'un joueur : setter la cible AVANT switchTab
    if (state.joueurCible) portfolioJoueurCible = state.joueurCible;

    // Tab principal
    if (state.tab) switchTab(state.tab);

    // Fenêtre graphique
    if (state.detailSymbole) {
        periodeActuelle = state.detailPeriode || "24h";
        setTimeout(() => voirDetail(state.detailSymbole), 600);
    }

    // Fenêtre achat/vente
    if (state.tradeAction && state.tradeSymbole) {
        setTimeout(() => {
            symboleActuel = state.tradeSymbole;
            prixActuel    = state.tradePrix || 0;
            ouvrirModalTrade(state.tradeAction);
        }, 650);
    }
}

function toggleDarkMode() {
    const isDark = document.body.classList.toggle('dark');
    localStorage.setItem('darkMode', isDark ? '1' : '0');
}

let utilisateurActuel = "";
let masterTickInterval = null;
let cacheClassement = {};
let graphiqueActif = null;
let portfolioChart = null;
let classementChart = null;
let affichageClassementGraphique = false;
let graphiqueLabels = [];   // labels temps courants du graphique (pour ticks.callback)
let classementLabels = [];  // labels temps courants pour le classement
let symboleActuel = "";
let periodeActuelle = "24h";
let tickCount = 0;
let prixActuel = 0;
let graphiquePremierChargement = true;

let historiquePortefeuilleCharge = false;
let historiqueTradesCharge = false;

// ── Scroll lock robuste (position:fixed, iOS-safe) ───────────────────────────
let _scrollLockCount = 0;
let _scrollLockY = 0;
let _touchLockStartY = 0;
function _onTouchLockStart(e) {
    _touchLockStartY = e.touches[0].clientY;
}
function _preventTouch(e) {
    const panels = ['panneau-detail', 'trade-modal'];
    const panelEl = panels.map(id => document.getElementById(id)).find(el => el?.contains(e.target));
    if (panelEl) {
        // Bloquer pull-to-refresh : si panel en haut et swipe vers le bas → preventDefault
        const dy = e.touches[0].clientY - _touchLockStartY;
        if (panelEl.scrollTop <= 0 && dy > 0) e.preventDefault();
        return;
    }
    e.preventDefault();
}
function lockScroll() {
    if (++_scrollLockCount === 1) {
        _scrollLockY = window.scrollY;
        document.body.style.position = 'fixed';
        document.body.style.top      = `-${_scrollLockY}px`;
        document.body.style.width    = '100%';
        document.addEventListener('touchstart', _onTouchLockStart, { passive: true });
        document.addEventListener('touchmove',  _preventTouch,     { passive: false });
    }
}
function unlockScroll() {
    if (--_scrollLockCount <= 0) {
        _scrollLockCount = 0;
        document.removeEventListener('touchstart', _onTouchLockStart);
        document.removeEventListener('touchmove',  _preventTouch);
        document.body.style.position = '';
        document.body.style.top      = '';
        document.body.style.width    = '';
        window.scrollTo(0, _scrollLockY);
    }
}

const PERIODE_ORDER = ['1h', '3h', '24h', '7d', '1m', '4m', '1y', '5y'];

const NOM_ACTIF = {
    "AAPL":"Apple",        "MSFT":"Microsoft",     "NVDA":"NVIDIA",        "TSLA":"Tesla",
    "GOOGL":"Alphabet",    "AMZN":"Amazon",         "META":"Meta",          "NFLX":"Netflix",
    "AMD":"AMD",           "INTC":"Intel",          "QCOM":"Qualcomm",      "AVGO":"Broadcom",
    "TSM":"TSMC",          "ASML":"ASML",           "MU":"Micron",          "AMAT":"Appl. Materials",
    "JPM":"JPMorgan",      "GS":"Goldman Sachs",    "BAC":"Bank of America","V":"Visa",
    "MA":"Mastercard",     "BRK":"Berkshire",       "AXP":"Amex",           "BLK":"BlackRock",
    "JNJ":"J&J",           "PFE":"Pfizer",          "LLY":"Eli Lilly",      "ABBV":"AbbVie",
    "MRK":"Merck",         "UNH":"UnitedHealth",    "BMY":"Bristol-Myers",  "GILD":"Gilead",
    "XOM":"ExxonMobil",    "CVX":"Chevron",         "NEE":"NextEra",        "CAT":"Caterpillar",
    "BA":"Boeing",         "GE":"GE Aerospace",     "RTX":"RTX Corp",       "HON":"Honeywell",
    "WMT":"Walmart",       "COST":"Costco",         "TGT":"Target",         "NKE":"Nike",
    "SBUX":"Starbucks",    "MCD":"McDonald's",      "DIS":"Disney",         "PYPL":"PayPal",
};

let affichageNom = false;
let dernierActifs = [];

function labelActif(sym) {
    return affichageNom && NOM_ACTIF[sym] ? NOM_ACTIF[sym] : sym;
}

function setAffichageNom(useNom) {
    affichageNom = useNom;
    document.querySelectorAll(".actif-display-toggle").forEach(t => t.classList.toggle("nom-actif", useNom));
    afficherTableauMarche();
    afficherActifs(dernierActifs);
    // Mettre à jour les panneaux ouverts
    if (symboleActuel) {
        const detailTitre = document.getElementById("detail-titre");
        if (detailTitre) detailTitre.innerText = labelActif(symboleActuel);
        const tradeSymbole = document.getElementById("trade-modal-symbole");
        if (tradeSymbole) tradeSymbole.innerText = labelActif(symboleActuel);
    }
}
const PERIODE_SEC   = { '1h': 3600, '3h': 10800, '24h': 86400, '7d': 604800,
                        '1m': 2592000, '4m': 10368000, '1y': 31536000, '5y': 157680000 };

let axeAnimRafId = null;

function calculerBornesY(prices) {
    if (!prices || !prices.length) return { min: 0, max: 0 };
    let yMin = Infinity, yMax = -Infinity;
    for (let i = 0; i < prices.length; i++) {
        const v = prices[i].y !== undefined ? prices[i].y : prices[i];
        if (v < yMin) yMin = v;
        if (v > yMax) yMax = v;
    }
    const yPad = (yMax - yMin) * 0.05 || yMax * 0.05 || 1;
    return { min: yMin - yPad, max: yMax + yPad };
}

// Anime x.min et y.min/y.max d'un chart Chart.js en simultané
function animerAxes(chart, from, to, duration, easing, onComplete) {
    if (axeAnimRafId) cancelAnimationFrame(axeAnimRafId);
    const ds = chart.data.datasets[0];
    const savedFill = ds.fill;
    ds.fill = false;

    const start = performance.now();
    function step(now) {
        const t = Math.min((now - start) / duration, 1);
        const e = easing === 'in'  ? t * t * t
                : easing === 'out' ? 1 - (1 - t) * (1 - t) * (1 - t)
                :                    t < 0.5 ? 4*t*t*t : 1 - 4*(1-t)*(1-t)*(1-t);
        
        chart.options.scales.x.min = from.xMin + (to.xMin - from.xMin) * e;
        chart.options.scales.y.min = from.yMin + (to.yMin - from.yMin) * e;
        chart.options.scales.y.max = from.yMax + (to.yMax - from.yMax) * e;
        chart.update('none');
        
        if (t < 1) {
            axeAnimRafId = requestAnimationFrame(step);
        } else {
            axeAnimRafId = null;
            ds.fill = savedFill;
            chart.update('none');
            if (onComplete) onComplete();
        }
    }
    axeAnimRafId = requestAnimationFrame(step);
}

const tickHandlers = new Set();
const syncClockHandles = {};
const GRAPH_REFRESH_RATE = 1;
const cacheGraphique = {};
const CACHE_GRAPH_MAX = 40; // max entrées (50 actifs × 8 périodes potentiels → on garde les 40 dernières)
function setCacheGraphique(key, val) {
    cacheGraphique[key] = { val: val, ts: Date.now() };
    const keys = Object.keys(cacheGraphique);
    if (keys.length > CACHE_GRAPH_MAX) delete cacheGraphique[keys[0]];
}

function obtenirHistorique(symbol, periode) {
    const key = getCacheKey(symbol, periode);
    const cache = cacheGraphique[key];
    // Utiliser le cache s'il a moins de 60 secondes (évite le spam réseau lors des clics rapides)
    if (cache && (Date.now() - cache.ts < 60000)) {
        return Promise.resolve(cache.val);
    }
    return fetch(`/api/historique?symbole=${symbol}&periode=${periode}`)
        .then(r => r.json())
        .then(data => {
            const val = { labels: data.map(d => d.time), prices: data.map(d => d.price) };
            setCacheGraphique(key, val);
            return val;
        });
}

let CYCLE_MS = 65000;
let serverLastUpdateMs = 0;   // horodatage Unix (ms) de la dernière update serveur
let masterTickTimeout = null;
let masterTickStarted = false;
let resyncInterval = null;

// Format uniforme : 1 234.56 (espace insécable comme séparateur milliers, point décimal)
function formatNum(n, decimals = 2) {
    return n.toLocaleString("en-US", {
        minimumFractionDigits: decimals,
        maximumFractionDigits: decimals
    }).replace(/,/g, '\u00A0');
}
function formatDevise(n) { return formatNum(n) + '\u00A0$'; }

async function fetchSyncStatus() {
    const d = await fetch('/api/sync-status').then(r => r.json());
    CYCLE_MS = d.cycle_ms || 65000;
    if (d.last_update_ms > 0) serverLastUpdateMs = d.last_update_ms;
}

function startMasterTick() {
    if (masterTickStarted) return;
    masterTickStarted = true;

    const tick = () => {
        tickCount++;
        tickHandlers.forEach(fn => fn(tickCount));
    };

    fetchSyncStatus()
        .then(() => {
            const elapsed = serverLastUpdateMs > 0 ? (Date.now() - serverLastUpdateMs) % CYCLE_MS : 0;
            const remaining = CYCLE_MS - elapsed;

            masterTickTimeout = setTimeout(() => {
                masterTickTimeout = null;
                tick();
                masterTickInterval = setInterval(tick, CYCLE_MS);
            }, remaining);

            // Re-sync toutes les 5 minutes pour corriger la dérive éventuelle
            resyncInterval = setInterval(() => {
                fetchSyncStatus().then(() => {
                    // Si le setInterval a dérivé de plus de 2s, on le recale
                    const elapsedNow = (Date.now() - serverLastUpdateMs) % CYCLE_MS;
                    const drift = Math.abs(elapsedNow - CYCLE_MS);
                    if (drift > 2000 && drift < CYCLE_MS - 2000) {
                        clearInterval(masterTickInterval);
                        const remaining2 = CYCLE_MS - elapsedNow;
                        masterTickTimeout = setTimeout(() => {
                            masterTickTimeout = null;
                            tick();
                            masterTickInterval = setInterval(tick, CYCLE_MS);
                        }, remaining2);
                    }
                });
            }, 5 * 60 * 1000);
        })
        .catch(() => {
            masterTickInterval = setInterval(tick, CYCLE_MS);
        });
}

function stopMasterTick() {
    if (masterTickTimeout) { clearTimeout(masterTickTimeout); masterTickTimeout = null; }
    clearInterval(masterTickInterval);
    clearInterval(resyncInterval);
    masterTickInterval = null;
    resyncInterval = null;
    masterTickStarted = false;
}

function tickGraphique(tick) {
    if (symboleActuel && tick % GRAPH_REFRESH_RATE === 0) {
        chargerGraphique(symboleActuel, periodeActuelle);
    }
}

function verifierSession() {
    const loader = document.getElementById("chargement");
    fetch("/api/check-session")
        .then(res => {
            if (loader) loader.style.display = "none";
            if (res.ok) return res.json();
            throw new Error();
        })
        .then(data => {
            utilisateurActuel = data.user;
            demarrerJeu();
        })
        .catch(() => {
            if (loader) loader.style.display = "none";
            document.getElementById("zone-login").style.display = "block";
        });
}

function seConnecter() {
    const params = new URLSearchParams();
    params.append("user", document.getElementById("user").value);
    params.append("pass", document.getElementById("pass").value);

    fetch("/api/login", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) {
                // On utilise le message envoyé par le serveur s'il existe
                throw new Error(data.erreur || "Identifiants incorrects");
            }
            return data;
        })
        .then(() => fetch("/api/check-session").then(r => r.json()))
        .then(data => {
            utilisateurActuel = data.user;
            demarrerJeu();
        })
        .catch(err => {
            // Ici, err.message contiendra "Compte en attente de validation..."
            document.getElementById("message-erreur").innerText = err.message;
        });
}

function sInscrire() {
    const params = new URLSearchParams();
    params.append("user", document.getElementById("reg-user").value);
    params.append("pass", document.getElementById("reg-pass").value);
    params.append("tel", document.getElementById("reg-tel").value); // NOUVEAU
    const msg = document.getElementById("reg-message");

    fetch("/api/register", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.erreur || "Ce pseudo ou numéro est déjà pris.");
            msg.style.color = "orange";
            msg.innerText = "Compte créé ! En attente de validation par un administrateur ⏳";
            
            // Vider les champs
            document.getElementById("reg-user").value = "";
            document.getElementById("reg-pass").value = "";
            document.getElementById("reg-tel").value = "";
        })
        .catch(err => {
            msg.style.color = "red";
            msg.innerText = err.message;
        });
}



function seDeconnecter() {
    fetch("/api/logout", { method: "POST" }).then(() => {
        localStorage.removeItem("dernierTab");
        sessionStorage.removeItem("uiState");
        tickHandlers.clear();
        stopMasterTick();
        location.reload();
    });
}

function demarrerJeu() {
    document.getElementById("zone-login").style.display = "none";
    document.getElementById("zone-jeu").style.display = "block";

    actualiserDashboard();
    // On synchronise le dashboard avec les mises à jour du marché (toutes les 65s) au lieu d'un interval asynchrone de 30s
    tickHandlers.add(actualiserDashboard);

    if (!window._clockInterval) window._clockInterval = setInterval(() => {
        const el = document.getElementById("dash-clock");
        if (el) el.innerText = new Date().toLocaleTimeString();
    }, 1000);
    startMasterTick();
    initStickyNav();
    initDetailMobileDismiss();

    restaurerUI();
}

function initDetailMobileDismiss() {
    [
        { el: document.getElementById('panneau-detail'), close: () => fermerDetail() },
        { el: document.getElementById('trade-modal'),    close: () => fermerModalTrade(null) },
    ].forEach(({ el, close }) => {
        let startY = 0;
        el.addEventListener('touchstart', e => {
            startY = e.touches[0].clientY;
        }, { passive: true });
        el.addEventListener('touchend', e => {
            if (window.innerWidth > 700) return;
            const delta = e.changedTouches[0].clientY - startY;
            if (delta > 60 && el.scrollTop < 10) close();
        }, { passive: true });
    });
}

function initStickyNav() {
    const sticky = document.getElementById('game-nav-sticky');
    const orig   = document.querySelector('#zone-jeu > nav.game-nav:not(.game-nav-sticky)');
    if (!sticky || !orig) return;

    let lastY   = window.scrollY;
    let ticking = false;

    window.addEventListener('scroll', function() {
        if (ticking) return;
        ticking = true;
        requestAnimationFrame(function() {
            ticking = false;
            if (_scrollLockCount > 0) return; // panel ouvert → ignorer
            const y         = window.scrollY;
            const origBelow = orig.getBoundingClientRect().top > 15;
            const mobile    = window.innerWidth <= 700;

            if (origBelow) {
                sticky.classList.remove('nav-visible');
            } else if (mobile) {
                const delta = y - lastY;
                if (delta < -8)     sticky.classList.add('nav-visible');
                else if (delta > 5) sticky.classList.remove('nav-visible');
            } else {
                sticky.classList.add('nav-visible');
            }

            lastY = y;
        });
    }, { passive: true });
}

function switchTab(tabName) {
    localStorage.setItem("dernierTab", tabName);
    sauvegarderUI();
    document.querySelectorAll(".game-nav [data-tab]").forEach(b => b.classList.remove("active"));
    document.querySelectorAll(`.game-nav [data-tab="${tabName}"]`).forEach(b => b.classList.add("active"));
    document.querySelectorAll(".tab-content").forEach(t => (t.style.display = "none"));
    
    const target = document.getElementById(`tab-${tabName}`);
    if (target) target.style.display = "block";

    const isPropos = tabName === "propos";
    document.querySelector(".user-dashboard-header").style.display = isPropos ? "none" : "flex";

    arreterBoucleMarche();
    arreterBoucleClassement();
    arreterBouclePortefeuille();

    if (tabName === "marche") demarrerBoucleMarche();
    else if (tabName === "classement") demarrerBoucleClassement();
    else if (tabName === "portefeuille") { 
        historiquePortefeuilleCharge = false;
        historiqueTradesCharge = false;
        demarrerBouclePortefeuille(); 
    }
}

function demarrerBoucleMarche() {
    tickHandlers.add(actualiserTableauMarche);
    tickHandlers.add(tickGraphique);
    actualiserTableauMarche();
    startSyncClock("sync-clock");
}

function arreterBoucleMarche() {
    tickHandlers.delete(actualiserTableauMarche);
    tickHandlers.delete(tickGraphique);
    cancelSyncClock("sync-clock");
}

let donneesMarche = [];
let filtreMarche  = "";
let triMarche     = { col: null, dir: 1 };
let _marcheFetchCtrl = null;

function actualiserTableauMarche() {
    const tbody = document.getElementById("market-body");
    if (!tbody) return;

    // Annuler le fetch précédent s'il est encore en vol (évite les race conditions)
    if (_marcheFetchCtrl) _marcheFetchCtrl.abort();
    _marcheFetchCtrl = new AbortController();

    fetch("/api/marche", { signal: _marcheFetchCtrl.signal })
        .then(res => res.json())
        .then(data => {
            _marcheFetchCtrl = null;
            if (data.length === 0) {
                tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;padding:20px;color:#f39c12;">Synchronisation en cours... 📡</td></tr>`;
                return;
            }
            donneesMarche = data;

            // Mise à jour silencieuse du prix dans le panneau détail/trade ouvert
            const actuelData = data.find(a => a.symbol === symboleActuel);
            if (actuelData) {
                prixActuel = actuelData.price;
                const elPrix = document.getElementById("detail-prix");
                if (elPrix) elPrix.innerText = formatDevise(actuelData.price);
                majPreviewTrade();
            }

            afficherTableauMarche();
            majTicker(data);
        })
        .catch(err => {
            if (err.name === 'AbortError') return; // fetch annulé volontairement
            tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;color:red;">Erreur réseau ❌</td></tr>`;
        });
}

function majTicker(data) {
    const container = document.getElementById("ticker-content");
    if (!container || !data || data.length === 0) return;

    // Trier par variation sur 24h
    const sorted = [...data].sort((a, b) => b.variation_24h - a.variation_24h);
    
    // Récupérer les 4 meilleurs et les 4 pires
    const top4 = sorted.slice(0, 4);
    const bottom4 = sorted.slice(-4).reverse(); // Pire en premier

    const buildItem = (item, isUp) => {
        const sign = item.variation_24h > 0 ? '+' : '';
        return `<div class="ticker-item ${isUp ? 'up' : 'down'}" onclick="switchTab('marche'); voirDetail('${item.symbol}')">
                    <span>${labelActif(item.symbol)}</span>${sign}${item.variation_24h.toFixed(2)}%
                </div>`;
    };

    let html = '';
    top4.forEach(item => html += buildItem(item, true));
    bottom4.forEach(item => html += buildItem(item, false));

    // On duplique 4 fois pour être sûr de remplir l'écran et boucler de manière parfaitement invisible
    container.innerHTML = html + html + html + html;
}

function afficherTableauMarche() {
    const tbody = document.getElementById("market-body");
    if (!tbody || donneesMarche.length === 0) return;

    // Filtrage
    let données = donneesMarche;
    if (filtreMarche) {
        données = données.filter(a =>
            a.symbol.toUpperCase().includes(filtreMarche) ||
            (NOM_ACTIF[a.symbol] || "").toUpperCase().includes(filtreMarche)
        );
    }

    // Tri
    if (triMarche.col) {
        données = [...données].sort((a, b) => {
            const va = a[triMarche.col];
            const vb = b[triMarche.col];
            if (typeof va === "string") return triMarche.dir * va.localeCompare(vb);
            return triMarche.dir * (va - vb);
        });
    }

    // Compteur
    const countEl = document.getElementById("market-count");
    if (countEl) {
        countEl.textContent = filtreMarche
            ? `${données.length} / ${donneesMarche.length} actif${donneesMarche.length > 1 ? "s" : ""}`
            : `${donneesMarche.length} actif${donneesMarche.length > 1 ? "s" : ""}`;
    }

    // Indicateurs visuels de tri sur les <th>
    ["symbol", "price", "variation_24h"].forEach(col => {
        const thId = col === "symbol" ? "th-symbol" : col === "price" ? "th-price" : "th-variation";
        const th = document.getElementById(thId);
        if (!th) return;
        th.classList.remove("asc", "desc");
        if (triMarche.col === col) th.classList.add(triMarche.dir === 1 ? "asc" : "desc");
    });

    // Rendu des lignes
    tbody.innerHTML = "";
    if (données.length === 0) {
        tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;padding:20px;color:#888;">Aucun actif trouvé pour « <span id="no-result-text"></span> »</td></tr>`;
        document.getElementById("no-result-text").textContent = filtreMarche; // .textContent bloque l'exécution HTML
        return;
    }
    données.forEach(action => {
        const tr = document.createElement("tr");
        tr.classList.add("row-cliquable");
        tr.addEventListener("click", e => {
            if (!e.target.closest("button")) voirDetail(action.symbol);
        });
        const variColor = action.variation_24h >= 0 ? "#2ecc71" : "#e74c3c";
        const variSign  = action.variation_24h >= 0 ? "+" : "";
        tr.innerHTML = `
            <td><span class="lien-actif"><strong>${labelActif(action.symbol)}</strong></span></td>
            <td class="price">${formatDevise(action.price)}</td>
            <td style="font-family:'JetBrains Mono','Courier New',monospace; font-weight:bold; color:${variColor};">
                ${variSign}${action.variation_24h.toFixed(2)}%
            </td>
            <td style="display:flex; gap:6px; justify-content:center;">
                <button class="btn-achat" style="padding:4px 10px; font-size:0.85em;" onclick="achatDepuisMarche('${action.symbol}', ${action.price})">Acheter</button>
                <button class="btn-vente" style="padding:4px 10px; font-size:0.85em;" onclick="venteDepuisMarche('${action.symbol}', ${action.price})">Vendre</button>
            </td>`;
        tbody.appendChild(tr);
    });
}

function trierColonneMarche(col) {
    if (triMarche.col === col) {
        triMarche.dir = -triMarche.dir; // inverser direction
    } else {
        triMarche.col = col;
        triMarche.dir = 1;
    }
    afficherTableauMarche();
}

function toggleAffichageClassement() {
    affichageClassementGraphique = !affichageClassementGraphique;
    const toggle = document.getElementById("toggle-classement-vue");
    if (toggle) toggle.classList.toggle("nom-actif", affichageClassementGraphique);

    document.getElementById("classement-table-container").style.display = affichageClassementGraphique ? "none" : "block";
    document.getElementById("classement-graph-container").style.display = affichageClassementGraphique ? "block" : "none";

    if (affichageClassementGraphique) {
        chargerClassementGraphique();
    }
}

function demarrerBoucleClassement() {
    tickHandlers.add(chargerClassement);
    chargerClassement();
    startSyncClock("sync-clock-classement");
}

function arreterBoucleClassement() {
    tickHandlers.delete(chargerClassement);
    cancelSyncClock("sync-clock-classement");
}

function chargerClassement() {
    fetch("/api/classement")
        .then(res => res.json())
        .then(data => {
            const moi = data.find(u => u.username === utilisateurActuel);
            if (moi) appliquerDonneesDashboard(moi);

            const tbody = document.getElementById("leaderboard-body");
            if (!tbody) return;
            tbody.innerHTML = "";
            
            data.forEach((joueur, index) => {
                const tr = document.createElement("tr");
                const medaille = index === 0 ? "🥇" : index === 1 ? "🥈" : index === 2 ? "🥉" : index + 1;

                const valeurAncienne = cacheClassement[joueur.username];
                if (valeurAncienne !== undefined) {
                    if (joueur.valeur_totale > valeurAncienne) tr.className = "gain-update";
                    else if (joueur.valeur_totale < valeurAncienne) tr.className = "perte-update";
                }
                cacheClassement[joueur.username] = joueur.valeur_totale;

                const colorTotal = joueur.pnl.startsWith("+") && joueur.pnl !== "+0.00%" ? "#2ecc71" : joueur.pnl.startsWith("-") ? "#e74c3c" : "gray";
                const color24h = joueur.pnl_24h_pct?.startsWith("+") && joueur.pnl_24h_pct !== "+0.00%" ? "#2ecc71" : joueur.pnl_24h_pct?.startsWith("-") ? "#e74c3c" : "gray";

                tr.innerHTML = `
                    <td>${medaille}</td>
                    <td><strong><a href="#" class="lien-joueur" onclick="voirPortefeuilleJoueur('${joueur.username}');return false;">${joueur.username}</a></strong></td>
                    <td style="font-weight:bold;">${formatDevise(joueur.valeur_totale)}</td>
                    <td style="color:${color24h};font-weight:bold;">
                        ${joueur.pnl_24h_usd ?? "—"}\u00A0$<br><span style="font-size:0.85em;">${joueur.pnl_24h_pct ?? "—"}</span>
                    </td>
                    <td style="color:${colorTotal};font-weight:bold;">
                        ${joueur.pnl_total_usd ?? "—"}\u00A0$<br><span style="font-size:0.85em;">${joueur.pnl}</span>
                    </td>`;
                tbody.appendChild(tr);
            });
        
        if (affichageClassementGraphique) {
            chargerClassementGraphique();
        }
        });
}

function appliquerDonneesDashboard(data) {
    document.getElementById("dash-username").innerText = data.username.toUpperCase();
    document.getElementById("dash-wallet").innerText = formatDevise(data.valeur_totale);

    const pnlEl = document.getElementById("dash-pnl");
    pnlEl.innerText = data.pnl;
    pnlEl.style.color = data.pnl.startsWith("+") && data.pnl !== "+0.00%" ? "#2ecc71" : data.pnl.startsWith("-") ? "#e74c3c" : "#888";

    const usdEl = document.getElementById("dash-usd");
    if (usdEl && data.usd !== undefined) {
        usdEl.innerText = formatDevise(data.usd);
    }
}

function actualiserDashboard() {
    if (!utilisateurActuel) return;
    // Optimisation : Si on est sur l'onglet classement, les données du dashboard 
    // sont déjà incluses nativement dans la réponse de /api/classement. On évite le doublon !
    const tabClassement = document.getElementById("tab-classement");
    if (tabClassement && tabClassement.style.display === "block") return;

    fetch("/api/dashboard").then(res => res.json()).then(data => appliquerDonneesDashboard(data));
}

let portfolioJoueurCible = null; // null = propre portefeuille

function voirPortefeuilleJoueur(username) {
    portfolioJoueurCible = username;
    localStorage.setItem("dernierTab", "portefeuille");
    sauvegarderUI(); // appelé APRÈS que le tab soit mis à jour
    document.querySelectorAll(".tab-content").forEach(t => (t.style.display = "none"));
    const target = document.getElementById("tab-portefeuille");
    if (target) target.style.display = "block";
    document.querySelector(".user-dashboard-header").style.display = "flex";
    arreterBoucleMarche();
    arreterBoucleClassement();
    arreterBouclePortefeuille();
    demarrerBouclePortefeuille();
}

function retourClassement() {
    portfolioJoueurCible = null;
    switchTab("classement");
}

function demarrerBouclePortefeuille() {
    tickHandlers.add(chargerPortefeuille);
    chargerPortefeuille();
    startSyncClock("sync-clock-portefeuille");
}

function arreterBouclePortefeuille() {
    tickHandlers.delete(chargerPortefeuille);
    cancelSyncClock("sync-clock-portefeuille");
}

function afficherActifs(actifs) {
    dernierActifs = actifs;
    const tbody = document.getElementById("portfolio-body");
    if (!tbody) return;
    tbody.innerHTML = "";
    actifs.forEach(ligne => {
        const color24h = ligne.variation_24h?.startsWith("+") && ligne.variation_24h !== "+0.00%" ? "#2ecc71"
            : ligne.variation_24h?.startsWith("-") ? "#e74c3c" : "#888";
        const colorAlltime = ligne.pnl_alltime_pct?.startsWith("+") && ligne.pnl_alltime_pct !== "+0.00%" ? "#2ecc71"
            : ligne.pnl_alltime_pct?.startsWith("-") ? "#e74c3c" : "#888";
        const tr = document.createElement("tr");
        tr.innerHTML = `
            <td><a href="#" class="lien-actif" onclick="voirDetail('${ligne.symbole}');return false;"><strong>${labelActif(ligne.symbole)}</strong></a></td>
            <td style="font-family:'Courier New',monospace;">${formatNum(ligne.quantite, 6)}</td>
            <td style="font-family:'Courier New',monospace;">${ligne.prix_unitaire > 0 ? formatDevise(ligne.prix_unitaire) : "—"}</td>
            <td style="font-family:'Courier New',monospace; font-weight:bold;">${formatDevise(ligne.valeur)}</td>
            <td style="font-family:'Courier New',monospace; color:${color24h};">${ligne.variation_24h ?? "—"}</td>
            <td style="font-family:'Courier New',monospace; color:${colorAlltime};">${ligne.pnl_alltime_pct ?? "—"}<br><span style="font-size:0.85em;">${ligne.pnl_alltime_usd ?? "—"}\u00A0$</span></td>
            ${!portfolioJoueurCible ? `<td><button class="btn-vente" style="padding:4px 12px; font-size:0.85em;" onclick="vendreDepuisPortefeuille('${ligne.symbole}', ${ligne.prix_unitaire})">Vendre</button></td>` : '<td></td>'}`;
        tbody.appendChild(tr);
    });
    if (actifs.length === 0) {
        const tr = document.createElement("tr");
        tr.innerHTML = `<td colspan="7" style="text-align:center; padding:20px; color:var(--text-muted);">Aucune position ouverte</td>`;
        tbody.appendChild(tr);
    }
}

function chargerPortefeuille() {
    const titre = document.getElementById("portefeuille-titre");
    const btnRetour = document.getElementById("btn-retour-classement");
    const histTable = document.getElementById("portfolio-history-table");

    const joueurSummary = document.getElementById("portfolio-joueur-summary");

    const sectionTrades = document.getElementById("section-trades-perso");

    if (portfolioJoueurCible) {
        // Vue d'un autre joueur
        if (!historiquePortefeuilleCharge) {
        if (titre) titre.innerText = `Portefeuille de ${portfolioJoueurCible}`;
        if (btnRetour) btnRetour.style.display = "inline-block";
        if (joueurSummary) joueurSummary.style.display = "flex";
        if (histTable) histTable.style.display = "";
        if (sectionTrades) sectionTrades.style.display = "none";

        fetch(`/api/portefeuille/joueur?username=${encodeURIComponent(portfolioJoueurCible)}`)
            .then(res => res.json())
            .then(data => {
                // Résumé
                const pjUsd = document.getElementById("pj-usd");
                const pjPnl = document.getElementById("pj-pnltotal");
                if (pjUsd) pjUsd.innerText = formatDevise(data.usd);
                if (pjPnl && data.pnl_total_pct) {
                    const isPos = data.pnl_total_pct.startsWith("+") && data.pnl_total_pct !== "+0.00%";
                    pjPnl.innerText = `${data.pnl_total_pct}  (${data.pnl_total_usd}\u00A0$)`;
                    pjPnl.style.color = isPos ? "#2ecc71" : data.pnl_total_pct.startsWith("-") ? "#e74c3c" : "#888";
                }
                // Historique
                afficherHistorique(data.historique || []);
                // Actifs
                afficherActifs(data.actifs || []);
                    historiquePortefeuilleCharge = true;
            });
            chargerGraphiquePortefeuille(portfolioJoueurCible);
        }
        return;
    }

    // Vue propre portefeuille
    if (titre) titre.innerText = "Mon portefeuille";
    if (btnRetour) btnRetour.style.display = "none";
    if (joueurSummary) joueurSummary.style.display = "none";
    if (histTable) histTable.style.display = "";
    if (sectionTrades) sectionTrades.style.display = "";

    // 1. Les positions ouvertes bougent avec le marché, on les refresh à chaque tick
    fetch("/api/portefeuille").then(r => r.json()).then(portefeuille => {
        afficherActifs(portefeuille.actifs || []);
    }).catch(() => {});

    // 2. Historique journalier (ne bouge qu'une fois/jour) et trades (bougent via actions du joueur)
    // On évite de les redemander toutes les 65s inutilement !
    if (!historiquePortefeuilleCharge) {
        fetch("/api/portefeuille/historique").then(r => r.json()).then(historique => {
            afficherHistorique(historique);
            historiquePortefeuilleCharge = true;
        }).catch(() => {});
        chargerGraphiquePortefeuille(null);
    }
    
    if (!historiqueTradesCharge) {
        chargerHistoriquesTrades();
        historiqueTradesCharge = true;
    }
}

function afficherHistorique(rows) {
    const tbody = document.getElementById("portfolio-history-body");
    if (!tbody) return;
    tbody.innerHTML = "";
    rows.forEach(row => {
        const isPos = row.pnl_day_pct.startsWith("+") && row.pnl_day_pct !== "+0.00%";
        const color = isPos ? "#2ecc71" : row.pnl_day_pct.startsWith("-") ? "#e74c3c" : "#888";
        const tr = document.createElement("tr");
        tr.innerHTML = `
            <td>${row.today ? `<strong>${row.date} (aujourd'hui)</strong>` : row.date}</td>
            <td style="font-family:'Courier New',monospace; color:${color}; font-weight:bold;">${row.pnl_day_pct}</td>
            <td style="font-family:'Courier New',monospace;">#${row.rank}</td>`;
        tbody.appendChild(tr);
    });
}

function chargerHistoriquesTrades() {
    const tbody = document.getElementById("trades-history-body");
    if (!tbody) return;

    fetch("/api/trades/historique")
        .then(res => res.json())
        .then(trades => {
            tbody.innerHTML = "";
            if (trades.length === 0) {
                tbody.innerHTML = `<tr><td colspan="6" style="text-align:center;padding:16px;color:#888;">Aucun trade effectué pour l'instant.</td></tr>`;
                return;
            }
            trades.forEach(t => {
                const isAchat = t.action === "achat";
                const couleurAction = isAchat ? "#2ecc71" : "#e74c3c";
                const labelAction = isAchat ? "Achat" : "Vente";
                const tr = document.createElement("tr");
                tr.innerHTML = `
                    <td style="color:#888;font-size:0.9em;">${t.timestamp}</td>
                    <td><strong style="color:${couleurAction}">${labelAction}</strong></td>
                    <td><a href="#" class="lien-actif" onclick="voirDetail('${t.symbole}');return false;"><strong>${t.symbole}</strong></a></td>
                    <td style="font-family:'JetBrains Mono','Courier New',monospace;">${formatNum(t.quantite, 6)}</td>
                    <td style="font-family:'JetBrains Mono','Courier New',monospace;">${formatDevise(t.prix)}</td>
                    <td style="font-family:'JetBrains Mono','Courier New',monospace;font-weight:bold;">${formatDevise(t.valeur)}</td>`;
                tbody.appendChild(tr);
            });
        })
        .catch(() => {
            tbody.innerHTML = `<tr><td colspan="6" style="text-align:center;color:red;">Erreur de chargement.</td></tr>`;
        });
}

function startSyncClock(canvasId) {
    cancelSyncClock(canvasId);
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;

    // HiDPI : résolution interne × devicePixelRatio
    const dpr = window.devicePixelRatio || 1;
    const size = 28;
    canvas.width  = size * dpr;
    canvas.height = size * dpr;
    canvas.style.width  = size + "px";
    canvas.style.height = size + "px";

    const ctx = canvas.getContext("2d");
    ctx.scale(dpr, dpr);
    let resetting = false;

    const localStart = Date.now();
    function draw() {
        const elapsed = serverLastUpdateMs > 0
            ? (Date.now() - serverLastUpdateMs) % CYCLE_MS
            : (Date.now() - localStart) % CYCLE_MS;   // fallback local si pas encore sync
        const progress = elapsed / CYCLE_MS;
        ctx.clearRect(0, 0, size, size);

        const cx = 14, cy = 14, r = 10;
        const startAngle = -Math.PI / 2;
        const endAngle = startAngle + progress * 2 * Math.PI;

        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.strokeStyle = "rgba(128,128,128,0.2)";
        ctx.lineWidth = 2.5;
        ctx.stroke();

        if (resetting) {
            ctx.strokeStyle = "#2ecc71";
            ctx.stroke();
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(9, 14); ctx.lineTo(12.5, 17.5); ctx.lineTo(19, 10);
            ctx.stroke();
        } else if (progress <= 0.98) {
            const grad = ctx.createLinearGradient(cx - r, cy, cx + r, cy);
            grad.addColorStop(0, "#1877f2"); grad.addColorStop(1, "#2ecc71");
            ctx.beginPath();
            ctx.arc(cx, cy, r, startAngle, endAngle);
            ctx.strokeStyle = grad;
            ctx.stroke();
            
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

function voirDetail(symbol) {
    symboleActuel = symbol;
    graphiquePremierChargement = true;
    document.getElementById("detail-titre").innerText = labelActif(symbol);
    document.getElementById("trade-message").innerText = "";
    document.getElementById("modal-trade-bg").classList.remove("open");

    const bg    = document.getElementById("panneau-detail-bg");
    const panel = document.getElementById("panneau-detail");

    lockScroll();

    if (window.innerWidth <= 700) {
        bg.style.opacity    = '0';
        bg.style.transition = 'none';
        panel.style.transition = 'none';
        panel.style.transform  = 'translateY(110%)';
        bg.classList.add("open");
        requestAnimationFrame(() => requestAnimationFrame(() => {
            bg.style.transition    = 'opacity 0.30s ease';
            bg.style.opacity       = '1';
            panel.style.transition = 'transform 0.30s cubic-bezier(0.4, 0, 0.2, 1)';
            panel.style.transform  = 'translateY(0)';
        }));
    } else {
        bg.classList.add("open");
    }

    chargerGraphique(symboleActuel, periodeActuelle);
    chargerStats(symbol);
    sauvegarderUI();
}

function chargerStats(symbol) {
    const fmt = (v) => v != null ? formatDevise(v) : "—";
    const fmtPct = (v, decimals = 2) => v != null ? (v >= 0 ? "+" : "") + v.toFixed(decimals) + "%" : "—";

    // Réinitialiser
    ["stat-open","stat-close","stat-high","stat-low","stat-variation","stat-volatilite"].forEach(id => {
        const el = document.getElementById(id);
        if (el) { el.innerText = "…"; el.style.color = ""; }
    });

    fetch(`/api/stats?symbole=${encodeURIComponent(symbol)}`)
        .then(r => r.json())
        .then(d => {
            const set = (id, val) => { const el = document.getElementById(id); if (el) el.innerText = val; };
            const setColor = (id, val) => {
                const el = document.getElementById(id);
                if (!el) return;
                el.innerText = val;
                el.style.color = val.startsWith("+") ? "#2ecc71" : val.startsWith("-") ? "#e74c3c" : "";
            };
            set("stat-open",       fmt(d.open));
            set("stat-close",      fmt(d.close));
            set("stat-high",       fmt(d.high));
            set("stat-low",        fmt(d.low));
            setColor("stat-variation",  fmtPct(d.variation));
            set("stat-volatilite", d.volatilite != null ? d.volatilite.toFixed(2) + "%" : "—");
        })
        .catch(() => {
            ["stat-open","stat-close","stat-high","stat-low","stat-variation","stat-volatilite"]
                .forEach(id => { const el = document.getElementById(id); if (el) el.innerText = "—"; });
        });
}

function fermerDetail() {
    const bg    = document.getElementById("panneau-detail-bg");
    const panel = document.getElementById("panneau-detail");

    const cleanup = () => {
        bg.classList.remove("open");
        panel.style.transform  = '';
        panel.style.transition = '';
        bg.style.opacity    = '';
        bg.style.transition = '';
        unlockScroll();
        if (graphiqueActif) { graphiqueActif.destroy(); graphiqueActif = null; }
        symboleActuel = "";
        sauvegarderUI();
    };

    if (window.innerWidth <= 700) {
        bg.style.transition    = 'opacity 0.26s ease';
        bg.style.opacity       = '0';
        panel.style.transition = 'transform 0.26s cubic-bezier(0.4, 0, 0.2, 1)';
        panel.style.transform  = 'translateY(110%)';
        setTimeout(cleanup, 280);
    } else {
        cleanup();
    }
}

function fermerDetailOverlay(event) {
    if (event.target === document.getElementById("panneau-detail-bg")) fermerDetail();
}

function changerFenetre(periode) {
    const prev = periodeActuelle;
    periodeActuelle = periode;
    document.querySelectorAll(".btn-time").forEach(b => b.classList.remove("active"));
    document.getElementById(`btn-${periode}`).classList.add("active");
    chargerGraphique(symboleActuel, periode, prev);
}

function getCacheKey(symbol, periode) { return `${symbol}_${periode}`; }

function chargerGraphique(symbol, periode, prevPeriode) {
    const isTransition = !!prevPeriode && prevPeriode !== periode;
    const zoomIn = isTransition &&
        PERIODE_ORDER.indexOf(periode) < PERIODE_ORDER.indexOf(prevPeriode);

    // Fraction visible utilisée pour les animations : ratio exact des fenêtres temporelles,
    // plancher à 3% pour éviter de finir sur 1 seul point, pas de plafond artificiel
    const zoomRatio = (from, to) =>
        Math.max(0.03, PERIODE_SEC[to] / PERIODE_SEC[from]);

    if (!isTransition) {
        obtenirHistorique(symbol, periode)
            .then(c => {
                if (!c.prices.length) return;
                prixActuel = c.prices[c.prices.length - 1];
                document.getElementById("detail-prix").innerText = formatDevise(prixActuel);
                const withAnim = graphiquePremierChargement;
                graphiquePremierChargement = false;
                dessinerGraphique(c.labels, c.prices, withAnim);
                majPreviewTrade();
            })
            .catch(err => console.error("Erreur graphique:", err));
        return;
    }

    // On récupère les bornes actuelles pour synchroniser la transition
    const oldYMin = graphiqueActif?.scales?.y?.min ?? graphiqueActif?.options?.scales?.y?.min ?? 0;
    const oldYMax = graphiqueActif?.scales?.y?.max ?? graphiqueActif?.options?.scales?.y?.max ?? 0;
    const oldXMin = graphiqueActif?.options?.scales?.x?.min ?? 0;
    
    // Dans tous les cas (zoom in/out), on fetch d'abord pour connaître les futures bornes Y
        obtenirHistorique(symbol, periode)
            .then(c => {
            if (!c.prices.length) return;
            
            prixActuel = c.prices[c.prices.length - 1];
            document.getElementById("detail-prix").innerText = formatDevise(prixActuel);
            graphiquePremierChargement = false;
            majPreviewTrade();

            const newYBounds = calculerBornesY(c.prices);

            if (zoomIn) {
                if (graphiqueActif) {
                    const N_old = graphiqueActif.data.datasets[0].data.length;
                    const targetXMin = Math.floor(N_old * (1 - zoomRatio(prevPeriode, periode)));

                    animerAxes(
                        graphiqueActif,
                        { xMin: oldXMin, yMin: oldYMin, yMax: oldYMax },
                        { xMin: targetXMin, yMin: newYBounds.min, yMax: newYBounds.max },
                        375, 'inout',
                        () => {
                            // Swap sans délai une fois l'ancien graph aligné
                            dessinerGraphique(c.labels, c.prices, false);
                        }
                    );
                } else {
                    dessinerGraphique(c.labels, c.prices, false);
                }
            } else {
                const startXMin = Math.floor(c.prices.length * (1 - zoomRatio(periode, prevPeriode)));

                // Dessine le nouveau graph mais restreint sur l'ancienne fenêtre (X et Y)
                dessinerGraphique(c.labels, c.prices, false, {
                    xMin: startXMin > 0 ? startXMin : 0,
                    yMin: oldYMin,
                    yMax: oldYMax
                });

                if (graphiqueActif && startXMin > 0) {
                    animerAxes(
                        graphiqueActif,
                        { xMin: startXMin, yMin: oldYMin, yMax: oldYMax },
                        { xMin: 0, yMin: newYBounds.min, yMax: newYBounds.max },
                        425, 'in',
                        () => {
                            delete graphiqueActif.options.scales.y.min;
                            delete graphiqueActif.options.scales.y.max;
                            graphiqueActif.update('none');
                        }
                    );
                }
            }
        })
        .catch(err => console.error("Erreur graphique:", err));
}

function dessinerGraphique(labels, prices, animate = false, initialBounds = null) {
    const ctx = document.getElementById("graphique-actif").getContext("2d");

    const isDark = document.body.classList.contains("dark");
    const curveColor = isDark ? "#6aa3ff" : "#1877f2";
    const fillColor  = isDark ? "rgba(106,163,255,0.15)" : "rgba(24,119,242,0.1)";
    const gridColor  = isDark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.06)";
    const N = prices.length;
    // Données en format {x, y} pour l'axe linéaire (accepte des valeurs min/max flottantes →
    // animation frame-to-frame en sous-pixel, sans saccades discrètes)
    const xyData = prices.map((p, i) => ({ x: i, y: p }));

    const xMin = initialBounds?.xMin !== undefined ? initialBounds.xMin : 0;
    const yMin = initialBounds?.yMin;
    const yMax = initialBounds?.yMax;

    const isMobile = window.innerWidth <= 700;
    const paddingDroit = isMobile ? 22 : 35; // Plus léger sur mobile
    const largeurAxeY = isMobile ? 50 : 65;  // On réduit la largeur de l'axe sur mobile
    const nbLabelsX = isMobile ? 5 : 7;      // Homogénéise le nombre de labels X (7 sur PC, 5 sur mobile)

    // Mise à jour en place quand aucune animation d'entrée n'est requise
    if (graphiqueActif && !animate) {
        graphiqueLabels = labels;
        graphiqueActif.data.datasets[0].data = xyData;
        graphiqueActif.data.datasets[0].borderColor = curveColor;
        graphiqueActif.data.datasets[0].backgroundColor = fillColor;
        graphiqueActif.options.scales.y.grid.color = gridColor;
        graphiqueActif.options.scales.x.max = N - 1;
        graphiqueActif.options.scales.x.min = xMin;
        
        if (!graphiqueActif.options.layout.padding) graphiqueActif.options.layout.padding = {};
        graphiqueActif.options.layout.padding.right = paddingDroit;
        graphiqueActif.options.scales.y.afterFit = (scale) => { scale.width = largeurAxeY; };
        
        if (yMin !== undefined && yMax !== undefined) {
            graphiqueActif.options.scales.y.min = yMin;
            graphiqueActif.options.scales.y.max = yMax;
        } else {
            delete graphiqueActif.options.scales.y.min;
            delete graphiqueActif.options.scales.y.max;
        }
        graphiqueActif.update('none');
        return;
    }

    if (graphiqueActif) graphiqueActif.destroy();
    graphiqueLabels = labels;

    const yScaleOptions = { 
        position: 'left', // Remet l'axe à gauche
        grace: "5%", 
        grid: { color: gridColor, lineWidth: 0.5 },
        afterFit: (scale) => {
            scale.width = largeurAxeY; // Largeur fixe responsive
        }
    };
    if (yMin !== undefined && yMax !== undefined) {
        yScaleOptions.min = yMin;
        yScaleOptions.max = yMax;
    }

    const crosshairPlugin = {
        id: 'crosshair',
        afterDraw: chart => {
            if (chart.tooltip?._active?.length) {
                const activePoint = chart.tooltip._active[0];
                const ctx = chart.ctx;
                const x = activePoint.element.x;
                const y = activePoint.element.y;
                const topY = chart.scales.y.top;
                const bottomY = chart.scales.y.bottom;
                const leftX = chart.scales.x.left;
                const rightX = chart.scales.x.right;

                ctx.save();
                ctx.beginPath();
                ctx.moveTo(x, topY); ctx.lineTo(x, bottomY); // Ligne verticale
                ctx.moveTo(leftX, y); ctx.lineTo(rightX, y); // Ligne horizontale
                ctx.lineWidth = 1;
                ctx.strokeStyle = isDark ? 'rgba(255, 255, 255, 0.15)' : 'rgba(0, 0, 0, 0.15)';
                ctx.setLineDash([4, 4]);
                ctx.stroke();
                ctx.restore();
            }
        }
    };

    graphiqueActif = new Chart(ctx, {
        type: "line",
        data: {
            datasets: [{
                label: "Prix",
                data: xyData,
                borderColor: curveColor,
                backgroundColor: fillColor,
                borderWidth: isDark ? 2.5 : 2,
                pointRadius: 0,
                pointHoverRadius: 6,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            layout: {
                padding: { right: paddingDroit } // Compense le poids visuel de l'axe gauche pour centrer la courbe
            },
            animation: animate
                ? { duration: 700, easing: "easeInOutQuart" }
                : false,
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: "index", intersect: false },
            plugins: { 
                legend: { display: false },
                tooltip: {
                    backgroundColor: isDark ? 'rgba(30, 45, 61, 0.95)' : 'rgba(255, 255, 255, 0.95)',
                    titleColor: isDark ? '#c9d1d9' : '#1c1e21',
                    bodyColor: curveColor,
                    borderColor: isDark ? '#3d4068' : '#e1e4e8',
                    borderWidth: 1,
                    padding: 12,
                    displayColors: false,
                    titleFont: { family: "'Share Tech Mono', monospace", size: 12 },
                    bodyFont: { family: "'Share Tech Mono', monospace", size: 14, weight: 'bold' },
                    callbacks: {
                        title: (tooltipItems) => {
                            const idx = Math.round(tooltipItems[0].parsed.x);
                            return (idx >= 0 && idx < graphiqueLabels.length) ? graphiqueLabels[idx] : '';
                        },
                        label: (context) => { return formatNum(context.parsed.y) + ' $'; }
                    }
                }
            },
            scales: {
                x: {
                    type: 'linear',
                    min: xMin,
                    max: N - 1,
                    grid: { display: false },
                    ticks: {
                        maxRotation: 35,
                        minRotation: 35,
                        count: nbLabelsX, // Force la répartition homogène exacte
                        // Mappe les indices numériques vers les labels temporels
                        callback: (value) => {
                            const idx = Math.round(value);
                            return (idx >= 0 && idx < graphiqueLabels.length) ? graphiqueLabels[idx] : '';
                        }
                    }
                },
                y: yScaleOptions
            }
        },
        plugins: [crosshairPlugin]
    });
}

let tradeActionCourante = null;

function achatDepuisMarche(symbole, prix) {
    symboleActuel = symbole;
    prixActuel = prix;
    ouvrirModalTrade('achat');
}

function venteDepuisMarche(symbole, prix) {
    symboleActuel = symbole;
    prixActuel = prix;
    ouvrirModalTrade('vente');
}

function vendreDepuisPortefeuille(symbole, prix) {
    symboleActuel = symbole;
    prixActuel = prix;
    ouvrirModalTrade('vente');
}

function ouvrirModalTrade(action) {
    tradeActionCourante = action;
    const bg = document.getElementById("modal-trade-bg");
    const titre = document.getElementById("trade-modal-titre");
    const btn = document.getElementById("trade-modal-btn");
    const prixEl = document.getElementById("trade-modal-prix");
    const qteInput = document.getElementById("trade-modal-qte");
    const msg = document.getElementById("trade-modal-msg");

    titre.innerText = action === "achat" ? "Acheter" : "Vendre";
    document.getElementById("trade-modal-symbole").innerText = labelActif(symboleActuel);
    btn.className = "trade-modal-confirm " + (action === "achat" ? "btn-achat" : "btn-vente");
    btn.innerText = action === "achat" ? "Confirmer l'achat" : "Confirmer la vente";

    prixEl.innerText = prixActuel > 0 ? formatDevise(prixActuel) : "—";

    document.getElementById("prev-total-label").innerText = action === "achat" ? "Total à débiter" : "Net à recevoir";

    qteInput.value = "";
    msg.innerText = "";
    document.getElementById("prev-brut").innerText = "—";
    document.getElementById("prev-frais").innerText = "—";
    document.getElementById("prev-total").innerText = "—";

    lockScroll();

    const modal = document.getElementById("trade-modal");
    if (window.innerWidth <= 700) {
        bg.style.opacity    = '0';
        bg.style.transition = 'none';
        modal.style.transition = 'none';
        modal.style.transform  = 'translateY(110%)';
        bg.classList.add("open");
        requestAnimationFrame(() => requestAnimationFrame(() => {
            bg.style.transition    = 'opacity 0.30s ease';
            bg.style.opacity       = '1';
            modal.style.transition = 'transform 0.30s cubic-bezier(0.4, 0, 0.2, 1)';
            modal.style.transform  = 'translateY(0)';
        }));
    } else {
        bg.classList.add("open");
    }
    setTimeout(() => qteInput.focus(), 50);
    sauvegarderUI();
}

function fermerModalTrade(event) {
    if (event && event.target !== document.getElementById("modal-trade-bg")) return;
    const bg    = document.getElementById("modal-trade-bg");
    const modal = document.getElementById("trade-modal");

    const cleanup = () => {
        bg.classList.remove("open");
        modal.style.transform  = '';
        modal.style.transition = '';
        bg.style.opacity    = '';
        bg.style.transition = '';
        tradeActionCourante = null;
        unlockScroll();
        sauvegarderUI();
    };

    if (window.innerWidth <= 700) {
        bg.style.transition    = 'opacity 0.26s ease';
        bg.style.opacity       = '0';
        modal.style.transition = 'transform 0.26s cubic-bezier(0.4, 0, 0.2, 1)';
        modal.style.transform  = 'translateY(110%)';
        setTimeout(cleanup, 280);
    } else {
        cleanup();
    }
}

function majPreviewTrade() {
    const qte = parseInt(document.getElementById("trade-modal-qte").value, 10);

    if (!qte || qte <= 0 || prixActuel <= 0) {
        document.getElementById("prev-brut").innerText = "—";
        document.getElementById("prev-frais").innerText = "—";
        document.getElementById("prev-total").innerText = "—";
        return;
    }

    const brut = qte * prixActuel;
    const frais = brut * 0.003;
    const total = tradeActionCourante === "achat" ? brut + frais : brut - frais;

    document.getElementById("prev-brut").innerText = formatDevise(brut);
    document.getElementById("prev-frais").innerText = formatDevise(frais);
    document.getElementById("prev-total").innerText = (tradeActionCourante === "achat" ? "-" : "+") + formatDevise(total);
}

function confirmerTrade() {
    const qte = parseInt(document.getElementById("trade-modal-qte").value, 10);
    const msg = document.getElementById("trade-modal-msg");

    if (!qte || qte <= 0) {
        msg.innerText = "Veuillez entrer une quantité valide.";
        msg.style.color = "#e74c3c";
        return;
    }

    msg.innerText = "Transaction en cours... ⏳";
    msg.style.color = "orange";

    const params = new URLSearchParams();
    params.append("symbole", symboleActuel);
    params.append("action", tradeActionCourante);
    params.append("quantite", qte);

    fetch("/api/trade", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.erreur || "Erreur de transaction");
            msg.innerText = `${data.message} ✅`;
            msg.style.color = "#2ecc71";
            const msgBox = document.getElementById("trade-message");
            if (msgBox) { msgBox.innerText = `${data.message} ✅`; msgBox.style.color = "green"; }
            actualiserDashboard();
            
            // On reset les drapeaux pour forcer le rechargement de l'historique car un trade a été effectué
            historiqueTradesCharge = false;
            historiquePortefeuilleCharge = false;
            
            const tabPortefeuille = document.getElementById("tab-portefeuille");
            if (tabPortefeuille && tabPortefeuille.style.display === "block" && !portfolioJoueurCible) {
                chargerPortefeuille();
            }

            setTimeout(() => fermerModalTrade(null), 1200);
        })
        .catch(err => {
            msg.innerText = `${err.message} ❌`;
            msg.style.color = "#e74c3c";
        });
}

document.getElementById("trade-modal-qte").addEventListener("keydown", e => {
    if (e.key === "Enter") confirmerTrade();
    if (e.key === "Escape") fermerModalTrade(null);
});
document.addEventListener("keydown", e => {
    if (e.key === "Escape") { fermerModalTrade(null); fermerDetail(); }
});

function lierToucheEntree(idActuel, idSuivant) {
    document.getElementById(idActuel).addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            document.getElementById(idSuivant).focus();
        }
    });
}

function chargerClassementGraphique() {
    fetch("/api/classement/chart")
        .then(r => r.json())
        .then(data => {
            if (!data || !data.labels || data.labels.length === 0) return;
            dessinerClassementGraphique(data.labels, data.datasets);
        })
        .catch(err => console.error("Erreur graphe classement:", err));
}

function dessinerClassementGraphique(labels, datasetsData) {
    const ctx = document.getElementById("classement-chart").getContext("2d");
    const isDark = document.body.classList.contains("dark");
    const gridColor = isDark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.06)";
    
    classementLabels = labels;

    const colors = isDark 
        ? ["#4f8ef7", "#2ecc71", "#e74c3c", "#f39c12", "#9b59b6", "#1abc9c", "#e67e22", "#3498db", "#16a085", "#d35400"]
        : ["#1877f2", "#27ae60", "#c0392b", "#d35400", "#8e44ad", "#16a085", "#e67e22", "#2980b9", "#1abc9c", "#c0392b"];

    const chartDatasets = datasetsData.map((ds, i) => {
        const color = colors[i % colors.length];
        const xyData = ds.data.map((p, idx) => ({ x: idx, y: p }));
        return {
            label: ds.username,
            data: xyData,
            borderColor: color,
            backgroundColor: 'transparent',
            borderWidth: isDark ? 2.5 : 2,
            pointRadius: 0,
            pointHoverRadius: 6,
            fill: false,
            tension: 0.1
        };
    });

    let yMin = Infinity, yMax = -Infinity;
    datasetsData.forEach(ds => {
        ds.data.forEach(v => {
            if (v < yMin) yMin = v;
            if (v > yMax) yMax = v;
        });
    });
    const yPad = (yMax - yMin) * 0.05 || yMax * 0.05 || 1;
    const finalYMin = yMin - yPad;
    const finalYMax = yMax + yPad;

    const N = labels.length;
    const isMobile = window.innerWidth <= 700;
    const paddingDroit = isMobile ? 22 : 35;
    const largeurAxeY = isMobile ? 50 : 65;
    const nbLabelsX = isMobile ? 5 : 7;

    if (classementChart) {
        classementChart.data.datasets = chartDatasets;
        classementChart.options.scales.x.max = N - 1;
        classementChart.options.scales.y.min = finalYMin;
        classementChart.options.scales.y.max = finalYMax;
        classementChart.options.scales.y.grid.color = gridColor;
        classementChart.update('none');
        return;
    }

    const crosshairPlugin = {
        id: 'crosshair',
        afterDraw: chart => {
            if (chart.tooltip?._active?.length) {
                const activePoint = chart.tooltip._active[0];
                const ctx = chart.ctx;
                const x = activePoint.element.x;
                const topY = chart.scales.y.top;
                const bottomY = chart.scales.y.bottom;

                ctx.save();
                ctx.beginPath();
                ctx.moveTo(x, topY); ctx.lineTo(x, bottomY);
                ctx.lineWidth = 1;
                ctx.strokeStyle = isDark ? 'rgba(255, 255, 255, 0.15)' : 'rgba(0, 0, 0, 0.15)';
                ctx.setLineDash([4, 4]);
                ctx.stroke();
                ctx.restore();
            }
        }
    };

    classementChart = new Chart(ctx, {
        type: "line",
        data: { datasets: chartDatasets },
        options: {
            layout: { padding: { right: paddingDroit } },
            animation: { duration: 700, easing: "easeInOutQuart" },
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: "index", intersect: false },
            plugins: { 
                legend: { 
                    display: true, 
                    position: 'top',
                    labels: { 
                        color: isDark ? '#c9d1d9' : '#1c1e21', 
                        boxWidth: 12, 
                        usePointStyle: true,
                        generateLabels: (chart) => {
                            const labels = Chart.defaults.plugins.legend.labels.generateLabels(chart);
                            labels.forEach(lbl => { lbl.text = `#${lbl.datasetIndex + 1} ${lbl.text}`; });
                            return labels;
                        }
                    }
                },
                tooltip: {
                    enabled: false,
                    external: function(context) {
                        const {chart, tooltip} = context;
                        let tooltipEl = chart.canvas.parentNode.querySelector('div.classement-tooltip');
                        if (!tooltipEl) {
                            tooltipEl = document.createElement('div');
                            tooltipEl.className = 'classement-tooltip';
                            tooltipEl.style.pointerEvents = 'none';
                            tooltipEl.style.position = 'absolute';
                            tooltipEl.style.transform = 'translate(-50%, -100%)';
                            tooltipEl.style.transition = 'all .1s ease';
                            tooltipEl.style.borderRadius = '6px';
                            tooltipEl.style.padding = '12px';
                            tooltipEl.style.fontFamily = "'Share Tech Mono', monospace";
                            tooltipEl.style.zIndex = 100;
                            chart.canvas.parentNode.appendChild(tooltipEl);
                            chart.canvas.parentNode.style.position = 'relative';
                        }
                        
                        const isDark = document.body.classList.contains('dark');
                        tooltipEl.style.background = isDark ? 'rgba(30, 45, 61, 0.95)' : 'rgba(255, 255, 255, 0.95)';
                        tooltipEl.style.color = isDark ? '#c9d1d9' : '#1c1e21';
                        tooltipEl.style.border = '1px solid ' + (isDark ? '#3d4068' : '#e1e4e8');
                        tooltipEl.style.boxShadow = isDark ? '0 4px 12px rgba(0,0,0,0.5)' : '0 4px 12px rgba(0,0,0,0.15)';

                        if (tooltip.opacity === 0) {
                            tooltipEl.style.opacity = 0;
                            return;
                        }

                        if (tooltip.body) {
                            const titleLines = tooltip.title || [];
                            let innerHtml = '<div style="margin-bottom: 12px; font-size: 12px; color: ' + (isDark ? '#9fa3ba' : '#606770') + '; text-align: center; text-transform: uppercase; letter-spacing: 1px;">';
                            titleLines.forEach(title => { innerHtml += title; });
                            innerHtml += '</div><div style="display: flex; flex-direction: column; gap: 8px;">';

                            const dataPoints = tooltip.dataPoints;
                            // Trier par valeur décroissante pour afficher le plus riche en haut
                            const sortedPoints = [...dataPoints].sort((a, b) => b.parsed.y - a.parsed.y);

                            sortedPoints.forEach((dp) => {
                                const color = dp.dataset.borderColor;
                                const username = dp.dataset.label;
                                const val = formatNum(dp.parsed.y) + ' $';
                                const bgColor = color + '26'; // Ajoute 15% d'opacité à la couleur hexadécimale

                                innerHtml += `
                                    <div style="display: flex; justify-content: space-between; align-items: center; gap: 24px;">
                                        <span style="background: ${bgColor}; color: ${color}; border: 1px solid ${color}40; padding: 3px 8px; border-radius: 4px; font-size: 12px; font-weight: bold; letter-spacing: 0.5px;">
                                            ${username}
                                        </span>
                                        <span style="font-size: 14px; font-weight: bold; font-family: 'Courier New', monospace;">${val}</span>
                                    </div>
                                `;
                            });
                            innerHtml += '</div>';
                            tooltipEl.innerHTML = innerHtml;
                        }

                        const {offsetLeft: positionX, offsetTop: positionY} = chart.canvas;
                        const tooltipWidth = tooltipEl.offsetWidth;
                        const chartWidth = chart.canvas.offsetWidth;
                        
                        let left = tooltip.caretX;
                        if (left < tooltipWidth / 2) left = tooltipWidth / 2;
                        if (left > chartWidth - tooltipWidth / 2) left = chartWidth - tooltipWidth / 2;

                        tooltipEl.style.opacity = 1;
                        tooltipEl.style.left = positionX + left + 'px';
                        
                        // Éviter que l'infobulle ne sorte par le haut
                        let top = positionY + tooltip.caretY - 12;
                        if (top - tooltipEl.offsetHeight < 0) {
                            tooltipEl.style.transform = 'translate(-50%, 0)';
                            top = positionY + tooltip.caretY + 12;
                        } else {
                            tooltipEl.style.transform = 'translate(-50%, -100%)';
                        }
                        tooltipEl.style.top = top + 'px';
                    },
                    callbacks: {
                        title: (tooltipItems) => {
                            const idx = Math.round(tooltipItems[0].parsed.x);
                            return (idx >= 0 && idx < classementLabels.length) ? classementLabels[idx] : '';
                        },
                    }
                }
            },
            scales: {
                x: {
                    type: 'linear', min: 0, max: N - 1, grid: { display: false },
                    ticks: { maxRotation: 35, minRotation: 35, count: nbLabelsX,
                        callback: (value) => {
                            const idx = Math.round(value);
                            return (idx >= 0 && idx < classementLabels.length) ? classementLabels[idx] : '';
                        }
                    }
                },
                y: {
                    position: 'left', grace: "5%", min: finalYMin, max: finalYMax,
                    grid: { color: gridColor, lineWidth: 0.5 },
                    afterFit: (scale) => { scale.width = largeurAxeY; }
                }
            }
        },
        plugins: [crosshairPlugin]
    });
}

function chargerGraphiquePortefeuille(username) {
    const url = username ? `/api/portefeuille/chart?username=${encodeURIComponent(username)}` : `/api/portefeuille/chart`;
    fetch(url)
        .then(r => r.json())
        .then(data => {
            if (!data || !data.length) return;
            const labels = data.map(d => d.time);
            const prices = data.map(d => d.price);
            dessinerGraphiquePortefeuille(labels, prices);
        })
        .catch(err => console.error("Erreur graphe portf:", err));
}

function dessinerGraphiquePortefeuille(labels, prices) {
    const ctx = document.getElementById("portfolio-chart").getContext("2d");
    const isDark = document.body.classList.contains("dark");
    
    const startPrice = prices[0] || 100000;
    const currentPrice = prices[prices.length - 1] || 100000;
    const isPos = currentPrice >= startPrice;

    // Vert si bénéfice global, rouge si perte !
    const curveColor = isPos ? (isDark ? "#2ecc71" : "#27ae60") : (isDark ? "#e74c3c" : "#c0392b");
    const fillColor  = isPos ? "rgba(46,204,113,0.15)" : "rgba(231,76,60,0.15)";
    const gridColor  = isDark ? "rgba(255,255,255,0.06)" : "rgba(0,0,0,0.06)";
    
    const N = prices.length;
    const xyData = prices.map((p, i) => ({ x: i, y: p }));

    const isMobile = window.innerWidth <= 700;
    const paddingDroit = isMobile ? 22 : 35;
    const largeurAxeY = isMobile ? 50 : 65;
    const nbLabelsX = isMobile ? 5 : 7;

    const bounds = calculerBornesY(prices);

    if (portfolioChart) portfolioChart.destroy();

    const crosshairPlugin = {
        id: 'crosshair',
        afterDraw: chart => {
            if (chart.tooltip?._active?.length) {
                const activePoint = chart.tooltip._active[0];
                const ctx = chart.ctx;
                const x = activePoint.element.x;
                const y = activePoint.element.y;
                const topY = chart.scales.y.top;
                const bottomY = chart.scales.y.bottom;
                const leftX = chart.scales.x.left;
                const rightX = chart.scales.x.right;

                ctx.save();
                ctx.beginPath();
                ctx.moveTo(x, topY); ctx.lineTo(x, bottomY);
                ctx.moveTo(leftX, y); ctx.lineTo(rightX, y);
                ctx.lineWidth = 1;
                ctx.strokeStyle = isDark ? 'rgba(255, 255, 255, 0.15)' : 'rgba(0, 0, 0, 0.15)';
                ctx.setLineDash([4, 4]);
                ctx.stroke();
                ctx.restore();
            }
        }
    };

    portfolioChart = new Chart(ctx, {
        type: "line",
        data: {
            datasets: [{
                label: "Valeur",
                data: xyData,
                borderColor: curveColor,
                backgroundColor: fillColor,
                borderWidth: isDark ? 2.5 : 2,
                pointRadius: 0,
                pointHoverRadius: 6,
                fill: true,
                tension: 0.1
            }]
        },
        options: {
            layout: { padding: { right: paddingDroit } },
            animation: { duration: 700, easing: "easeInOutQuart" },
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: "index", intersect: false },
            plugins: { 
                legend: { display: false },
                tooltip: {
                    backgroundColor: isDark ? 'rgba(30, 45, 61, 0.95)' : 'rgba(255, 255, 255, 0.95)',
                    titleColor: isDark ? '#c9d1d9' : '#1c1e21',
                    bodyColor: curveColor,
                    borderColor: isDark ? '#3d4068' : '#e1e4e8',
                    borderWidth: 1, padding: 12, displayColors: false,
                    titleFont: { family: "'Share Tech Mono', monospace", size: 12 },
                    bodyFont: { family: "'Share Tech Mono', monospace", size: 14, weight: 'bold' },
                    callbacks: {
                        title: (tooltipItems) => {
                            const idx = Math.round(tooltipItems[0].parsed.x);
                            return (idx >= 0 && idx < labels.length) ? labels[idx] : '';
                        },
                        label: (context) => { return formatNum(context.parsed.y) + ' $'; }
                    }
                }
            },
            scales: {
                x: {
                    type: 'linear', min: 0, max: N - 1, grid: { display: false },
                    ticks: { maxRotation: 35, minRotation: 35, count: nbLabelsX,
                        callback: (value) => {
                            const idx = Math.round(value);
                            return (idx >= 0 && idx < labels.length) ? labels[idx] : '';
                        }
                    }
                },
                y: {
                    position: 'left', grace: "5%", min: bounds.min, max: bounds.max,
                    grid: { color: gridColor, lineWidth: 0.5 },
                    afterFit: (scale) => { scale.width = largeurAxeY; }
                }
            }
        },
        plugins: [crosshairPlugin]
    });
}

lierToucheEntree("user", "pass");
lierToucheEntree("reg-user", "reg-pass");

verifierSession();