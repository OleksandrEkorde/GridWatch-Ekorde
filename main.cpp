/*
  GridWatch (ESP32 + LCD1602 + Web UI)

  TODO #1: додати опцію "Buzzer/LED" (сигнал при тривозі) з налаштувань веб-інтерфейсу
  TODO #2: додати кешування/бекоф для API (якщо помилка 3+ рази — робити паузу 30-60с)
*/

#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

const char* AP_SSID = "GridWatch";
IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GW(192, 168, 4, 1);
IPAddress AP_MASK(255, 255, 255, 0);
const byte DNS_PORT = 53;

String MY_QUEUE  = "2.1";
String LIGHT_URL = "http://78.137.5.162:8009/light.json";

const char* TZ   = "EET-2EEST,M3.5.0/3,M10.5.0/4";
const char* NTP1 = "ua.pool.ntp.org";
const char* NTP2 = "pool.ntp.org";

String ALERTS_URL = "https://ubilling.net.ua/aerialalerts/";
String OBLAST_KEY = "Черкаська область";

const char* QUEUES[] = {
  "1.1","1.2",
  "2.1","2.2",
  "3.1","3.2",
  "4.1","4.2",
  "5.1","5.2",
  "6.1","6.2"
};
const int QUEUE_COUNT = sizeof(QUEUES)/sizeof(QUEUES[0]);

const char* OBLASTS[] = {
  "Вінницька область",
  "Волинська область",
  "Дніпропетровська область",
  "Донецька область",
  "Житомирська область",
  "Закарпатська область",
  "Запорізька область",
  "Івано-Франківська область",
  "Київська область",
  "Кіровоградська область",
  "Луганська область",
  "Львівська область",
  "Миколаївська область",
  "Одеська область",
  "Полтавська область",
  "Рівненська область",
  "Сумська область",
  "Тернопільська область",
  "Харківська область",
  "Херсонська область",
  "Хмельницька область",
  "Черкаська область",
  "Чернівецька область",
  "Чернігівська область",
  "м. Київ",
  "Автономна Республіка Крим",
  "м. Севастополь"
};
const int OBLAST_COUNT = sizeof(OBLASTS)/sizeof(OBLASTS[0]);

// ASCII (для LCD)
const char* OBLASTS_LCD[] = {
  "VINNYTSKA OBL",
  "VOLYNSKA OBL",
  "DNIPRO OBL",
  "DONETSKA OBL",
  "ZHYTOMYR OBL",
  "ZAKARPAT OBL",
  "ZAPORIZH OBL",
  "IFRANK OBL",
  "KYIVSKA OBL",
  "KIROVOHR OBL",
  "LUHANSKA OBL",
  "LVIVSKA OBL",
  "MYKOLAIV OBL",
  "ODESA OBL",
  "POLTAVA OBL",
  "RIVNE OBL",
  "SUMY OBL",
  "TERNOPIL OBL",
  "KHARKIV OBL",
  "KHERSON OBL",
  "KHMELNYT OBL",
  "CHERKASKA OBL",
  "CHERNIV OBL",
  "CHERNIH OBL",
  "M KYIV",
  "AR KRYM",
  "M SEVASTOP"
};

LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
DNSServer dns;
Preferences prefs;

unsigned long MODE_MS  = 10000UL;
unsigned long ALERT_MS = 5000UL;
unsigned long LIGHT_MS = 60UL * 1000UL;

unsigned long lastMode  = 0;
unsigned long lastAlert = 0;
unsigned long lastLight = 0;

int mode = 0;
int webMode = -1;

bool haveAlert = false;
bool alertNow  = false;

bool haveLight = false;
bool isLightOn = true;
unsigned long nextOffEpoch = 0;
unsigned long nextOnEpoch  = 0;

static bool colonBlink = false;

bool portalMode = true;
bool staConnected = false;

String staSsid = "";
String staPass = "";

// ---------------- LCD helpers ----------------
void clearLine(int row) {
  lcd.setCursor(0, row);
  lcd.print("                ");
}

void printCentered(int row, const String& s) {
  String out = s;
  if (out.length() > 16) out = out.substring(0, 16);
  int x = (16 - out.length()) / 2;
  if (x < 0) x = 0;
  clearLine(row);
  lcd.setCursor(x, row);
  lcd.print(out);
}

void showCentered(const String& a, const String& b) {
  printCentered(0, a);
  printCentered(1, b);
}

void showRaw16(const String& a, const String& b) {
  String A = a; String B = b;
  while (A.length() < 16) A += " ";
  while (B.length() < 16) B += " ";
  if (A.length() > 16) A = A.substring(0, 16);
  if (B.length() > 16) B = B.substring(0, 16);
  lcd.setCursor(0, 0); lcd.print(A);
  lcd.setCursor(0, 1); lcd.print(B);
}

void showSetupHintLCD() {
  String l1 = String("WiFi: ") + AP_SSID;
  if (l1.length() > 16) l1 = l1.substring(0, 16);
  String l2 = "IP: 192.168.4.1";
  if (l2.length() > 16) l2 = l2.substring(0, 16);
  showRaw16(l1, l2);
}

String oblastToLCD(const String& ua) {
  for (int i = 0; i < OBLAST_COUNT; i++) {
    if (ua == String(OBLASTS[i])) return String(OBLASTS_LCD[i]);
  }
  return "OBLAST";
}

// ---------------- HTTP helpers ----------------
bool httpsGet(const String& url, String& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(12000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}

bool httpGet(const String& url, String& out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(12000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}

// ---------------- Time helpers ----------------
long timeToSeconds(const String& t) {
  int p = t.indexOf(':');
  if (p < 0) return 0;
  return t.substring(0, p).toInt() * 3600L + t.substring(p + 1).toInt() * 60L;
}

String fmtLeftHMS(unsigned long t) {
  if (!t) return "--:--:--";
  long d = (long)t - (long)time(nullptr);
  if (d <= 0) return "00:00:00";
  char b[10];
  snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", d / 3600, (d / 60) % 60, d % 60);
  return String(b);
}

String nowTimeHHMM() {
  tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

String nowDate() {
  tm t;
  if (!getLocalTime(&t)) return "--.--.----";
  char buf[11];
  snprintf(buf, sizeof(buf), "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  return String(buf);
}

// ---------------- Fetchers ----------------
bool fetchAlerts() {
  String body;
  if (!httpsGet(ALERTS_URL, body)) return false;

  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, body)) return false;

  alertNow = doc["states"][OBLAST_KEY]["alertnow"].as<bool>();
  return true;
}

bool fetchLight() {
  tm t;
  if (!getLocalTime(&t, 1500)) return false;

  unsigned long nowEpoch = time(nullptr);
  unsigned long nowSecOfDay = t.tm_hour * 3600UL + t.tm_min * 60UL + t.tm_sec;
  unsigned long midnightEpoch = nowEpoch - nowSecOfDay;

  String body;
  if (!httpGet(LIGHT_URL, body)) return false;

  DynamicJsonDocument doc(65536);
  if (deserializeJson(doc, body)) return false;

  JsonArray today = doc["queues"][MY_QUEUE]["today"]["periods"];
  if (today.isNull()) return false;

  isLightOn = true;
  nextOffEpoch = 0;
  nextOnEpoch  = 0;

  for (JsonObject p : today) {
    unsigned long s = timeToSeconds(p["start"]);
    unsigned long e = timeToSeconds(p["end"]);
    if (nowSecOfDay >= s && nowSecOfDay < e) {
      isLightOn = false;
      nextOnEpoch = midnightEpoch + e;
      break;
    }
  }

  for (JsonObject p : today) {
    unsigned long s = timeToSeconds(p["start"]);
    unsigned long e = timeToSeconds(p["end"]);
    if (nowSecOfDay < s) {
      unsigned long off = midnightEpoch + s;
      unsigned long on  = midnightEpoch + e;
      if (nextOffEpoch == 0 || off < nextOffEpoch) {
        nextOffEpoch = off;
        if (isLightOn) nextOnEpoch = on;
      }
    }
  }
  return true;
}

// ---------------- LCD render ----------------
void drawClock() {
  tm t;
  if (!getLocalTime(&t)) {
    showCentered("NO TIME", "");
    return;
  }
  colonBlink = !colonBlink;
  char timeStr[6], dateStr[11];
  snprintf(timeStr, sizeof(timeStr), "%02d%c%02d", t.tm_hour, (colonBlink ? ':' : ' '), t.tm_min);
  snprintf(dateStr, sizeof(dateStr), "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  showCentered(timeStr, dateStr);
}

void drawAlerts() {
  if (!haveAlert) {
    showCentered("ALERTS", "NO DATA");
    return;
  }

  String ob = oblastToLCD(OBLAST_KEY);
  String l0 = "OBL:" + ob;
  if (l0.length() > 16) l0 = l0.substring(0, 16);

  String l1 = alertNow ? "POVITR TRIVOGA" : "NEMA TRIVOGY";
  showRaw16(l0, l1);
}

void drawLight() {
  if (!haveLight) {
    showCentered("SVITLO", "NO DATA");
    return;
  }
  if (isLightOn) showRaw16("SVITLO: ON",  "OFF IN " + fmtLeftHMS(nextOffEpoch));
  else          showRaw16("SVITLO: OFF", "ON  IN " + fmtLeftHMS(nextOnEpoch));
}

// ---------------- Preferences ----------------
void loadPrefs() {
  prefs.begin("cfg", true);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  MY_QUEUE = prefs.getString("queue", "2.1");
  OBLAST_KEY = prefs.getString("oblast", "Черкаська область");
  MODE_MS = prefs.getULong("mode_ms", 10000UL);
  ALERT_MS = prefs.getULong("al_ms", 5000UL);
  LIGHT_MS = prefs.getULong("li_ms", 60000UL);
  prefs.end();
}

void saveWifi(const String& ssid, const String& pass) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  staSsid = ssid;
  staPass = pass;
}

void saveSettings() {
  prefs.begin("cfg", false);
  prefs.putString("queue", MY_QUEUE);
  prefs.putString("oblast", OBLAST_KEY);
  prefs.putULong("mode_ms", MODE_MS);
  prefs.putULong("al_ms", ALERT_MS);
  prefs.putULong("li_ms", LIGHT_MS);
  prefs.end();
}

void clearWifi() {
  prefs.begin("cfg", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  staSsid = "";
  staPass = "";
}

// ---------------- WiFi ----------------
void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  WiFi.softAP(AP_SSID);
  dns.start(DNS_PORT, "*", AP_IP);
}

bool connectSTA(unsigned long timeoutMs = 20000UL) {
  if (staSsid.length() == 0) return false;
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(250);
  return (WiFi.status() == WL_CONNECTED);
}

void applyTimeIfOnline() {
  if (WiFi.status() == WL_CONNECTED) configTzTime(TZ, NTP1, NTP2);
}

// ---------------- Web UI CSS (centered, no emoji design) ----------------
String baseCSS() {
  return R"CSS(
:root{
  --bg0:#050814;
  --bg1:#060a18;
  --bg2:#081026;

  --ink:rgba(255,255,255,.92);
  --muted:rgba(255,255,255,.66);

  --glass:rgba(255,255,255,.06);
  --glass2:rgba(255,255,255,.04);
  --stroke:rgba(255,255,255,.12);

  --field:rgba(6,10,24,.82);
  --fieldStroke:rgba(255,255,255,.14);

  --accent1:#4f7cff;
  --accent2:#22d3ee;
  --accent3:#7c3aed;

  --r:22px;
  --shadow: 0 18px 60px rgba(0,0,0,.45);
  --shadow2: 0 10px 30px rgba(0,0,0,.28);
}

*{box-sizing:border-box}
html,body{height:100%}

body{
  margin:0;
  color:var(--ink);
  font-family: ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, Arial;
  -webkit-font-smoothing: antialiased;
  text-rendering: optimizeLegibility;

  display:flex;
  align-items:center;
  justify-content:center;

  background:
    radial-gradient(1100px 650px at 18% 18%, rgba(79,124,255,.18), transparent 62%),
    radial-gradient(900px 520px at 85% 20%, rgba(34,211,238,.14), transparent 60%),
    radial-gradient(1000px 650px at 50% 92%, rgba(124,58,237,.12), transparent 62%),
    linear-gradient(160deg, var(--bg0), var(--bg2));
}

.wrap{
  width: min(980px, 100%);
  padding: 18px;
  text-align:center;
}

/* unified centered header */
.header{
  position:relative;
  padding: 22px 18px;
  border-radius: calc(var(--r) + 8px);
  background: linear-gradient(135deg, rgba(79,124,255,.14), rgba(34,211,238,.07));
  border: 1px solid rgba(255,255,255,.10);
  box-shadow: var(--shadow);
  backdrop-filter: blur(10px);
  text-align:center;
}

.header:before{
  content:"";
  position:absolute; inset:0;
  border-radius: inherit;
  pointer-events:none;
  background:
    radial-gradient(700px 220px at 50% 0%, rgba(255,255,255,.10), transparent 60%),
    radial-gradient(520px 260px at 50% 100%, rgba(255,255,255,.06), transparent 62%);
  opacity:.75;
}

.title{
  font-size: 20px;
  font-weight: 950;
  letter-spacing: .2px;
}

.small{
  margin-top:8px;
  color:var(--muted);
  font-size:13px;
  line-height:1.35;
}

/* centered grid */
.grid{
  margin-top: 14px;
  display:grid;
  grid-template-columns: repeat(12, 1fr);
  gap:14px;
}

.card{
  grid-column: span 6;
  padding:18px 16px;
  border-radius: var(--r);
  background: linear-gradient(180deg, var(--glass), var(--glass2));
  border: 1px solid var(--stroke);
  box-shadow: var(--shadow2);
  backdrop-filter: blur(10px);
  position:relative;
  overflow:hidden;
  text-align:center;
}

.card:after{
  content:"";
  position:absolute;
  inset:-1px;
  border-radius: inherit;
  pointer-events:none;
  background: radial-gradient(900px 260px at 50% 0%, rgba(79,124,255,.10), transparent 60%);
  opacity:.55;
}

.card.big{ grid-column: span 12; }

.label{
  font-size: 11px;
  color: var(--muted);
  text-transform: uppercase;
  letter-spacing: .14em;
}

.value{
  margin-top:10px;
  font-size: 22px;
  font-weight: 950;
  letter-spacing: .2px;
}

/* centered controls */
.controls{
  margin-top:14px;
  padding:16px;
  border-radius: var(--r);
  background: linear-gradient(135deg, rgba(255,255,255,.07), rgba(255,255,255,.04));
  border:1px solid rgba(255,255,255,.10);
  box-shadow: var(--shadow2);
  backdrop-filter: blur(10px);

  display:flex;
  flex-direction:column;
  gap:12px;
  align-items:center;
  justify-content:center;
  text-align:center;
}

.controls .row{
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  align-items:center;
  justify-content:center;
  width:100%;
}

input,select,button{
  border:none; outline:none;
  border-radius: 14px;
  padding: 12px 14px;
  color: var(--ink);
  background: var(--field);
  border: 1px solid var(--fieldStroke);
  min-width: 220px;
}

select{appearance:none}
option{background:#0b1020; color:rgba(255,255,255,.92);}

button{
  cursor:pointer;
  font-weight: 950;
  letter-spacing:.2px;
  background: linear-gradient(135deg, rgba(79,124,255,.95), rgba(34,211,238,.78));
  box-shadow: 0 10px 30px rgba(79,124,255,.18);
  border:1px solid rgba(255,255,255,.16);
  transition: transform .06s ease, filter .12s ease;
}
button:hover{ filter: brightness(1.03); }
button:active{ transform: translateY(1px); }

button.secondary{
  background: rgba(255,255,255,.10);
  border: 1px solid rgba(255,255,255,.14);
  box-shadow:none;
}

a{color:#b7f0ff}
footer{
  margin-top:14px;
  color:var(--muted);
  font-size:12px;
  text-align:center;
}

@media (max-width: 760px){
  .wrap{padding:12px}
  .card{grid-column: span 12;}
  .value{font-size:18px;}
  input,select,button{width:100%; min-width: 0;}
  .controls{padding:14px}
}
)CSS";
}

// ---------------- HTML helpers ----------------
String queueOptionsHTML(const String& selected) {
  String o;
  for (int i = 0; i < QUEUE_COUNT; i++) {
    String q = QUEUES[i];
    o += "<option value='" + q + "'";
    if (q == selected) o += " selected";
    o += ">" + q + "</option>";
  }
  return o;
}

String oblastOptionsHTML(const String& selected) {
  String o;
  for (int i = 0; i < OBLAST_COUNT; i++) {
    String ob = OBLASTS[i];
    o += "<option value='" + ob + "'";
    if (ob == selected) o += " selected";
    o += ">" + ob + "</option>";
  }
  return o;
}

// ---------------- Pages ----------------
String pageSetup(const String& msg = "") {
  int n = WiFi.scanNetworks(false, true);
  String ssidOptions;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    ssidOptions += "<option value=\"" + s + "\">" + s + " (" + String(rssi) + "dBm";
    ssidOptions += open ? ", open" : "";
    ssidOptions += ")</option>";
  }

  String h;
  h += "<!doctype html><html lang='uk'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Перший запуск</title><style>" + baseCSS() + "</style></head><body><div class='wrap'>";

  h += "<div class='header'><div class='title'>Перший запуск</div>";
  h += "<div class='small'>Підключись до <b>" + String(AP_SSID) + "</b> і відкрий <b>http://192.168.4.1</b></div>";
  if (msg.length()) h += "<div class='small' style='color:#ffd7d7'><b>" + msg + "</b></div>";
  h += "</div>";

  h += "<div class='grid'><div class='card big'>";
  h += "<form method='POST' action='/setup/save' style='margin-top:10px;display:grid;gap:10px;justify-items:center'>";

  h += "<div class='label'>Wi-Fi мережа</div>";
  h += "<select name='ssid'><option value=''>— вибери зі списку —</option>" + ssidOptions + "</select>";
  h += "<input name='ssid_manual' placeholder='або впиши SSID вручну (якщо прихована)'>";
  h += "<input type='password' name='pass' placeholder='Пароль Wi-Fi (якщо open — залиш пустим)'>";

  h += "<div class='label'>Черга відключень</div>";
  h += "<select name='queue' required>" + queueOptionsHTML(MY_QUEUE) + "</select>";

  h += "<div class='label'>Область для тривог</div>";
  h += "<select name='oblast' required>" + oblastOptionsHTML(OBLAST_KEY) + "</select>";

  h += "<button type='submit'>Зберегти і підключитись</button>";
  h += "</form>";
  h += "<div class='small'>Якщо пароль/мережа неправильні — поверне сюди.</div>";
  h += "</div></div>";

  h += "<footer>Setup IP: <b>192.168.4.1</b></footer>";
  h += "</div></body></html>";
  return h;
}

String pagePanel() {
  String h;
  h += "<!doctype html><html lang='uk'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>GridWatch</title><style>" + baseCSS() + "</style></head><body><div class='wrap'>";

  h += "<div class='header'>";
  h += "<div class='title'>GridWatch</div>";
  h += "<div class='small'>Wi-Fi: <b id='ssid'>—</b> &nbsp; • &nbsp; Черга: <b id='q'>—</b> &nbsp; • &nbsp; Область: <b id='ob'>—</b></div>";
  h += "<div class='small'>AP: <b>192.168.4.1</b> &nbsp; • &nbsp; STA: <b id='staIp'>—</b></div>";
  h += "</div>";

  h += "<div class='grid'>";
  h += "<div class='card'><div class='label'>Час</div><div class='value' id='t'>--:--</div><div class='small' id='d'>--.--.----</div></div>";
  h += "<div class='card'><div class='label'>Тривога</div><div class='value' id='al'>—</div><div class='small'>оновлення 1с</div></div>";
  h += "<div class='card big'><div class='label'>Світло</div><div class='value' id='lt'>—</div><div class='small'>оновлення 1с</div></div>";
  h += "</div>";

  h += "<div class='controls'>";
  h += "<div class='row'>";
  h += "<span class='label' style='margin-right:6px'>LCD режим</span>";
  h += "<select id='m'>";
  h += "<option value='-1'>Авто (карусель)</option>";
  h += "<option value='0'>Годинник</option>";
  h += "<option value='1'>Тривога</option>";
  h += "<option value='2'>Світло</option>";
  h += "</select>";
  h += "<button class='secondary' onclick='setMode()'>Застосувати</button>";
  h += "</div>";

  h += "<div class='row'>";
  h += "<button onclick='refreshNow()'>Оновити зараз</button>";
  h += "<button class='secondary' onclick=\"location.href='/settings'\">Параметри</button>";
  h += "<button class='secondary' onclick=\"if(confirm('Скинути Wi-Fi?')) fetch('/wifi/reset').then(()=>location.href='/')\">Скинути Wi-Fi</button>";
  h += "</div>";
  h += "</div>";

  h += R"JS(
<script>
let lockModeUntil = 0;
function lockMode(ms=5000){ lockModeUntil = Date.now() + ms; }

async function api(p){
  const r = await fetch(p,{cache:"no-store"});
  return await r.json();
}

async function tick(){
  try{
    const j = await api('/api/status');
    document.getElementById('t').textContent = j.time;
    document.getElementById('d').textContent = j.date;
    document.getElementById('al').textContent = j.alert;
    document.getElementById('lt').textContent = j.light;
    document.getElementById('q').textContent  = j.queue;
    document.getElementById('ob').textContent = j.oblast;
    document.getElementById('ssid').textContent = j.ssid || "не підключено";
    document.getElementById('staIp').textContent = j.staIp || "—";
    const sel = document.getElementById('m');
    if (Date.now() > lockModeUntil) sel.value = String(j.webMode);
  }catch(e){}
}

async function setMode(){
  const sel = document.getElementById('m');
  lockMode(5000);
  const v = sel.value;
  await fetch('/api/mode?m='+encodeURIComponent(v), {cache:"no-store"});
  await tick();
}

async function refreshNow(){
  await fetch('/api/refresh',{cache:"no-store"});
  await tick();
}

const sel = document.getElementById('m');
sel.addEventListener('focus', ()=>lockMode(5000));
sel.addEventListener('mousedown', ()=>lockMode(5000));
sel.addEventListener('touchstart', ()=>lockMode(5000));
sel.addEventListener('change', ()=>lockMode(5000));

setInterval(tick, 1000);
tick();
</script>
)JS";

  h += "<footer>Зайти можна завжди через <b>http://192.168.4.1</b></footer>";
  h += "</div></body></html>";
  return h;
}

String pageSettings(const String& msg = "") {
  String h;
  h += "<!doctype html><html lang='uk'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>Параметри</title><style>" + baseCSS() + "</style></head><body><div class='wrap'>";

  h += "<div class='header'>";
  h += "<div class='title'>Параметри</div>";
  h += "<div class='small'>Зміна черги, області та інтервалів оновлень.</div>";
  if (msg.length()) h += "<div class='small' style='color:#d7ffe8'><b>" + msg + "</b></div>";
  h += "<div class='small'><a href='/'>Назад</a></div>";
  h += "</div>";

  h += "<div class='grid'><div class='card big'>";
  h += "<form method='POST' action='/settings/save' style='display:grid;gap:10px;margin-top:6px;justify-items:center'>";

  h += "<div class='label'>Черга відключень</div>";
  h += "<select name='queue' required>" + queueOptionsHTML(MY_QUEUE) + "</select>";

  h += "<div class='label'>Область для тривог</div>";
  h += "<select name='oblast' required>" + oblastOptionsHTML(OBLAST_KEY) + "</select>";

  h += "<div class='label'>MODE_MS (мс) — автоперемикання LCD</div>";
  h += "<input name='mode_ms' value='" + String(MODE_MS) + "'>";

  h += "<div class='label'>ALERT_MS (мс) — оновлення тривог</div>";
  h += "<input name='al_ms' value='" + String(ALERT_MS) + "'>";

  h += "<div class='label'>LIGHT_MS (мс) — оновлення світла</div>";
  h += "<input name='li_ms' value='" + String(LIGHT_MS) + "'>";

  h += "<button type='submit'>Зберегти</button>";
  h += "</form>";
  h += "</div></div>";

  h += "</div></body></html>";
  return h;
}

// ---------------- Handlers ----------------
void handleRoot() {
  if (!staConnected && portalMode) { server.send(200, "text/html; charset=utf-8", pageSetup()); return; }
  server.send(200, "text/html; charset=utf-8", pagePanel());
}

void handleSetup() { server.send(200, "text/html; charset=utf-8", pageSetup()); }

void handleSetupSave() {
  String ssid = server.arg("ssid");
  String ssidManual = server.arg("ssid_manual");
  String pass = server.arg("pass");

  String q = server.arg("queue");
  String ob = server.arg("oblast");

  ssidManual.trim();
  if (ssidManual.length()) ssid = ssidManual;
  ssid.trim();

  if (!ssid.length()) { server.send(200, "text/html; charset=utf-8", pageSetup("Не вибрано SSID")); return; }
  if (!q.length()) q = "2.1";
  if (!ob.length()) ob = "Черкаська область";

  MY_QUEUE = q;
  OBLAST_KEY = ob;
  saveSettings();
  saveWifi(ssid, pass);

  showCentered("WIFI", "CONNECT...");
  bool ok = connectSTA(20000UL);
  staConnected = ok;

  if (!ok) {
    portalMode = true;
    showSetupHintLCD();
    server.send(200, "text/html; charset=utf-8", pageSetup("Не вдалося підключитись. Перевір пароль або мережу."));
    return;
  }

  portalMode = false;
  applyTimeIfOnline();

  haveAlert = fetchAlerts();
  haveLight = fetchLight();
  lastAlert = millis();
  lastLight = millis();

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSettings() {
  if (!staConnected && portalMode) { handleSetup(); return; }
  server.send(200, "text/html; charset=utf-8", pageSettings());
}

void handleSettingsSave() {
  if (!staConnected && portalMode) { handleSetup(); return; }

  String q = server.arg("queue"); q.trim();
  String ob = server.arg("oblast"); ob.trim();

  if (q.length()) MY_QUEUE = q;
  if (ob.length()) OBLAST_KEY = ob;

  String modeMs = server.arg("mode_ms");
  String alMs   = server.arg("al_ms");
  String liMs   = server.arg("li_ms");

  if (modeMs.length()) MODE_MS  = (unsigned long) modeMs.toInt();
  if (alMs.length())   ALERT_MS = (unsigned long) alMs.toInt();
  if (liMs.length())   LIGHT_MS = (unsigned long) liMs.toInt();

  if (MODE_MS  < 1000)  MODE_MS  = 1000;
  if (ALERT_MS < 1000)  ALERT_MS = 1000;
  if (LIGHT_MS < 5000)  LIGHT_MS = 5000;

  saveSettings();

  haveAlert = fetchAlerts();
  lastAlert = millis();

  server.send(200, "text/html; charset=utf-8", pageSettings("Збережено"));
}

void handleApiStatus() {
  unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastAlert >= 1000UL) { haveAlert = fetchAlerts(); lastAlert = now; }
    if (now - lastLight >= 1000UL) { haveLight = fetchLight();  lastLight = now; }
  }

  String alertText;
  if (!haveAlert) alertText = "Немає даних";
  else alertText = alertNow ? "ПОВІТРЯНА ТРИВОГА" : "Немає тривоги";

  String lightText;
  if (!haveLight) lightText = "Немає даних";
  else if (isLightOn) lightText = "Світло є. Вимкн. через " + fmtLeftHMS(nextOffEpoch);
  else lightText = "Світла нема. Увімкн. через " + fmtLeftHMS(nextOnEpoch);

  String json = "{";
  json += "\"time\":\"" + nowTimeHHMM() + "\",";
  json += "\"date\":\"" + nowDate() + "\",";
  json += "\"alert\":\"" + alertText + "\",";
  json += "\"light\":\"" + lightText + "\",";
  json += "\"queue\":\"" + MY_QUEUE + "\",";
  json += "\"oblast\":\"" + OBLAST_KEY + "\",";
  json += "\"webMode\":" + String(webMode) + ",";
  json += "\"ssid\":\"" + (staConnected ? staSsid : String("")) + "\",";
  json += "\"staIp\":\"" + (staConnected ? WiFi.localIP().toString() : String("")) + "\"";
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleApiMode() {
  if (!server.hasArg("m")) { server.send(400, "text/plain", "no m"); return; }
  int m = server.arg("m").toInt();
  if (m < -1) m = -1;
  if (m > 2)  m = 2;

  webMode = m;

  if (webMode == -1) lastMode = millis();
  else mode = webMode;

  lcd.clear();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiRefresh() {
  haveAlert = fetchAlerts();
  haveLight = fetchLight();
  lastAlert = millis();
  lastLight = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleWifiReset() {
  clearWifi();
  staConnected = false;
  portalMode = true;
  WiFi.disconnect(true, true);
  lcd.clear();
  showSetupHintLCD();
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void setupWeb() {
  server.on("/", handleRoot);
  server.on("/setup", HTTP_GET, handleSetup);
  server.on("/setup/save", HTTP_POST, handleSetupSave);

  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings/save", HTTP_POST, handleSettingsSave);

  server.on("/api/status", handleApiStatus);
  server.on("/api/mode", handleApiMode);
  server.on("/api/refresh", handleApiRefresh);

  server.on("/wifi/reset", HTTP_GET, handleWifiReset);

  server.onNotFound(handleNotFound);
  server.begin();
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  loadPrefs();
  startAP();
  setupWeb();

  if (staSsid.length()) {
    staConnected = connectSTA(20000UL);
    portalMode = !staConnected;
    if (staConnected) applyTimeIfOnline();
    else { portalMode = true; lcd.clear(); showSetupHintLCD(); }
  } else {
    portalMode = true;
    lcd.clear();
    showSetupHintLCD();
  }

  haveAlert = fetchAlerts();
  haveLight = fetchLight();
  lastAlert = lastLight = lastMode = millis();
}

void loop() {
  if (portalMode) dns.processNextRequest();
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) { staConnected = true; portalMode = false; }
  else { staConnected = false; portalMode = true; }

  if (portalMode) {
    showSetupHintLCD();
    delay(200);
    return;
  }

  unsigned long now = millis();

  if (now - lastAlert >= ALERT_MS) { haveAlert = fetchAlerts(); lastAlert = now; }
  if (now - lastLight >= LIGHT_MS) { haveLight = fetchLight();  lastLight = now; }

  if (webMode == -1) {
    if (now - lastMode >= MODE_MS) {
      mode = (mode + 1) % 3;
      lastMode = now;
      lcd.clear();
    }
  } else {
    if (mode != webMode) {
      mode = webMode;
      lcd.clear();
    }
  }

  if      (mode == 0) drawClock();
  else if (mode == 1) drawAlerts();
  else                drawLight();

  delay(50);
}
