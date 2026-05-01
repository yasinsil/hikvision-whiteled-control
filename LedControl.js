const http = require('http');
const crypto = require('crypto');

// ===== SETTINGS =====
const NVR_IP = '192.168.1.200';

// NVR for alertstream (alertStream icin)
const NVR_USER = 'admin';
const NVR_PASS = 'NVRPASSHere';
const NVR_PORT = 80;

// Cam ID Section (LED kontrol icin)
const CAM_USER = 'admin';
const CAM_PASS = 'Camerapasshere';

// Cam ports
const LIGHT_CAMERAS = [65011, 65013];
const ALERT_URI = '/ISAPI/Event/notification/alertStream';
const LIGHT_URI = '/ISAPI/Image/channels/1/supplementLight';

const shortXml = (br) => `<SupplementLight><whiteLightBrightness>${br}</whiteLightBrightness></SupplementLight>`;

// ===== LED Settings =====
let cfg = {
    dayRadar: 100,    dayFlash: 50,   daySteady: 50,
    nightRadar: 50,   nightFlash: 4,  nightSteady: 4,
    radarMs: 300,
    flashMs: 5000,    flashInt: 500,
    steadyMs: 15000,
    dayH: 7, dayM: 30,
    nightH: 19, nightM: 30
};

function isDay() {
    const now = new Date();
    const mins = now.getHours() * 60 + now.getMinutes();
    const dayStart = cfg.dayH * 60 + cfg.dayM;
    const nightStart = cfg.nightH * 60 + cfg.nightM;
    return mins >= dayStart && mins < nightStart;
}

function getRadarBr()  { return isDay() ? cfg.dayRadar  : cfg.nightRadar; }
function getFlashBr()  { return isDay() ? cfg.dayFlash  : cfg.nightFlash; }
function getSteadyBr() { return isDay() ? cfg.daySteady : cfg.nightSteady; }
function getModeStr()  { return isDay() ? 'GUNDUZ' : 'GECE'; }

// ===== STATE =====
const nonceCaches = {};
LIGHT_CAMERAS.forEach(port => nonceCaches[port] = null);

let currentState = 'IDLE';
let steadyEndTime = 0;
let sequenceId = 0;

const keepAliveAgent = new http.Agent({ keepAlive: true, maxSockets: 10 });
let commandLock = Promise.resolve();

function executeCommandSafe(brightness) {
    commandLock = commandLock.then(async () => {
        await setBothBrightness(brightness);
    }).catch(err => console.log(`[HATA] ${err}`));
    return commandLock;
}

// ===== DIGEST AUTH =====
function md5(str) {
    return crypto.createHash('md5').update(str).digest('hex');
}

function parseDigest(header) {
    const obj = {};
    header.replace(/(\w+)="([^"]+)"/g, (_, k, v) => obj[k] = v);
    header.replace(/(\w+)=([^,\s"]+)/g, (_, k, v) => { if (!obj[k]) obj[k] = v; });
    return obj;
}

function buildAuth(method, uri, digestParams, user, pass) {
    const nc = '00000001';
    const cnonce = crypto.randomBytes(8).toString('hex');
    const ha1 = md5(`${user}:${digestParams.realm}:${pass}`);
    const ha2 = md5(`${method}:${uri}`);
    const response = md5(`${ha1}:${digestParams.nonce}:${nc}:${cnonce}:${digestParams.qop}:${ha2}`);
    return `Digest username="${user}", realm="${digestParams.realm}", nonce="${digestParams.nonce}", uri="${uri}", qop=${digestParams.qop}, nc=${nc}, cnonce="${cnonce}", response="${response}"`;
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

// ===== LED Control =====
function getRequest(port, path = LIGHT_URI) {
    return new Promise((resolve, reject) => {
        const req = http.request({ hostname: NVR_IP, port, path, method: 'GET', agent: keepAliveAgent }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: data }));
        });
        req.on('error', reject);
        req.end();
    });
}

function putRequest(port, brightness, auth) {
    const xml = shortXml(brightness);
    return new Promise((resolve, reject) => {
        const req = http.request({
            hostname: NVR_IP, port, path: LIGHT_URI, method: 'PUT', agent: keepAliveAgent,
            headers: {
                'Content-Type': 'application/xml',
                'Content-Length': Buffer.byteLength(xml),
                'Authorization': auth
            }
        }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => resolve({ status: res.statusCode, body: data }));
        });
        req.on('error', reject);
        req.write(xml);
        req.end();
    });
}

async function setBrightness(port, brightness) {
    if (nonceCaches[port]) {
        const auth = buildAuth('PUT', LIGHT_URI, nonceCaches[port], CAM_USER, CAM_PASS);
        const res = await putRequest(port, brightness, auth);
        if (res.status === 200) return;
    }

    const res1 = await getRequest(port);
    if (res1.status !== 401) throw new Error(`Port ${port}: Beklenen 401, gelen: ${res1.status}`);

    const digestParams = parseDigest(res1.headers['www-authenticate']);
    const auth = buildAuth('PUT', LIGHT_URI, digestParams, CAM_USER, CAM_PASS);

    const res2 = await putRequest(port, brightness, auth);
    if (res2.status === 200) nonceCaches[port] = digestParams;
}

async function setBothBrightness(brightness) {
    const t0 = Date.now();
    const results = await Promise.all(
        LIGHT_CAMERAS.map(async port => {
            const tStart = Date.now();
            try {
                await setBrightness(port, brightness);
                return { port, ms: Date.now() - tStart, ok: true };
            } catch (err) {
                console.log(`  HATA: Port ${port} - ${err.message}`);
                return { port, ms: Date.now() - tStart, ok: false };
            }
        })
    );
    const total = Date.now() - t0;
    const parts = results.map(r =>
        `${r.port}:${r.ok ? 'OK' : 'FAIL'}/${r.ms}ms`
    ).join(' ');
    const flag = total > 300 ? ' <<<' : '';
    console.log(`  [${brightness}%] ${parts} tot:${total}ms${flag}`);
}

// ===== SEKANS =====
async function handleTrigger(source) {
    const nowStr = new Date().toLocaleTimeString('tr-TR');
    console.log(`\n[${nowStr}] TETİK: ${source} (${getModeStr()})`);

    if (currentState === 'FLASHING') {
        console.log('  -> Flash aktif, yoksayildi.');
        return;
    }

    if (currentState === 'STEADY') {
        console.log('  -> Steady uzatildi.');
        steadyEndTime = Date.now() + cfg.steadyMs;
        return;
    }

    runSequence();
}

async function runSequence() {
    currentState = 'FLASHING';
    const currentSeqId = ++sequenceId;
    const radarBr  = getRadarBr();
    const flashBr  = getFlashBr();
    const steadyBr = getSteadyBr();

    console.log(`  [RADAR] %${radarBr} ${cfg.radarMs}ms`);
    await executeCommandSafe(radarBr);
    await sleep(cfg.radarMs);

    console.log(`  [FLASH] %${flashBr} ${cfg.flashMs / 1000}sn aralik:${cfg.flashInt}ms`);
    const flashEnd = Date.now() + cfg.flashMs;
    let toggle = false;

    while (Date.now() < flashEnd) {
        if (sequenceId !== currentSeqId) return;

        const t0 = Date.now();
        await executeCommandSafe(toggle ? flashBr : 0);
        toggle = !toggle;

        const elapsed = Date.now() - t0;
        const waitTime = cfg.flashInt - elapsed;
        if (waitTime > 0) await sleep(waitTime);
    }

    console.log(`  [SABİT] %${steadyBr} ${cfg.steadyMs / 1000}sn`);
    currentState = 'STEADY';
    steadyEndTime = Date.now() + cfg.steadyMs;
    await executeCommandSafe(steadyBr);

    while (Date.now() < steadyEndTime) {
        if (sequenceId !== currentSeqId) return;
        await sleep(500);
    }

    console.log('  [KAPAT]');
    currentState = 'IDLE';
    await executeCommandSafe(0);
}

// ===== ALERTSTREAM (NVR PORT 80) =====
function connectAlertStream() {
    console.log(`[BİLGİ] AlertStream baglaniliyor: NVR port ${NVR_PORT}...`);

    const req1 = http.request({
        hostname: NVR_IP, port: NVR_PORT, path: ALERT_URI, method: 'GET'
    }, (res1) => {
        if (res1.statusCode !== 401) { res1.resume(); return; }

        const params = parseDigest(res1.headers['www-authenticate']);
        const authHeader = buildAuth('GET', ALERT_URI, params, NVR_USER, NVR_PASS);

        const req2 = http.request({
            hostname: NVR_IP, port: NVR_PORT, path: ALERT_URI, method: 'GET',
            headers: { 'Authorization': authHeader }
        }, (res2) => {
            if (res2.statusCode !== 200) {
                console.log(`[HATA] AlertStream: ${res2.statusCode}`);
                res2.resume();
                return;
            }

            console.log(`[BİLGİ] AlertStream aktif. Hareket bekleniyor...`);
            let buffer = '';

            res2.on('data', (chunk) => {
                buffer += chunk.toString();
                if (buffer.length > 5000) buffer = buffer.slice(-2000);

                if (buffer.includes('</EventNotificationAlert>')) {
                    const typeMatch = buffer.match(/<eventType>(.*?)<\/eventType>/);
                    const stateMatch = buffer.match(/<eventState>(.*?)<\/eventState>/);
                    const channelMatch = buffer.match(/<channelID>(.*?)<\/channelID>/);

                    if (typeMatch?.[1] === 'VMD' && stateMatch?.[1] === 'active') {
                        const ch = channelMatch?.[1] || '?';
                        handleTrigger(`ch:${ch}`);
                    }
                    buffer = '';
                }
            });

            res2.on('end', () => {
                console.log('[BİLGİ] AlertStream koptu, 5sn...');
                setTimeout(connectAlertStream, 5000);
            });
        });

        req2.on('error', () => setTimeout(connectAlertStream, 5000));
        req2.end();
        res1.resume();
    });

    req1.on('error', () => setTimeout(connectAlertStream, 5000));
    req1.end();
}

// ===== WEB PANEL =====
const HTML = `<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>KameraIOT</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;max-width:500px;margin:0 auto;padding:16px;background:#0f0f1a;color:#ddd}
h1{font-size:1.3em;color:#0cf;margin-bottom:14px}
h2{font-size:.9em;color:#0cf;margin:16px 0 8px;border-bottom:1px solid #2a2a3e;padding-bottom:4px}
.r{display:flex;gap:8px;margin-bottom:6px}.r>div{flex:1}
label{display:block;font-size:.78em;color:#999;margin-bottom:2px}
input{width:100%;padding:7px;background:#1a1a2e;color:#ddd;border:1px solid #333;border-radius:5px;font-size:.9em}
input:focus{border-color:#0cf;outline:none}
.btn{width:100%;padding:11px;margin-top:10px;border:none;border-radius:5px;font-weight:bold;cursor:pointer;font-size:1em}
.save{background:#0cf;color:#000}.test{background:#fa0;color:#000}
.save:active,.test:active{opacity:.7}
#st{margin-bottom:12px;padding:10px;background:#1a1a2e;border-radius:5px;font-size:.85em;line-height:1.8}
#msg{text-align:center;color:#0f0;margin-top:8px;font-size:.85em;min-height:1.3em}
.idle{color:#666}.flash{color:#fa0}.steady{color:#0c6}
</style></head><body>
<h1>&#128249; KameraIOT</h1>
<div id="st">Yukleniyor...</div>

<h2>Gunduz Parlaklik (%)</h2>
<div class="r">
<div><label>Radar</label><input id="dR" type="number" min="0" max="100"></div>
<div><label>Flash</label><input id="dF" type="number" min="0" max="100"></div>
<div><label>Sabit</label><input id="dS" type="number" min="0" max="100"></div>
</div>

<h2>Gece Parlaklik (%)</h2>
<div class="r">
<div><label>Radar</label><input id="nR" type="number" min="0" max="100"></div>
<div><label>Flash</label><input id="nF" type="number" min="0" max="100"></div>
<div><label>Sabit</label><input id="nS" type="number" min="0" max="100"></div>
</div>

<h2>Sure (ms)</h2>
<div class="r">
<div><label>Radar</label><input id="rMs" type="number" min="50" max="5000"></div>
<div><label>Flash Toplam</label><input id="fMs" type="number" min="500" max="60000"></div>
</div>
<div class="r">
<div><label>Flash Aralik</label><input id="fInt" type="number" min="100" max="2000"></div>
<div><label>Sabit Isik</label><input id="sMs" type="number" min="1000" max="120000"></div>
</div>

<h2>Gunduz / Gece Saatleri</h2>
<div class="r">
<div><label>Gunduz Baslangic</label><input id="dT" type="time"></div>
<div><label>Gece Baslangic</label><input id="nT" type="time"></div>
</div>

<button class="btn save" onclick="save()">KAYDET</button>
<button class="btn test" onclick="test()">TEST (Manuel Tetikle)</button>
<div id="msg"></div>

<script>
const $=id=>document.getElementById(id);
const pad=n=>String(n).padStart(2,'0');
async function load(){
  try{
    const d=await(await fetch('/api/cfg')).json();
    $('dR').value=d.dayRadar;$('dF').value=d.dayFlash;$('dS').value=d.daySteady;
    $('nR').value=d.nightRadar;$('nF').value=d.nightFlash;$('nS').value=d.nightSteady;
    $('rMs').value=d.radarMs;$('fMs').value=d.flashMs;$('fInt').value=d.flashInt;$('sMs').value=d.steadyMs;
    $('dT').value=pad(d.dayH)+':'+pad(d.dayM);
    $('nT').value=pad(d.nightH)+':'+pad(d.nightM);
  }catch(e){$('msg').textContent='Hata'}
}
async function save(){
  const dt=$('dT').value.split(':'),nt=$('nT').value.split(':');
  const p={dayRadar:$('dR').value,dayFlash:$('dF').value,daySteady:$('dS').value,
    nightRadar:$('nR').value,nightFlash:$('nF').value,nightSteady:$('nS').value,
    radarMs:$('rMs').value,flashMs:$('fMs').value,flashInt:$('fInt').value,steadyMs:$('sMs').value,
    dayH:dt[0],dayM:dt[1],nightH:nt[0],nightM:nt[1]};
  try{
    const r=await(await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)})).json();
    $('msg').textContent=r.ok?'Kaydedildi!':'Hata!';
    setTimeout(()=>$('msg').textContent='',2500);
  }catch(e){$('msg').textContent='Hata!'}
}
async function test(){
  try{await fetch('/api/test');$('msg').textContent='Tetiklendi!';
    setTimeout(()=>$('msg').textContent='',2500)}catch(e){$('msg').textContent='Hata!'}
}
async function status(){
  try{
    const d=await(await fetch('/api/st')).json();
    const cls={IDLE:'idle',FLASHING:'flash',STEADY:'steady'};
    const labels={IDLE:'BEKLEMEDE',FLASHING:'FLASH',STEADY:'SABIT'};
    let h='<span class="'+cls[d.state]+'"><b>'+labels[d.state]+'</b></span>';
    h+=' &middot; '+(d.day?'&#9728; Gunduz':'&#9790; Gece');
    h+='<br>Radar:'+(d.day?d.cfg.dayRadar:d.cfg.nightRadar)+'%';
    h+=' Flash:'+(d.day?d.cfg.dayFlash:d.cfg.nightFlash)+'%';
    h+=' Sabit:'+(d.day?d.cfg.daySteady:d.cfg.nightSteady)+'%';
    $('st').innerHTML=h;
  }catch(e){}
}
load();status();setInterval(status,3000);
</script></body></html>`;

function startWebServer() {
    const server = http.createServer((req, res) => {
        const url = req.url;

        if (url === '/' && req.method === 'GET') {
            res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
            res.end(HTML);

        } else if (url === '/api/cfg' && req.method === 'GET') {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify(cfg));

        } else if (url === '/api/save' && req.method === 'POST') {
            let body = '';
            req.on('data', c => body += c);
            req.on('end', () => {
                try {
                    const d = JSON.parse(body);
                    cfg.dayRadar   = parseInt(d.dayRadar)   || cfg.dayRadar;
                    cfg.dayFlash   = parseInt(d.dayFlash)    || cfg.dayFlash;
                    cfg.daySteady  = parseInt(d.daySteady)   || cfg.daySteady;
                    cfg.nightRadar = parseInt(d.nightRadar)  || cfg.nightRadar;
                    cfg.nightFlash = parseInt(d.nightFlash)  || cfg.nightFlash;
                    cfg.nightSteady= parseInt(d.nightSteady) || cfg.nightSteady;
                    cfg.radarMs    = parseInt(d.radarMs)     || cfg.radarMs;
                    cfg.flashMs    = parseInt(d.flashMs)     || cfg.flashMs;
                    cfg.flashInt   = parseInt(d.flashInt)    || cfg.flashInt;
                    cfg.steadyMs   = parseInt(d.steadyMs)    || cfg.steadyMs;
                    cfg.dayH       = parseInt(d.dayH);
                    cfg.dayM       = parseInt(d.dayM);
                    cfg.nightH     = parseInt(d.nightH);
                    cfg.nightM     = parseInt(d.nightM);
                    console.log(`[AYAR] ${getModeStr()} | Radar:${getRadarBr()}% Flash:${getFlashBr()}% Sabit:${getSteadyBr()}%`);
                    res.writeHead(200, { 'Content-Type': 'application/json' });
                    res.end('{"ok":true}');
                } catch (e) {
                    res.writeHead(400); res.end('{"ok":false}');
                }
            });

        } else if (url === '/api/test' && req.method === 'GET') {
            handleTrigger('Web Panel');
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end('{"ok":true}');

        } else if (url === '/api/st' && req.method === 'GET') {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ state: currentState, day: isDay(), cfg }));

        } else {
            res.writeHead(404); res.end('Not Found');
        }
    });

    server.listen(3000, () => {
        console.log(`[BİLGİ] Web panel: http://localhost:3000\n`);
    });
}

// ===== BASLAT =====
async function init() {
    console.log('=== HIKVISION AKILLI ISIK SISTEMI ===');
    console.log(`AlertStream: NVR port ${NVR_PORT} (${NVR_USER})`);
    console.log(`LED Kontrol: Virtual Host ${LIGHT_CAMERAS.join(', ')} (${CAM_USER})`);
    console.log(`Mod: ${getModeStr()} | Radar:${getRadarBr()}% Flash:${getFlashBr()}% Sabit:${getSteadyBr()}%\n`);

    console.log('Nonce onbellekleniyor...');
    await executeCommandSafe(0);
    console.log('Hazir.\n');

    connectAlertStream();
    startWebServer();
}

init();