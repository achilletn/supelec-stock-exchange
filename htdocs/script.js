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
    if (!raw) return;
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
let symboleActuel = "";
let periodeActuelle = "24h";
let tickCount = 0;
let prixActuel = 0;

const tickHandlers = new Set();
const syncClockHandles = {};
const GRAPH_REFRESH_RATE = 1;
const cacheGraphique = {};

let CYCLE_MS = 65000;
let serverLastUpdateMs = 0;   // horodatage Unix (ms) de la dernière update serveur
let masterTickTimeout = null;
let masterTickStarted = false;
let resyncInterval = null;

const formateurDevise = new Intl.NumberFormat('fr-FR', {
    style: 'currency',
    currency: 'USD',
    currencyDisplay: 'narrowSymbol', // Force le symbole court ($) au lieu de $US
    minimumFractionDigits: 2,
    maximumFractionDigits: 2
});

function fetchSyncStatus() {
    return fetch('/api/sync-status')
        .then(r => r.json())
        .then(d => {
            CYCLE_MS = d.cycle_ms || 65000;
            if (d.last_update_ms > 0) serverLastUpdateMs = d.last_update_ms;
        });
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
    setInterval(actualiserDashboard, 30000);
    startMasterTick();

    restaurerUI();
}

function switchTab(tabName) {
    localStorage.setItem("dernierTab", tabName);
    sauvegarderUI();
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
    else if (tabName === "portefeuille") { demarrerBouclePortefeuille(); }
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

function actualiserTableauMarche() {
    const tbody = document.getElementById("market-body");
    if (!tbody) return;

    fetch("/api/marche")
        .then(res => res.json())
        .then(data => {
            if (data.length === 0) {
            // ...
                tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;padding:20px;color:#f39c12;">Synchronisation en cours... 📡</td></tr>`;
                return;
            }
            tbody.innerHTML = "";
            data.forEach(action => {
                const tr = document.createElement("tr");
                const variColor = action.variation_24h >= 0 ? "#2ecc71" : "#e74c3c";
                const variSign = action.variation_24h >= 0 ? "+" : "";
                tr.innerHTML = `
                    <td><a href="#" class="lien-actif" onclick="voirDetail('${action.symbol}');return false;"><strong>${action.symbol}</strong></a></td>
                    <td class="price">${action.price.toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} $</td>
                    <td style="font-family:'Courier New',monospace; font-weight:bold; color:${variColor};">
                        ${variSign}${action.variation_24h.toFixed(2)}%
                    </td>
                    <td style="display:flex; gap:6px; justify-content:center;">
                        <button class="btn-achat" style="padding:4px 10px; font-size:0.85em;" onclick="achatDepuisMarche('${action.symbol}', ${action.price})">Acheter</button>
                        <button class="btn-vente" style="padding:4px 10px; font-size:0.85em;" onclick="venteDepuisMarche('${action.symbol}', ${action.price})">Vendre</button>
                        <button class="btn-detail" onclick="voirDetail('${action.symbol}')">Détails</button>
                    </td>`;
                tbody.appendChild(tr);

                if (action.symbol === symboleActuel) {
                    prixActuel = action.price;
                    const elPrix = document.getElementById("detail-prix");
                    if (elPrix) {
                        elPrix.innerText = action.price.toLocaleString("fr-FR", {
                            minimumFractionDigits: 2,
                            maximumFractionDigits: 2
                        }) + " $";
                    }
                    majPreviewTrade();
                }
            });
        })
        .catch(() => {
            tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;color:red;">Erreur réseau ❌</td></tr>`;
        });
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
                    <td style="font-weight:bold;">${joueur.valeur_totale.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} $</td>
                    <td style="color:${color24h};font-weight:bold;">
                        ${joueur.pnl_24h_usd ?? "—"} $<br><span style="font-size:0.85em;">${joueur.pnl_24h_pct ?? "—"}</span>
                    </td>
                    <td style="color:${colorTotal};font-weight:bold;">
                        ${joueur.pnl_total_usd ?? "—"} $<br><span style="font-size:0.85em;">${joueur.pnl}</span>
                    </td>`;
                tbody.appendChild(tr);
            });
        });
}

function appliquerDonneesDashboard(data) {
    document.getElementById("dash-username").innerText = data.username.toUpperCase();
    document.getElementById("dash-wallet").innerText = formateurDevise.format(data.valeur_totale);

    const pnlEl = document.getElementById("dash-pnl");
    pnlEl.innerText = `${data.pnl} (${data.pnl_total_usd ?? "—"} $)`;
    pnlEl.style.color = data.pnl.startsWith("+") && data.pnl !== "+0.00%" ? "#2ecc71" : data.pnl.startsWith("-") ? "#e74c3c" : "#888";

    const usdEl = document.getElementById("dash-usd");
    if (usdEl && data.usd !== undefined) {
        usdEl.innerText = formateurDevise.format(data.usd);
    }
}

function actualiserDashboard() {
    if (!utilisateurActuel) return;
    fetch("/api/dashboard").then(res => res.json()).then(data => appliquerDonneesDashboard(data));
}

setInterval(() => {
    document.getElementById("dash-clock").innerText = new Date().toLocaleTimeString();
}, 1000);

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
            <td><a href="#" class="lien-actif" onclick="voirDetail('${ligne.symbole}');return false;"><strong>${ligne.symbole}</strong></a></td>
            <td style="font-family:'Courier New',monospace;">${ligne.quantite.toLocaleString("en-US", { maximumFractionDigits: 6 })}</td>
            <td style="font-family:'Courier New',monospace;">${ligne.prix_unitaire > 0 ? ligne.prix_unitaire.toLocaleString("en-US", { minimumFractionDigits: 2 }) + " $" : "—"}</td>
            <td style="font-family:'Courier New',monospace; font-weight:bold;">${formateurDevise.format(ligne.valeur)}</td>
            <td style="font-family:'Courier New',monospace; color:${color24h};">${ligne.variation_24h ?? "—"}</td>
            <td style="font-family:'Courier New',monospace; color:${colorAlltime};">${ligne.pnl_alltime_pct ?? "—"}<br><span style="font-size:0.85em;">${ligne.pnl_alltime_usd ?? "—"} $</span></td>
            ${!portfolioJoueurCible ? `<td><button class="btn-vente" style="padding:4px 12px; font-size:0.85em;" onclick="vendreDepuisPortefeuille('${ligne.symbole}', ${ligne.prix_unitaire})">Vendre</button></td>` : '<td></td>'}`;
        tbody.appendChild(tr);
    });
}

function chargerPortefeuille() {
    const titre = document.getElementById("portefeuille-titre");
    const btnRetour = document.getElementById("btn-retour-classement");
    const histTable = document.getElementById("portfolio-history-table");

    const joueurSummary = document.getElementById("portfolio-joueur-summary");

    if (portfolioJoueurCible) {
        // Vue d'un autre joueur
        if (titre) titre.innerText = `Portefeuille de ${portfolioJoueurCible}`;
        if (btnRetour) btnRetour.style.display = "inline-block";
        if (joueurSummary) joueurSummary.style.display = "flex";
        if (histTable) histTable.style.display = "";

        fetch(`/api/portefeuille/joueur?username=${encodeURIComponent(portfolioJoueurCible)}`)
            .then(res => res.json())
            .then(data => {
                // Résumé
                const pjUsd = document.getElementById("pj-usd");
                const pjPnl = document.getElementById("pj-pnltotal");
                if (pjUsd) pjUsd.innerText = formateurDevise.format(data.usd);
                if (pjPnl && data.pnl_total_pct) {
                    const isPos = data.pnl_total_pct.startsWith("+") && data.pnl_total_pct !== "+0.00%";
                    pjPnl.innerText = `${data.pnl_total_pct}  (${data.pnl_total_usd} $)`;
                    pjPnl.style.color = isPos ? "#2ecc71" : data.pnl_total_pct.startsWith("-") ? "#e74c3c" : "#888";
                }
                // Historique
                afficherHistorique(data.historique || []);
                // Actifs
                afficherActifs(data.actifs || []);
            });
        return;
    }

    // Vue propre portefeuille
    if (titre) titre.innerText = "Mon portefeuille";
    if (btnRetour) btnRetour.style.display = "none";
    if (joueurSummary) joueurSummary.style.display = "none";
    if (histTable) histTable.style.display = "";

    // Actifs
    fetch("/api/portefeuille")
        .then(res => res.json())
        .then(data => afficherActifs(data.actifs || []));

    // Historique journalier (propre portefeuille)
    fetch("/api/portefeuille/historique")
        .then(res => res.json())
        .then(rows => afficherHistorique(rows));
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

function startSyncClock(canvasId) {
    cancelSyncClock(canvasId);
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    let resetting = false;

    const localStart = Date.now();
    function draw() {
        const elapsed = serverLastUpdateMs > 0
            ? (Date.now() - serverLastUpdateMs) % CYCLE_MS
            : (Date.now() - localStart) % CYCLE_MS;   // fallback local si pas encore sync
        const progress = elapsed / CYCLE_MS;
        ctx.clearRect(0, 0, 28, 28);

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
    document.getElementById("detail-titre").innerText = symbol;
    document.getElementById("trade-message").innerText = "";
    document.getElementById("modal-trade-bg").classList.remove("open");
    document.getElementById("panneau-detail-bg").classList.add("open");
    chargerGraphique(symboleActuel, periodeActuelle);
    chargerStats(symbol);
    sauvegarderUI();
}

function chargerStats(symbol) {
    const fmt = (v, decimals = 2) => v != null ? v.toLocaleString("en-US", { minimumFractionDigits: decimals, maximumFractionDigits: decimals }) + " $" : "—";
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
    document.getElementById("panneau-detail-bg").classList.remove("open");
    if (graphiqueActif) { graphiqueActif.destroy(); graphiqueActif = null; }
    symboleActuel = "";
    sauvegarderUI();
}

function fermerDetailOverlay(event) {
    if (event.target === document.getElementById("panneau-detail-bg")) fermerDetail();
}

function changerFenetre(periode) {
    periodeActuelle = periode;
    document.querySelectorAll(".btn-time").forEach(b => b.classList.remove("active"));
    document.getElementById(`btn-${periode}`).classList.add("active");
    chargerGraphique(symboleActuel, periodeActuelle);
}

function getCacheKey(symbol, periode) { return `${symbol}_${periode}`; }

function chargerGraphique(symbol, periode) {
    const key = getCacheKey(symbol, periode);
    const cache = cacheGraphique[key];
    const url = cache
        ? `/api/historique?symbole=${symbol}&periode=${periode}&depuis=${encodeURIComponent(cache.lastTimestamp)}`
        : `/api/historique?symbole=${symbol}&periode=${periode}`;

    fetch(url)
        .then(res => res.json())
        .then(data => {
            if (!cache) {
                cacheGraphique[key] = {
                    labels: data.map(d => d.time),
                    prices: data.map(d => d.price),
                    lastTimestamp: data.length ? data[data.length - 1].timestamp : ""
                };
            } else if (data.length > 0) {
                cache.labels.push(...data.map(d => d.time));
                cache.prices.push(...data.map(d => d.price));
                cache.lastTimestamp = data[data.length - 1].timestamp;
            }

            const c = cacheGraphique[key];
            if (!c.prices.length) return;

            const dernierPrix = c.prices[c.prices.length - 1];
            document.getElementById("detail-prix").innerText = dernierPrix.toLocaleString("fr-FR", {minimumFractionDigits: 2}) + " $";
            prixActuel = dernierPrix;
            dessinerGraphique(c.labels, c.prices);
            majPreviewTrade();
        })
        .catch(err => console.error("Erreur graphique:", err));
}

function dessinerGraphique(labels, prices) {
    const ctx = document.getElementById("graphique-actif").getContext("2d");
    if (graphiqueActif) graphiqueActif.destroy();

    graphiqueActif = new Chart(ctx, {
        type: "line",
        data: {
            labels,
            datasets: [{
                label: "Prix",
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
    document.getElementById("trade-modal-symbole").innerText = symboleActuel;
    btn.className = "trade-modal-confirm " + (action === "achat" ? "btn-achat" : "btn-vente");
    btn.innerText = action === "achat" ? "Confirmer l'achat" : "Confirmer la vente";

    const fmt = (n) => n.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    prixEl.innerText = prixActuel > 0 ? fmt(prixActuel) + " $" : "—";

    document.getElementById("prev-total-label").innerText = action === "achat" ? "Total à débiter" : "Net à recevoir";

    qteInput.value = "";
    msg.innerText = "";
    document.getElementById("prev-brut").innerText = "—";
    document.getElementById("prev-frais").innerText = "—";
    document.getElementById("prev-total").innerText = "—";

    bg.classList.add("open");
    setTimeout(() => qteInput.focus(), 50);
    sauvegarderUI();
}

function fermerModalTrade(event) {
    if (event && event.target !== document.getElementById("modal-trade-bg")) return;
    document.getElementById("modal-trade-bg").classList.remove("open");
    tradeActionCourante = null;
    sauvegarderUI();
}

function majPreviewTrade() {
    const qte = parseInt(document.getElementById("trade-modal-qte").value, 10);
    const fmt = (n) => n.toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });

    if (!qte || qte <= 0 || prixActuel <= 0) {
        document.getElementById("prev-brut").innerText = "—";
        document.getElementById("prev-frais").innerText = "—";
        document.getElementById("prev-total").innerText = "—";
        return;
    }

    const brut = qte * prixActuel;
    const frais = brut * 0.003;
    const total = tradeActionCourante === "achat" ? brut + frais : brut - frais;

    document.getElementById("prev-brut").innerText = fmt(brut) + " $";
    document.getElementById("prev-frais").innerText = fmt(frais) + " $";
    document.getElementById("prev-total").innerText = (tradeActionCourante === "achat" ? "-" : "+") + fmt(total) + " $";
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

lierToucheEntree("user", "pass");
lierToucheEntree("reg-user", "reg-pass");

verifierSession();