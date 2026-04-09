let utilisateurActuel = "";
let masterTickInterval = null;
let cacheClassement = {};
let graphiqueActif = null;
let symboleActuel = "";
let periodeActuelle = "24h";
let lastTickTime = performance.now();
let tickCount = 0;
let prixActuel = 0;

const tickHandlers = new Set();
const syncClockHandles = {};
const GRAPH_REFRESH_RATE = 1;
const cacheGraphique = {};

let CYCLE_MS = 65000;         // mis à jour depuis /api/sync-status
let masterTickTimeout = null; // timeout du premier tick aligné
let masterTickStarted = false;

const formateurDevise = new Intl.NumberFormat('fr-FR', {
    style: 'currency',
    currency: 'USD',
    currencyDisplay: 'narrowSymbol', // Force le symbole court ($) au lieu de $US
    minimumFractionDigits: 2,
    maximumFractionDigits: 2
});

function startMasterTick() {
    if (masterTickStarted) return;
    masterTickStarted = true;

    const tick = () => {
        lastTickTime = performance.now();
        tickCount++;
        tickHandlers.forEach(fn => fn(tickCount));
    };

    fetch('/api/sync-status')
        .then(r => r.json())
        .then(d => {
            CYCLE_MS = d.cycle_ms || 65000;
            const elapsed = d.last_update_ms > 0 ? Date.now() - d.last_update_ms : 0;
            const remaining = CYCLE_MS - (elapsed % CYCLE_MS);

            // Positionne le cercle au bon endroit dans le cycle serveur
            lastTickTime = performance.now() - (elapsed % CYCLE_MS);

            // Premier tick exactement quand le serveur se met à jour
            masterTickTimeout = setTimeout(() => {
                masterTickTimeout = null;
                tick();
                masterTickInterval = setInterval(tick, CYCLE_MS);
            }, remaining);
        })
        .catch(() => {
            // Fallback si l'endpoint est indisponible
            masterTickInterval = setInterval(tick, CYCLE_MS);
        });
}

function stopMasterTick() {
    if (masterTickTimeout) { clearTimeout(masterTickTimeout); masterTickTimeout = null; }
    clearInterval(masterTickInterval);
    masterTickInterval = null;
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
        tickHandlers.clear();
        stopMasterTick();
        location.reload();
    });
}

function demarrerJeu() {
    document.getElementById("zone-login").style.display = "none";
    document.getElementById("zone-jeu").style.display = "block";

    tickHandlers.add(actualiserDashboard);
    actualiserDashboard();
    startMasterTick();

    switchTab(localStorage.getItem("dernierTab") || "marche");
}

function switchTab(tabName) {
    localStorage.setItem("dernierTab", tabName);
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
    else if (tabName === "portefeuille") demarrerBouclePortefeuille();
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
            console.log("Données reçues du serveur :", data); // <--- DEBUG
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
                    <td><strong>${action.symbol}</strong></td>
                    <td class="price">${action.price.toLocaleString("fr-FR", { minimumFractionDigits: 2, maximumFractionDigits: 2 })} $</td>
                    <td style="font-family:'Courier New',monospace; font-weight:bold; color:${variColor};">
                        ${variSign}${action.variation_24h.toFixed(2)}%
                    </td>
                    <td><button class="btn-detail" onclick="voirDetail('${action.symbol}')">Détails</button></td>`;
                tbody.appendChild(tr);
                
                // 2. NOUVEAU : Si cet actif est celui ouvert dans le détail, on met à jour le prix Spot
                if (action.symbol === symboleActuel) {
                    prixActuel = action.price; // <-- Ajout de cette ligne
                    const elPrix = document.getElementById("detail-prix");
                    if (elPrix) {
                        elPrix.innerText = action.price.toLocaleString("fr-FR", { 
                            minimumFractionDigits: 2, 
                            maximumFractionDigits: 2 
                        }) + " $";
                    }
                    calculerEstimation(); // <-- Ajout de cette ligne pour actualiser en temps réel
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
                    <td><strong>${joueur.username}</strong></td>
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

    const pnl24El = document.getElementById("dash-pnl24h");
    if (pnl24El && data.pnl_24h_pct !== undefined) {
        const isPos = data.pnl_24h_pct.startsWith("+") && data.pnl_24h_pct !== "+0.00%";
        pnl24El.innerText = `${data.pnl_24h_pct} (${data.pnl_24h_usd} $)`;
        pnl24El.style.color = isPos ? "#2ecc71" : data.pnl_24h_pct.startsWith("-") ? "#e74c3c" : "#888";
    }
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

function demarrerBouclePortefeuille() {
    tickHandlers.add(chargerPortefeuille);
    chargerPortefeuille();
    startSyncClock("sync-clock-portefeuille");
}

function arreterBouclePortefeuille() {
    tickHandlers.delete(chargerPortefeuille);
    cancelSyncClock("sync-clock-portefeuille");
}

function chargerPortefeuille() {
    fetch("/api/portefeuille")
        .then(res => res.json())
        .then(data => {
            const tbody = document.getElementById("portfolio-body");
            if (!tbody) return;
            tbody.innerHTML = "";
            data.forEach(ligne => {
                const tr = document.createElement("tr");
                tr.innerHTML = `
                    <td><strong>${ligne.symbole}</strong></td>
                    <td style="font-family:'Courier New',monospace;">${ligne.quantite.toLocaleString("en-US", { maximumFractionDigits: 6 })}</td>
                    <td style="font-family:'Courier New',monospace;">${ligne.prix_unitaire > 0 ? ligne.prix_unitaire.toLocaleString("en-US", { minimumFractionDigits: 2 }) + " $" : "—"}</td>
                    <td style="font-family:'Courier New',monospace; font-weight:bold;">${formateurDevise.format(ligne.valeur)}</td>`;
                tbody.appendChild(tr);
            });
        });
}

function startSyncClock(canvasId) {
    cancelSyncClock(canvasId);
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const TICK = CYCLE_MS;
    let resetting = false;

    function draw(now) {
        const elapsed = (now - lastTickTime) % TICK;
        const progress = elapsed / TICK;
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
    
    document.getElementById("trade-quantite").value = ""; 
    const estEl = document.getElementById("trade-estimation");
    if(estEl) estEl.style.display = "none"; 
    
    document.getElementById("panneau-detail").style.display = "block";
    chargerGraphique(symboleActuel, periodeActuelle);
}

function fermerDetail() {
    document.getElementById("panneau-detail").style.display = "none";
    if (graphiqueActif) { graphiqueActif.destroy(); graphiqueActif = null; }
    symboleActuel = "";
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

            document.getElementById("detail-prix").innerText = c.prices[c.prices.length - 1].toLocaleString("fr-FR", {minimumFractionDigits: 2}) + " $";
            dessinerGraphique(c.labels, c.prices);
            calculerEstimation();
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

function passerOrdre(action) {
    const quantite = document.getElementById("trade-quantite").value;
    const msgBox = document.getElementById("trade-message");

    if (!quantite || quantite <= 0) {
        msgBox.innerText = "Veuillez entrer une quantité valide.";
        msgBox.style.color = "red";
        return;
    }

    msgBox.innerText = "Transaction en cours... ⏳";
    msgBox.style.color = "orange";

    const params = new URLSearchParams();
    params.append("symbole", symboleActuel);
    params.append("action", action);
    params.append("quantite", quantite);

    fetch("/api/trade", { method: "POST", body: params })
        .then(async res => {
            const data = await res.json();
            if (!res.ok) throw new Error(data.erreur || "Erreur de transaction");
            msgBox.innerText = `${data.message} ✅`;
            msgBox.style.color = "green";
            document.getElementById("trade-quantite").value = "";
            actualiserDashboard();
        })
        .catch(err => {
            msgBox.innerText = `${err.message} ❌`;
            msgBox.style.color = "red";
        });
}

function calculerEstimation() {
    const inputEl = document.getElementById("trade-quantite");
    const estimationEl = document.getElementById("trade-estimation");
    if (!inputEl || !estimationEl) return;

    const quantite = parseFloat(inputEl.value);
    if (isNaN(quantite) || quantite <= 0 || prixActuel <= 0) {
        estimationEl.style.display = "none";
        return;
    }

    const volume = quantite * prixActuel;
    const frais = volume * 0.005; // 0.5%
    const totalAchat = volume + frais;
    const totalVente = volume - frais;

    estimationEl.style.display = "block";
    estimationEl.innerHTML = `
        <div style="display: flex; justify-content: space-between; margin-bottom: 4px; color: #555;">
            <span>Coût brut :</span> <strong>${volume.toLocaleString("en-US", {minimumFractionDigits:2, maximumFractionDigits:2})} $</strong>
        </div>
        <div style="display: flex; justify-content: space-between; margin-bottom: 8px; color: #e74c3c;">
            <span>Frais (0.5%) :</span> <strong>${frais.toLocaleString("en-US", {minimumFractionDigits:2, maximumFractionDigits:2})} $</strong>
        </div>
        <div style="border-top: 1px solid #ccc; padding-top: 8px;">
            <div style="display: flex; justify-content: space-between; margin-bottom: 4px;">
                <span>Coût Total si <strong style="color:#2ecc71;">Achat</strong> :</span> <strong>-${totalAchat.toLocaleString("en-US", {minimumFractionDigits:2, maximumFractionDigits:2})} $</strong>
            </div>
            <div style="display: flex; justify-content: space-between;">
                <span>Gain Net si <strong style="color:#e74c3c;">Vente</strong> :</span> <strong>+${totalVente.toLocaleString("en-US", {minimumFractionDigits:2, maximumFractionDigits:2})} $</strong>
            </div>
        </div>
    `;
}

// Lier l'événement de saisie
document.getElementById("trade-quantite").addEventListener("input", calculerEstimation);

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