#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

TFT_eSPI tft = TFT_eSPI();

#define SCREEN_W     320
#define SCREEN_H     240
#define CHAR_W         8
#define CHAR_H        16
#define STAT_H        16
#define HINT_H        16
#define CONT_Y        STAT_H
#define CONT_H        (SCREEN_H - STAT_H - HINT_H)
#define CONT_ROWS     (CONT_H / CHAR_H)
#define CONT_COLS     (SCREEN_W / CHAR_W)
#define HINT_Y        (SCREEN_H - HINT_H)

#define SB_W    300
#define SB_H     26
#define SB_X    ((SCREEN_W - SB_W) / 2)
#define SB_Y    (CONT_Y + (CONT_H - SB_H) / 2)

#define C_WHITE    0xFFFF
#define C_BLACK    0x0000
#define C_BLUE     0xF800
#define C_RED      0x001F
#define C_GREEN    0x07E0
#define C_ORANGE   0x041F
#define C_LTGRAY   0x8410
#define C_DKGRAY   0x4208
#define C_HIBLUE   0x3800

#define KEYBOARD_ADDR  0x55
#define I2C_SDA        18
#define I2C_SCL         8
#define BOARD_POWERON  10
#define BAT_ADC         4
#define TB_UP           3
#define TB_DOWN        15
#define TB_CLICK        0

#define PSRAM_PAGE_SIZE  (200 * 1024)
#define MAX_LINES         400
#define MAX_LINKS          30
#define LINK_URL_LEN      256

struct LineSpan  { uint32_t start; uint16_t len; };
struct LinkEntry { char url[LINK_URL_LEN]; };

static char*      g_pageText  = nullptr;
static size_t     g_pageLen   = 0;
static LineSpan*  g_lines     = nullptr;
static int        g_lineCount = 0;
static LinkEntry* g_links     = nullptr;
static int        g_linkCount = 0;

#define DDG_LITE_HOST  "lite.duckduckgo.com"
#define DDG_LITE_PATH  "/lite/"
#define DDG_LITE_PORT  443

enum AppState {
    STATE_BOOT, STATE_WIFI_SCAN, STATE_SEARCH_IDLE,
    STATE_RESULTS, STATE_PAGE_VIEW
};
AppState appState      = STATE_BOOT;
int      scrollPos     = 0;
String   currentURL    = "";
String   baseDomain    = "";
String   g_searchQuery = "";

String  wifiSSIDs[20];
int8_t  wifiRSSI[20];
int     wifiCount    = 0;
int     wifiSelected = 0;
int     wifiScrollOff= 0;

#define HISTORY_MAX 16
String urlHistory[HISTORY_MAX];
int    historyCount = 0;

#define MAX_RESULTS       100
#define RESULTS_PER_PAGE    4
#define ROWS_PER_RESULT     3

struct SearchResult { char title[80]; char url[256]; char snippet[160]; };
static SearchResult g_results[MAX_RESULTS];
static int          g_resultCount  = 0;
static int          g_resultScroll = 0;
static int          g_resultCursor = 0;

static WiFiClientSecure* g_ssl       = nullptr;
static unsigned long     lastStatusMs = 0;
#define STATUS_INTERVAL 3000
static Preferences prefs;

static void getBattery(int& pct, bool& charging) {
    int raw = analogRead(BAT_ADC);
    float v  = (raw / 4095.0f) * 3.3f * 2.0f;
    charging = (v > 4.3f);
    pct = charging ? 100 : (int)constrain((v - 3.0f) / 1.2f * 100.0f, 0, 100);
}

static char readKey() {
    Wire.requestFrom(KEYBOARD_ADDR, 1);
    if (Wire.available()) {
        char k = (char)Wire.read();
        if (k != 0 && k != (char)255) return k;
    }
    return 0;
}

static void ptext(int x, int y, const char* s, uint16_t fg, uint16_t bg, int sz = 1) {
    tft.setTextSize(sz);
    tft.setTextColor(fg, bg);
    tft.setCursor(x, y);
    tft.print(s);
}

static void pcenter(int y, const char* s, uint16_t fg, uint16_t bg, int sz = 1) {
    int w = (int)strlen(s) * CHAR_W * sz;
    ptext((SCREEN_W - w) / 2, y, s, fg, bg, sz);
}

static void drawStatusBar(const char* label = nullptr) {

    tft.fillRect(0, 0, SCREEN_W, STAT_H, C_WHITE);

    tft.drawFastHLine(0, STAT_H - 1, SCREEN_W, C_LTGRAY);

    tft.setTextSize(1);

    uint32_t freeK = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    char ram[16]; snprintf(ram, 16, "RAM:%lukb", (unsigned long)freeK);
    int ramX = SCREEN_W - (int)strlen(ram) * CHAR_W - 3;
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(ramX, 1);
    tft.print(ram);

    const char* mid = label ? label : currentURL.c_str();
    if (mid && mid[0]) {
        if (strncmp(mid, "https://", 8) == 0) mid += 8;
        else if (strncmp(mid, "http://", 7) == 0) mid += 7;
        int avail = (ramX - 4) / CHAR_W;
        if (avail > 3) {
            tft.setTextColor(C_DKGRAY, C_WHITE);
            tft.setCursor(3, 1);
            int len = (int)strlen(mid);
            if (len > avail) {
                for (int i = 0; i < avail - 2; i++) tft.print(mid[i]);
                tft.print("..");
            } else {
                tft.print(mid);
            }
        }
    }
}

static void drawHintBar(const char* msg) {
    tft.fillRect(0, HINT_Y, SCREEN_W, HINT_H, C_WHITE);
    tft.drawFastHLine(0, HINT_Y, SCREEN_W, C_LTGRAY);
    tft.setTextSize(1);
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(3, HINT_Y + 1);
    tft.print(msg);
}

static void drawBoot(int pct) {
    if (pct == 0) {
        tft.fillScreen(C_WHITE);

        tft.setTextSize(3);
        tft.setTextColor(C_BLACK, C_WHITE);
        tft.setCursor(52, 50);
        tft.print("CanuckWeb");

        tft.setTextSize(1);
        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor(72, 104);
        tft.print("DDG Lite + Jina Reader");

        tft.setTextColor(C_BLACK, C_WHITE);
        tft.setCursor(68, 120);
        tft.print("Canadians don't give up");
    }

    int barY = 230;

    tft.fillRect(0, barY, SCREEN_W, 10, C_WHITE);

    int fw = SCREEN_W * pct / 100;
    if (fw > 0) tft.fillRect(0, barY, fw, 10, C_RED);
}

static void doWifiScan() {
    tft.fillScreen(C_WHITE);
    drawStatusBar("WiFi Setup");

    tft.setTextSize(1);
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor(116, CONT_Y + 80);
    tft.print("Scanning...");

    WiFi.disconnect(true); delay(100);
    wifiCount = WiFi.scanNetworks();
    if (wifiCount < 0) wifiCount = 0;
    if (wifiCount > 20) wifiCount = 20;
    for (int i = 0; i < wifiCount; i++) {
        wifiSSIDs[i] = WiFi.SSID(i);
        wifiRSSI[i]  = (int8_t)WiFi.RSSI(i);
    }
    wifiSelected  = 0;
    wifiScrollOff = 0;
}

static void drawWifiList() {
    tft.fillScreen(C_WHITE);
    drawStatusBar("WiFi Setup");
    drawHintBar("UP/DN=select  ENTER=connect  R=rescan");

    tft.setTextSize(1);

    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor(108, CONT_Y + 8);
    tft.print("WiFi Networks");

    tft.drawFastHLine(10, CONT_Y + 26, SCREEN_W - 20, C_LTGRAY);

    if (wifiCount == 0) {

        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor(92, CONT_Y + 80);
        tft.print("No networks found");
        return;
    }

    int listY  = CONT_Y + 30;
    int maxVis = 9;
    for (int i = 0; i < maxVis; i++) {
        int idx = i + wifiScrollOff;
        if (idx >= wifiCount) break;
        bool sel = (idx == wifiSelected);
        int rowY = listY + i * CHAR_H;

        if (sel) {

            tft.fillRect(0, rowY, SCREEN_W, CHAR_H, C_BLUE);
            tft.setTextColor(C_WHITE, C_BLUE);
        } else {
            tft.fillRect(0, rowY, SCREEN_W, CHAR_H, C_WHITE);
            tft.setTextColor(C_BLACK, C_WHITE);
        }

        char ssid[29]; strncpy(ssid, wifiSSIDs[idx].c_str(), 28); ssid[28] = 0;
        char db[8]; snprintf(db, 8, "%ddB", (int)wifiRSSI[idx]);

        tft.setCursor(8, rowY + 1);
        tft.print(ssid);

        int dbX = SCREEN_W - (int)strlen(db) * CHAR_W - 6;
        tft.setCursor(dbX, rowY + 1);
        tft.print(db);
    }

    if (wifiCount > maxVis) {

        char sc[8]; snprintf(sc, 8, "%d/%d", wifiSelected + 1, wifiCount);
        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor(SCREEN_W - (int)strlen(sc) * CHAR_W - 4, HINT_Y - CHAR_H - 2);
        tft.print(sc);
    }
}

static String enterPassword(const String& ssid) {
    tft.fillScreen(C_WHITE);
    drawStatusBar("WiFi Setup");
    drawHintBar("Type password  ENTER=connect  BKSP=del");

    tft.setTextSize(1);

    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(10, CONT_Y + 20);
    tft.print("Network:");
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor(10 + 9 * CHAR_W, CONT_Y + 20);
    tft.print(ssid.substring(0, 28));

    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(10, CONT_Y + 52);
    tft.print("Password:");

    int bx = SB_X, by = CONT_Y + 72, bw = SB_W, bh = SB_H;
    tft.drawRect(bx,   by,   bw,   bh,   C_ORANGE);
    tft.drawRect(bx+1, by+1, bw-2, bh-2, C_ORANGE);
    tft.fillRect(bx+2, by+2, bw-4, bh-4, C_WHITE);

    String pw;
    while (true) {
        char k = readKey(); if (!k) { delay(10); continue; }
        if (k == '\n' || k == '\r') break;
        if ((k == 8 || k == 127) && pw.length() > 0) {
            pw.remove(pw.length() - 1);

            tft.fillRect(bx+2, by+2, bw-4, bh-4, C_WHITE);
            tft.setTextColor(C_BLACK, C_WHITE);
            tft.setCursor(bx + 6, by + 5);
            for (int i = 0; i < (int)pw.length(); i++) tft.print('*');
        } else if (k >= 32 && k < 127) {
            pw += k;
            tft.setTextColor(C_BLACK, C_WHITE);
            tft.setCursor(bx + 6 + (int)(pw.length() - 1) * CHAR_W, by + 5);
            tft.print('*');
        }
        delay(40);
    }
    return pw;
}

static bool connectWifi(const String& ssid, const String& pw) {
    tft.fillScreen(C_WHITE);
    drawStatusBar("Connecting...");
    drawHintBar("Please wait...");

    tft.setTextSize(1);

    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(104, CONT_Y + 50);
    tft.print("Connecting to:");

    String s = ssid.substring(0, 32);
    int sw = (int)s.length() * CHAR_W;
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor((SCREEN_W - sw) / 2, CONT_Y + 70);
    tft.print(s);

    WiFi.begin(ssid.c_str(), pw.c_str());
    int dots = 0, attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < 24) {
        delay(500);
        tft.setTextColor(C_BLACK, C_WHITE);
        tft.setCursor(10 + dots * (CHAR_W + 2), CONT_Y + 100);
        tft.print('.');
        dots = (dots + 1) % 38;
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!g_ssl) { g_ssl = new WiFiClientSecure(); g_ssl->setInsecure(); }

        tft.setTextColor(C_GREEN, C_WHITE);
        tft.setCursor(120, CONT_Y + 130);
        tft.print("Connected!");

        String ip = WiFi.localIP().toString();
        int ipW = ip.length() * CHAR_W;
        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor((SCREEN_W - ipW) / 2, CONT_Y + 150);
        tft.print(ip);
        delay(1000);
        return true;
    }

    tft.setTextColor(C_RED, C_WHITE);
    tft.setCursor(132, CONT_Y + 130);
    tft.print("Failed!");
    delay(2000);
    return false;
}

static void saveCredentials(const String& ssid, const String& pw) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pw", pw);
    prefs.end();
}

static bool loadAndConnect() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pw   = prefs.getString("pw",   "");
    prefs.end();
    if (ssid.isEmpty()) return false;

    tft.fillScreen(C_WHITE);
    drawStatusBar("Connecting...");
    drawHintBar("Press any key to skip");

    tft.setTextSize(1);

    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(88, CONT_Y + 50);
    tft.print("Reconnecting to:");
    String s = ssid.substring(0, 32);
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor((SCREEN_W - (int)s.length() * CHAR_W) / 2, CONT_Y + 70);
    tft.print(s);

    WiFi.begin(ssid.c_str(), pw.c_str());
    unsigned long t = millis(); int dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t < 8000) {
        delay(300);
        tft.setTextColor(C_BLACK, C_WHITE);
        tft.setCursor(10 + dots * (CHAR_W + 2), CONT_Y + 100);
        tft.print('.');
        dots = (dots + 1) % 38;
        char k = readKey(); if (k) { WiFi.disconnect(); return false; }
    }
    if (WiFi.status() == WL_CONNECTED) {
        if (!g_ssl) { g_ssl = new WiFiClientSecure(); g_ssl->setInsecure(); }
        tft.setTextColor(C_GREEN, C_WHITE);
        tft.setCursor(120, CONT_Y + 130);
        tft.print("Connected!");
        delay(600);
        return true;
    }
    return false;
}

static String urlEncodeQuery(const String& q) {
    String out; out.reserve(q.length() * 2);
    for (char c : q) {
        if (c == ' ') out += '+';
        else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { char buf[4]; snprintf(buf, 4, "%%%02X", (unsigned char)c); out += buf; }
    }
    return out;
}

static void drawSearchBox(int byY, const String& query, bool activeCursor) {
    int bx = SB_X, bw = SB_W, bh = SB_H;

    tft.fillRect(bx, byY, bw, bh, C_WHITE);

    tft.drawRect(bx,   byY,   bw,   bh,   C_ORANGE);
    tft.drawRect(bx+1, byY+1, bw-2, bh-2, C_ORANGE);

    int textX  = bx + 6;
    int textY  = byY + 5;
    int maxVis = (bw - 12) / CHAR_W;

    int qlen  = (int)query.length();
    int start = max(0, qlen - maxVis);

    tft.setTextSize(1);
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor(textX, textY);
    for (int i = start; i < qlen; i++) tft.print(query[i]);

    if (activeCursor) {
        int visible = qlen - start;
        int curX    = textX + visible * CHAR_W;
        bool blink  = (millis() / 800) % 2 == 0;
        tft.fillRect(curX, textY + 2, 2, CHAR_H - 6, blink ? C_BLACK : C_WHITE);
    }
}

static bool extractAttrVal(const char* tag, const char* attr, char* out, int outLen) {
    const char* p = strstr(tag, attr); if (!p) return false;
    p += strlen(attr);
    while (*p == ' ' || *p == '=') p++;
    char q = (*p == '"' || *p == '\'') ? *p++ : 0;
    int i = 0;
    while (*p && i < outLen - 1) {
        if (q && *p == q) break;
        if (!q && (*p == ' ' || *p == '>')) break;
        out[i++] = *p++;
    }
    out[i] = 0; return i > 0;
}

static void inlineStrip(char* s) {
    char *r = s, *w = s; bool intag = false;
    while (*r) {
        if (*r == '<') { intag = true;  r++; continue; }
        if (*r == '>') { intag = false; r++; continue; }
        if (!intag) {
            if (*r == '&') {
                char* semi = strchr(r, ';');
                if (semi && semi - r < 10) {
                    char ent[12] = {}; memcpy(ent, r+1, semi-r-1);
                    char d = 0;
                    if (!strcmp(ent,"amp"))  d = '&'; else if (!strcmp(ent,"lt"))   d = '<';
                    else if (!strcmp(ent,"gt"))  d = '>'; else if (!strcmp(ent,"nbsp")) d = ' ';
                    else if (!strcmp(ent,"quot")) d = '"';
                    if (d) { *w++ = d; r = semi + 1; continue; }
                }
            }
            *w++ = *r;
        }
        r++;
    }
    *w = 0;
    r = s; w = s; bool sp = true;
    while (*r) {
        if (*r == '\n' || *r == '\r' || *r == '\t') *r = ' ';
        if (*r == ' ' && sp) { r++; continue; }
        sp = (*r == ' '); *w++ = *r++;
    }
    if (w > s && *(w-1) == ' ') w--;
    *w = 0;
}

static void parseDDGLite(const String& html) {

    const char* p = html.c_str();
    const int   n = (int)html.length();
    while (g_resultCount < MAX_RESULTS) {
        const char* anchor = strstr(p, "result-link"); if (!anchor) break;
        const char* aStart = anchor;
        while (aStart > p && *aStart != '<') aStart--;
        if (*aStart != '<') { p = anchor + 11; continue; }
        const char* aTagEnd = strchr(aStart, '>'); if (!aTagEnd) break;
        char href[256] = {};
        { char tb[512] = {}; int tl = min((int)(aTagEnd - aStart), 510); memcpy(tb, aStart, tl);
          if (!extractAttrVal(tb, "href", href, sizeof(href))) { p = aTagEnd + 1; continue; } }
        if (strncmp(href, "http", 4) != 0) { p = aTagEnd + 1; continue; }
        const char* ts = aTagEnd + 1;
        const char* te = strstr(ts, "</a>");
        char title[80] = {};
        if (te) { int tl = min((int)(te - ts), 78); memcpy(title, ts, tl); inlineStrip(title); }
        if (!title[0]) strcpy(title, "(no title)");
        char snippet[160] = {};
        const char* st = te ? strstr(te, "result-snippet") : nullptr;
        if (st) {
            const char* ss2 = strchr(st, '>');
            if (ss2++) {
                const char* se = strstr(ss2, "</td>"); if (!se) se = strstr(ss2, "</span>");
                if (se) { int sl = min((int)(se - ss2), 158); memcpy(snippet, ss2, sl); inlineStrip(snippet); }
            }
        }
        SearchResult& sr = g_results[g_resultCount++];
        strlcpy(sr.title,   title,   sizeof(sr.title));
        strlcpy(sr.url,     href,    sizeof(sr.url));
        strlcpy(sr.snippet, snippet, sizeof(sr.snippet));
        p = te ? te + 4 : aTagEnd + 1;
        if (p - html.c_str() >= n) break;
    }
}

static int doSearch(const String& query) {
    g_resultCount = 0; g_resultScroll = 0; g_resultCursor = 0;

    tft.fillScreen(C_WHITE);
    drawStatusBar("Searching...");
    drawHintBar("Please wait...");
    tft.setTextSize(1);
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(68, CONT_Y + 50);
    tft.print("Searching DuckDuckGo...");
    String dq = query.substring(0, 36);
    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor((SCREEN_W - (int)dq.length() * CHAR_W) / 2, CONT_Y + 70);
    tft.print(dq);

    String body = "q=" + urlEncodeQuery(query);
    String req  = String("POST ") + DDG_LITE_PATH + " HTTP/1.0\r\n"
                  "Host: " DDG_LITE_HOST "\r\n"
                  "Content-Type: application/x-www-form-urlencoded\r\n"
                  "Content-Length: " + String(body.length()) + "\r\n"
                  "User-Agent: Mozilla/5.0 (compatible; CanuckWeb/1.00)\r\n"
                  "Accept: text/html\r\n"
                  "Connection: close\r\n\r\n" + body;

    if (!g_ssl) { g_ssl = new WiFiClientSecure(); g_ssl->setInsecure(); }

    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor((SCREEN_W - 13 * CHAR_W) / 2, CONT_Y + 90);
    tft.print("Connecting...");

    if (!g_ssl->connect(DDG_LITE_HOST, DDG_LITE_PORT)) {
        tft.setTextColor(C_RED, C_WHITE);
        tft.setCursor((SCREEN_W - 16 * CHAR_W) / 2, CONT_Y + 110);
        tft.print("Connection failed");
        delay(3000); return 0;
    }
    g_ssl->print(req);

    unsigned long t0 = millis();
    String response; response.reserve(30000);
    bool headersDone = false; String headerBuf;

    while (millis() - t0 < 15000) {
        if (g_ssl->available()) {
            char c = (char)g_ssl->read();
            if (!headersDone) {
                headerBuf += c;
                if (headerBuf.endsWith("\r\n\r\n")) {
                    headersDone = true;
                    if (headerBuf.indexOf("200") < 0) { g_ssl->stop(); delay(3000); return 0; }
                    tft.fillRect(0, CONT_Y + 90, SCREEN_W, CHAR_H, C_WHITE);
                    tft.setTextColor(C_DKGRAY, C_WHITE);
                    tft.setCursor((SCREEN_W - 14 * CHAR_W) / 2, CONT_Y + 90);
                    tft.print("Downloading...");
                }
            } else {
                response += c;
            }
        } else if (!g_ssl->connected()) break;
        else delay(2);
    }
    g_ssl->stop();

    if (!headersDone || response.length() < 100) {
        tft.setTextColor(C_RED, C_WHITE);
        tft.setCursor((SCREEN_W - 15 * CHAR_W) / 2, CONT_Y + 110);
        tft.print("No data received");
        delay(3000); return 0;
    }

    parseDDGLite(response);

    tft.fillRect(0, CONT_Y + 110, SCREEN_W, CHAR_H, C_WHITE);
    char found[24]; snprintf(found, 24, "Found: %d results", g_resultCount);
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor((SCREEN_W - (int)strlen(found) * CHAR_W) / 2, CONT_Y + 110);
    tft.print(found);
    delay(400);
    return g_resultCount;
}

static void drawIdleScreen() {
    tft.fillScreen(C_WHITE);
    drawStatusBar();
    tft.fillRect(0, HINT_Y, SCREEN_W, HINT_H, C_WHITE);

    tft.setTextSize(1);
    tft.setTextColor(C_DKGRAY, C_WHITE);

    tft.setCursor(124, SB_Y - 20);
    tft.print("CanuckWeb");

    drawSearchBox(SB_Y, g_searchQuery, true);
}

static void drawResults() {
    tft.fillScreen(C_WHITE);
    drawStatusBar("Results");
    drawHintBar("BALL=select  CLICK/ENTER=open  S=search");

    int boxY = CONT_Y + 4;
    drawSearchBox(boxY, g_searchQuery, false);

    tft.drawFastHLine(10, CONT_Y + 32, SCREEN_W - 20, C_LTGRAY);

    if (g_resultCount == 0) {

        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor((SCREEN_W - 10 * CHAR_W) / 2, CONT_Y + 80);
        tft.print("No results.");
        return;
    }

    int startY = CONT_Y + 36;
    int blockH = ROWS_PER_RESULT * CHAR_H;

    int visible = min(RESULTS_PER_PAGE, g_resultCount - g_resultScroll);
    for (int i = 0; i < visible; i++) {
        int idx  = g_resultScroll + i;
        auto& r  = g_results[idx];
        bool hi  = (idx == g_resultCursor);
        int yTop = startY + i * (blockH + 2);

        if (hi) {
            tft.fillRect(0, yTop, SCREEN_W, CHAR_H, C_BLUE);
            tft.setTextColor(C_WHITE, C_BLUE);
        } else {
            tft.fillRect(0, yTop, SCREEN_W, CHAR_H, C_WHITE);
            tft.setTextColor(C_BLACK, C_WHITE);
        }
        tft.setTextSize(1);
        tft.setCursor(6, yTop + 1);

        int tmax = CONT_COLS - 4;
        for (int c = 0; c < tmax && r.title[c]; c++) tft.print(r.title[c]);

        char badge[5]; snprintf(badge, 5, "#%d", idx + 1);
        tft.setCursor(SCREEN_W - (int)strlen(badge) * CHAR_W - 4, yTop + 1);
        tft.print(badge);

        tft.fillRect(0, yTop + CHAR_H, SCREEN_W, CHAR_H, C_WHITE);
        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor(6, yTop + CHAR_H + 1);
        const char* u = r.url;
        if (strncmp(u, "https://", 8) == 0) u += 8;
        else if (strncmp(u, "http://", 7) == 0) u += 7;
        for (int c = 0; c < CONT_COLS - 1 && u[c]; c++) tft.print(u[c]);

        tft.fillRect(0, yTop + CHAR_H * 2, SCREEN_W, CHAR_H, C_WHITE);
        tft.setTextColor(C_LTGRAY, C_WHITE);
        tft.setCursor(6, yTop + CHAR_H * 2 + 1);
        for (int c = 0; c < CONT_COLS - 1 && r.snippet[c]; c++) tft.print(r.snippet[c]);

        tft.drawFastHLine(0, yTop + blockH + 1, SCREEN_W, C_LTGRAY);
    }

    char nav[20];
    snprintf(nav, 20, "%d-%d / %d",
             g_resultScroll + 1,
             min(g_resultScroll + RESULTS_PER_PAGE, g_resultCount),
             g_resultCount);
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor(SCREEN_W - (int)strlen(nav) * CHAR_W - 4, HINT_Y - CHAR_H - 1);
    tft.print(nav);
}

static String enterText(const char* ttl, const char* hint, const char* placeholder) {
    tft.fillScreen(C_WHITE);
    drawStatusBar();
    drawHintBar(hint);

    tft.setTextSize(1);
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor((SCREEN_W - (int)strlen(ttl) * CHAR_W) / 2, CONT_Y + 20);
    tft.print(ttl);

    tft.setTextColor(C_LTGRAY, C_WHITE);
    tft.setCursor((SCREEN_W - (int)strlen(placeholder) * CHAR_W) / 2, CONT_Y + 40);
    tft.print(placeholder);

    int boxY = CONT_Y + 70;
    tft.drawRect(SB_X,   boxY,   SB_W,   SB_H,   C_ORANGE);
    tft.drawRect(SB_X+1, boxY+1, SB_W-2, SB_H-2, C_ORANGE);
    tft.fillRect(SB_X+2, boxY+2, SB_W-4, SB_H-4, C_WHITE);

    String text;
    while (true) {
        char k = readKey(); if (!k) { delay(10); continue; }
        if (k == '\n' || k == '\r') break;
        if (k == 27) { text = ""; break; }
        if ((k == 8 || k == 127) && text.length() > 0) {
            text.remove(text.length() - 1);
            tft.fillRect(SB_X+2, boxY+2, SB_W-4, SB_H-4, C_WHITE);
            tft.setTextColor(C_BLACK, C_WHITE);
            tft.setCursor(SB_X + 6, boxY + 5);
            for (int i = 0; i < (int)text.length(); i++) tft.print(text[i]);
        } else if (k >= 32 && k < 127) {
            text += (char)k;
            tft.setTextColor(C_BLACK, C_WHITE);
            int cx = SB_X + 6 + (int)(text.length() - 1) * CHAR_W;
            if (cx + CHAR_W < SB_X + SB_W - 4) {
                tft.setCursor(cx, boxY + 5);
                tft.print(text[text.length() - 1]);
            }
        }
        delay(40);
    }
    return text;
}

enum StripState { SS_TEXT, SS_TAG, SS_SCRIPT, SS_STYLE, SS_HEAD, SS_COMMENT };
static StripState ss_state = SS_TEXT; static char ss_tagBuf[128]; static int ss_tagPos = 0;
static bool ss_inAnchor = false; static char entBuf[16]; static int entLen = 0;
static bool inEntity = false; static int ss_dashCount = 0;

static void stripInit() {
    ss_state = SS_TEXT; ss_tagPos = 0; ss_inAnchor = false;
    inEntity = false; entLen = 0; ss_dashCount = 0;
    memset(ss_tagBuf, 0, sizeof(ss_tagBuf)); g_pageLen = 0; g_linkCount = 0;
}
static void sw(char c) { if (g_pageLen < PSRAM_PAGE_SIZE-2) g_pageText[g_pageLen++] = c; }
static void sws(const char* s) { while (*s) sw(*s++); }

static char decodeEnt(const char* e, int len) {
    if (len < 3) return ' ';
    char inner[16] = {}; int cl = min(len-2, 14); memcpy(inner, e+1, cl);
    if (!strcmp(inner,"amp"))   return '&'; if (!strcmp(inner,"nbsp")) return ' ';
    if (!strcmp(inner,"lt"))    return '<'; if (!strcmp(inner,"gt"))   return '>';
    if (!strcmp(inner,"quot"))  return '"'; if (!strcmp(inner,"apos")) return '\'';
    if (!strcmp(inner,"mdash")) return '-'; if (!strcmp(inner,"ndash")) return '-';
    if (!strcmp(inner,"hellip")) return '.';
    if (inner[0] == '#') { int c2 = atoi(inner+1); if (c2>=32&&c2<128) return (char)c2; }
    return ' ';
}

static bool isBlockTag(const char* n) {
    return (!strcmp(n,"p")||!strcmp(n,"div")||!strcmp(n,"br")||!strcmp(n,"h1")||
            !strcmp(n,"h2")||!strcmp(n,"h3")||!strcmp(n,"h4")||!strcmp(n,"li")||
            !strcmp(n,"tr")||!strcmp(n,"td")||!strcmp(n,"th")||!strcmp(n,"article")||
            !strcmp(n,"section")||!strcmp(n,"header")||!strcmp(n,"footer")||
            !strcmp(n,"nav")||!strcmp(n,"main"));
}

static bool extractHref(const char* tag, char* out, int outLen) {
    const char* p = strstr(tag, "href"); if (!p) return false; p += 4;
    while (*p == ' ' || *p == '=') p++;
    char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
    int i = 0;
    while (*p && i < outLen-1) {
        if (quote && *p == quote) break;
        if (!quote && (*p == ' ' || *p == '>')) break;
        out[i++] = *p++;
    }
    out[i] = 0; return i > 0;
}

static String resolveURL(const char* href) {
    String h(href);
    if (h.startsWith("http://") || h.startsWith("https://")) return h;
    if (h.startsWith("//")) return "https:" + h;
    if (h.startsWith("/"))  return baseDomain + h;
    if (h.startsWith("#"))  return currentURL;
    return baseDomain + "/" + h;
}

static void stripFeed(char c) {
    switch (ss_state) {
        case SS_SCRIPT:
            if (c=='<') ss_tagPos=0;
            else if (ss_tagPos==0&&c=='/') ss_tagPos=1;
            else if (ss_tagPos==1) { ss_tagBuf[0]=c; ss_tagBuf[1]=0; if(tolower(c)=='s') ss_tagPos=2; else ss_tagPos=0; }
            else if (ss_tagPos>=2&&ss_tagPos<8) {
                ss_tagBuf[ss_tagPos-1]=c; ss_tagBuf[ss_tagPos]=0;
                if (tolower(c)=='>') { ss_state=SS_TEXT; ss_tagPos=0; }
                else if (!strncasecmp(ss_tagBuf,"script",ss_tagPos-1)) ss_tagPos++;
                else ss_tagPos=0;
            }
            return;
        case SS_STYLE:  if (c=='<') ss_tagPos=0; return;
        case SS_HEAD:   if (c=='<') ss_tagPos=0; return;
        case SS_COMMENT:
            if (c=='-') ss_dashCount++;
            else if (c=='>'&&ss_dashCount>=2) { ss_state=SS_TEXT; ss_dashCount=0; }
            else ss_dashCount=0;
            return;
        case SS_TAG:
            if (c=='>') {
                ss_tagBuf[ss_tagPos<127?ss_tagPos:127]=0;
                char* tag=ss_tagBuf;
                if (tag[0]=='!') { ss_state=SS_TEXT; ss_tagPos=0; return; }
                bool closing=(tag[0]=='/'); if (closing) tag++;
                char name[32]={};int ni=0;
                while (tag[ni]&&tag[ni]!=' '&&ni<31) { name[ni]=(char)tolower(tag[ni]); ni++; }
                if (!closing) {
                    if      (!strcmp(name,"script")) ss_state=SS_SCRIPT;
                    else if (!strcmp(name,"style"))  ss_state=SS_STYLE;
                    else if (!strcmp(name,"head"))   ss_state=SS_HEAD;
                    else if (!strcmp(name,"a")) {
                        char href[LINK_URL_LEN]={};
                        if (extractHref(tag,href,LINK_URL_LEN)&&g_linkCount<MAX_LINKS) {
                            String res=resolveURL(href);
                            if (!res.startsWith("javascript")&&!res.startsWith("mailto")) {
                                strncpy(g_links[g_linkCount].url,res.c_str(),LINK_URL_LEN-1);
                                g_linkCount++;
                                char lbl[5]; snprintf(lbl,5,"[%d]",g_linkCount);
                                sws(lbl); ss_inAnchor=true;
                            }
                        }
                        ss_state=SS_TEXT;
                    } else {
                        ss_state=SS_TEXT;
                        if (isBlockTag(name)&&g_pageLen>0&&g_pageText[g_pageLen-1]!='\n') sw('\n');
                    }
                } else {
                    ss_state=SS_TEXT;
                    if (!strcmp(name,"a")) ss_inAnchor=false;
                    if (isBlockTag(name)&&g_pageLen>0&&g_pageText[g_pageLen-1]!='\n') sw('\n');
                }
                ss_tagPos=0;
            } else {
                if (ss_tagPos==1&&ss_tagBuf[0]=='!'&&c=='-') { ss_state=SS_COMMENT; ss_tagPos=0; return; }
                if (ss_tagPos<127) ss_tagBuf[ss_tagPos++]=c;
            }
            return;
        case SS_TEXT:
            if (c=='<') { ss_state=SS_TAG; ss_tagPos=0; if(inEntity){inEntity=false;entLen=0;} return; }
            if (c=='&') { inEntity=true; entLen=0; entBuf[entLen++]=c; return; }
            if (inEntity) {
                if (entLen<15) entBuf[entLen++]=c;
                if (c==';') {
                    entBuf[entLen]=0; char d=decodeEnt(entBuf,entLen);
                    if (d==' ') { if(g_pageLen>0&&g_pageText[g_pageLen-1]!=' ') sw(' '); }
                    else if (d) sw(d);
                    inEntity=false; entLen=0;
                } else if (entLen>12||c==' '||c=='\n') { inEntity=false; entLen=0; }
                return;
            }
            if (c=='\r') return;
            if (c=='\t') c=' ';
            if (c=='\n'&&g_pageLen>0&&g_pageText[g_pageLen-1]=='\n') return;
            if (c==' '&&g_pageLen>0&&g_pageText[g_pageLen-1]==' ') return;
            if ((unsigned char)c<32&&c!='\n') return;
            sw(c); return;
    }
}

static int readChunkSize(Stream* s) {
    unsigned long t=millis(); char buf[16]; int pos=0; bool cr=false;
    while (millis()-t<3000) {
        if (!s->available()){delay(1);continue;}
        char c=s->read(); if(c=='\r'){cr=true;continue;} if(c=='\n'&&cr) break;
        if(pos<14) buf[pos++]=c; cr=false;
    }
    buf[pos]=0; return (int)strtol(buf,nullptr,16);
}
#define MAX_RAW                   400000
#define STREAM_FIRST_BYTE_TIMEOUT   8000
#define STREAM_IDLE_TIMEOUT         4000

static bool readStream(Stream* s, int contentLen, bool chunked) {
    stripInit(); uint8_t buf[512]; int total=0;
    unsigned long fbw=millis();
    while (!s->available()&&millis()-fbw<STREAM_FIRST_BYTE_TIMEOUT) delay(5);
    if (!s->available()) return false;
    if (chunked) {
        while (total<MAX_RAW) {
            int csz=readChunkSize(s); if(csz<=0) break;
            int rem=csz;
            while (rem>0&&total<MAX_RAW) {
                unsigned long tw=millis();
                while (!s->available()&&millis()-tw<STREAM_IDLE_TIMEOUT) delay(2);
                if (!s->available()) break;
                int toRead=min(min(rem,(int)sizeof(buf)),s->available());
                s->setTimeout(STREAM_IDLE_TIMEOUT);
                int got=(int)s->readBytes(buf,toRead); if(got<=0) break;
                for(int i=0;i<got;i++) stripFeed((char)buf[i]);
                rem-=got; total+=got;
            }
            unsigned long tc=millis(); int nl=0;
            while (nl<2&&millis()-tc<800) {
                if(s->available()){char c=s->read();if(c=='\r'||c=='\n')nl++;else break;}
                else delay(1);
            }
        }
    } else {
        int rem=(contentLen>0)?contentLen:MAX_RAW;
        unsigned long lastData=millis();
        while (rem>0&&total<MAX_RAW) {
            if (s->available()) {
                int toRead=min(min(rem,(int)sizeof(buf)),s->available());
                s->setTimeout(STREAM_IDLE_TIMEOUT);
                int got=(int)s->readBytes(buf,toRead); if(got<=0) break;
                for(int i=0;i<got;i++) stripFeed((char)buf[i]);
                rem-=got; total+=got; lastData=millis();
            } else { if(millis()-lastData>STREAM_IDLE_TIMEOUT) break; delay(5); }
        }
    }
    if (g_pageLen<PSRAM_PAGE_SIZE-1) g_pageText[g_pageLen]=0;
    return g_pageLen>5;
}

static void buildLineCache() {
    g_lineCount=0; if(!g_pageText||g_pageLen==0) return;
    uint32_t pos=0;
    while (pos<g_pageLen&&g_lineCount<MAX_LINES) {
        uint32_t ls=pos; int col=0; bool wrapped=false;
        while (pos<g_pageLen) {
            char c=g_pageText[pos];
            if (c=='\n') {
                uint16_t len=(uint16_t)(pos-ls);
                if(len>0) g_lines[g_lineCount++]={ls,len};
                pos++; wrapped=false; break;
            }
            col++; pos++;
            if (col>=CONT_COLS) {
                uint32_t wrapAt=pos;
                for(int b=(int)pos-1;b>(int)ls;b--) if(g_pageText[b]==' '){wrapAt=(uint32_t)(b+1);break;}
                uint16_t len=(uint16_t)(wrapAt-ls); if(len==0) len=(uint16_t)(pos-ls);
                if(g_lineCount<MAX_LINES) g_lines[g_lineCount++]={ls,len};
                pos=ls+len; while(pos<g_pageLen&&g_pageText[pos]==' ') pos++;
                wrapped=true; break;
            }
        }
        if (!wrapped&&pos>=g_pageLen&&ls<g_pageLen) {
            uint16_t len=(uint16_t)(g_pageLen-ls);
            if(len>0&&g_lineCount<MAX_LINES) g_lines[g_lineCount++]={ls,len}; break;
        }
    }
}

static void updateBaseDomain(const String& url) {
    int se=url.indexOf("://"); if(se<0){baseDomain="https://"+url;return;}
    int he=url.indexOf('/',se+3); baseDomain=(he<0)?url:url.substring(0,he);
}

static void fetchStatus(const char* line1, const char* line2 = nullptr) {

    tft.setTextSize(1);
    tft.fillRect(0, CONT_Y+60, SCREEN_W, CHAR_H*2+4, C_WHITE);
    if (line1) {
        tft.setTextColor(C_DKGRAY, C_WHITE);
        tft.setCursor((SCREEN_W-(int)strlen(line1)*CHAR_W)/2, CONT_Y+60);
        tft.print(line1);
    }
    if (line2) {
        tft.setTextColor(C_LTGRAY, C_WHITE);
        int l2 = min((int)strlen(line2), CONT_COLS);
        tft.setCursor((SCREEN_W - l2*CHAR_W)/2, CONT_Y+78);
        for(int i=0;i<l2;i++) tft.print(line2[i]);
    }
}

static bool jinaFetch(const String& targetURL, const char* statusLine) {
    String jinaURL = "https://r.jina.ai/" + targetURL;
    HTTPClient http;
    const char* hdrs[] = {"Transfer-Encoding","Content-Encoding"};
    http.collectHeaders(hdrs,2); http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("Mozilla/5.0 (compatible; CanuckWeb/3.0)");
    http.addHeader("Accept-Encoding","identity");
    http.addHeader("X-Return-Format","text");
    http.addHeader("X-No-Cache","true");
    if (!g_ssl) { g_ssl=new WiFiClientSecure(); g_ssl->setInsecure(); }
    http.begin(*g_ssl,jinaURL);
    fetchStatus(statusLine);
    int code=http.GET();
    char codeStr[20]; snprintf(codeStr,20,"HTTP %d",code);
    fetchStatus(statusLine, codeStr);
    if (code!=200) { http.end(); return false; }
    String te=http.header("Transfer-Encoding"); te.toLowerCase();
    bool chunked=(te.indexOf("chunked")>=0);
    int cLen=http.getSize(); Stream* s=&http.getStream();
    bool ok=readStream(s,cLen,chunked); http.end();
    return ok&&g_pageLen>20;
}

static bool pageIsBlocked() {
    if (g_pageLen<10) return true;
    char buf[2049]; int scan=min((int)g_pageLen,2048);
    for(int i=0;i<scan;i++) buf[i]=tolower(g_pageText[i]); buf[scan]=0;
    const char* sigs[]={
        "enable javascript","please enable","access denied","subscribe to continue",
        "subscribe to read","sign in to read","create an account","log in to continue",
        "you've reached your","premium content","403 forbidden","just a moment",
        "checking your browser","ddos protection","ray id","verifying you are human",nullptr
    };
    for(int i=0;sigs[i];i++) if(strstr(buf,sigs[i])) return true;
    return false;
}

static bool waybackFetch(const String& url) {
    fetchStatus("Trying Wayback Machine...");
    String cdxURL="http://archive.org/wayback/available?url="+url;
    HTTPClient cdxHttp; cdxHttp.setTimeout(10000);
    cdxHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    cdxHttp.setUserAgent("Mozilla/5.0 (compatible; CanuckWeb/3.0)");
    static WiFiClient plainClient; cdxHttp.begin(plainClient,cdxURL);
    int cdxCode=cdxHttp.GET();
    if (cdxCode!=200) { cdxHttp.end(); fetchStatus("Wayback unavailable"); delay(2000); return false; }
    String cdxBody=cdxHttp.getString(); cdxHttp.end();
    int urlIdx=cdxBody.indexOf("\"url\":\""); if(urlIdx<0){ fetchStatus("Not in Wayback"); delay(2500); return false; }
    urlIdx+=7; int urlEnd=cdxBody.indexOf('"',urlIdx); if(urlEnd<0) return false;
    String archiveURL=cdxBody.substring(urlIdx,urlEnd); archiveURL.replace("\\/","/");
    return jinaFetch(archiveURL,"via Wayback + Jina...");
}

static bool fetchPage(const String& url) {
    currentURL=url; updateBaseDomain(url);
    tft.fillScreen(C_WHITE);
    drawStatusBar("Loading...");
    drawHintBar("Please wait...");
    tft.setTextSize(1);

    tft.setTextColor(C_BLACK, C_WHITE);
    tft.setCursor((SCREEN_W-12*CHAR_W)/2, CONT_Y+30);
    tft.print("Loading page");

    String disp=url; if(disp.startsWith("https://")) disp=disp.substring(8);
    if((int)disp.length()>CONT_COLS) disp=disp.substring(0,CONT_COLS-3)+"...";
    tft.setTextColor(C_DKGRAY, C_WHITE);
    tft.setCursor((SCREEN_W-(int)disp.length()*CHAR_W)/2, CONT_Y+48);
    tft.print(disp);

    bool ok=jinaFetch(url,"via r.jina.ai...");
    if (ok&&pageIsBlocked()) { ok=waybackFetch(url); }
    if (!ok) ok=waybackFetch(url);
    if (!ok||g_pageLen<20) {
        fetchStatus("Page unavailable");
        delay(3000); return false;
    }
    buildLineCache(); scrollPos=0; return true;
}

static void displayPage() {

    tft.fillRect(0, CONT_Y, SCREEN_W, CONT_H, C_WHITE);
    int maxS = max(0, g_lineCount - CONT_ROWS);
    scrollPos = constrain(scrollPos, 0, maxS);

    tft.setTextSize(1);
    for (int row = 0; row < CONT_ROWS; row++) {
        int li = scrollPos + row; if (li >= g_lineCount) break;
        LineSpan& ls = g_lines[li]; if (ls.len == 0) continue;
        int pl = min((int)ls.len, CONT_COLS - 1);
        tft.setTextColor(C_BLACK, C_WHITE);
        tft.setCursor(0, CONT_Y + row * CHAR_H);
        for (int i = 0; i < pl && g_pageText[ls.start+i]; i++)
            tft.print(g_pageText[ls.start+i]);
    }

    if (g_lineCount > CONT_ROWS) {
        int barH   = CONT_H;
        int thumbH = max(4, barH * CONT_ROWS / g_lineCount);
        int thumbY = CONT_Y + (barH - thumbH) * scrollPos / max(1, maxS);
        tft.fillRect(SCREEN_W-3, CONT_Y, 3, barH, C_LTGRAY);
        tft.fillRect(SCREEN_W-3, thumbY, 3, thumbH, C_BLACK);
    }

    drawStatusBar();
    if (g_linkCount > 0) {
        char hint[52]; snprintf(hint,52,"1-%d:link  N:URL  B:back  R:reload  S:srch",g_linkCount);
        drawHintBar(hint);
    } else {
        drawHintBar("N:URL  B:back  R:reload  S:search  Q:quit");
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BOARD_POWERON, OUTPUT); digitalWrite(BOARD_POWERON, HIGH); delay(100);
    pinMode(BAT_ADC, ANALOG);
    tft.init(); tft.setRotation(1); tft.fillScreen(C_WHITE);
    Wire.begin(I2C_SDA, I2C_SCL);
    pinMode(TB_UP, INPUT_PULLUP);
    pinMode(TB_DOWN, INPUT_PULLUP);
    pinMode(TB_CLICK, INPUT_PULLUP);

    g_pageText = (char*)heap_caps_malloc(PSRAM_PAGE_SIZE, MALLOC_CAP_SPIRAM);
    g_lines    = (LineSpan*)heap_caps_malloc(MAX_LINES * sizeof(LineSpan), MALLOC_CAP_SPIRAM);
    g_links    = (LinkEntry*)heap_caps_malloc(MAX_LINKS * sizeof(LinkEntry), MALLOC_CAP_SPIRAM);
    if (!g_pageText) g_pageText = (char*)malloc(32 * 1024);
    if (!g_lines)    g_lines    = (LineSpan*)malloc(MAX_LINES * sizeof(LineSpan));
    if (!g_links)    g_links    = (LinkEntry*)malloc(MAX_LINKS * sizeof(LinkEntry));

    drawBoot(0);
    for (int p = 1; p <= 100; p++) { drawBoot(p); delay(100); }
    delay(300);

    WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);

    if (loadAndConnect()) {
        appState = STATE_SEARCH_IDLE;
        drawIdleScreen();
    } else {
        doWifiScan();
        drawWifiList();
        appState = STATE_WIFI_SCAN;
    }
}

void loop() {
    if (appState == STATE_PAGE_VIEW && millis() - lastStatusMs > STATUS_INTERVAL) {
        drawStatusBar(); lastStatusMs = millis();
    }

    static bool lastUp = HIGH, lastDown = HIGH, lastClick = HIGH;
    bool up    = digitalRead(TB_UP);
    bool dn    = digitalRead(TB_DOWN);
    bool click = digitalRead(TB_CLICK);
    char key   = readKey();

    if (appState == STATE_WIFI_SCAN) {
        if (up == LOW && lastUp == HIGH) {
            if (wifiSelected > 0) { wifiSelected--; if(wifiSelected<wifiScrollOff) wifiScrollOff--; drawWifiList(); }
            delay(150);
        }
        if (dn == LOW && lastDown == HIGH) {
            if (wifiSelected < wifiCount - 1) {
                wifiSelected++;
                if (wifiSelected >= wifiScrollOff + 9) wifiScrollOff++;
                drawWifiList();
            }
            delay(150);
        }
        if (key == '\n' || key == '\r' || (click == LOW && lastClick == HIGH)) {
            if (wifiCount == 0) { doWifiScan(); drawWifiList(); goto end; }
            String ssid = wifiSSIDs[wifiSelected];
            String pw   = enterPassword(ssid);
            if (connectWifi(ssid, pw)) {
                saveCredentials(ssid, pw);
                appState = STATE_SEARCH_IDLE;
                drawIdleScreen();
            } else {
                drawWifiList();
            }
        }
        if (key == 'r' || key == 'R') { doWifiScan(); drawWifiList(); }

    } else if (appState == STATE_SEARCH_IDLE) {
        bool redraw = false;
        bool searching = false;
        if (key == '\n' || key == '\r') {
            if (g_searchQuery.length() > 0) {
                searching = true;
                tft.fillScreen(C_WHITE);
                doSearch(g_searchQuery);
                drawResults();
                appState = STATE_RESULTS;
            }
        } else if (key == 27) {
            g_searchQuery = ""; redraw = true;
        } else if ((key == 8 || key == 127) && g_searchQuery.length() > 0) {
            g_searchQuery.remove(g_searchQuery.length() - 1); redraw = true;
        } else if (key >= ' ' && key < 127) {
            g_searchQuery += (char)key; redraw = true;
        } else if (key == 'n' || key == 'N') {
            String url = enterText("Enter URL", "Type URL  ENTER=go  ESC=cancel", "https://");
            if (url.length() > 0) {
                if (!url.startsWith("http")) url = "https://" + url;
                if (fetchPage(url)) {
                    urlHistory[0] = url; historyCount = 1;
                    displayPage(); lastStatusMs = millis();
                    appState = STATE_PAGE_VIEW;
                } else drawIdleScreen();
            } else drawIdleScreen();
            goto end;
        }
        static unsigned long lastBlink = 0;
        if (!searching && (redraw || millis() - lastBlink > 480)) {
            drawSearchBox(SB_Y, g_searchQuery, true);
            lastBlink = millis();
        }

    } else if (appState == STATE_RESULTS) {
        bool redraw = false;
        if (dn == LOW && lastDown == HIGH) {
            if (g_resultCursor < g_resultCount - 1) {
                g_resultCursor++;
                if (g_resultCursor >= g_resultScroll + RESULTS_PER_PAGE)
                    g_resultScroll = g_resultCursor - RESULTS_PER_PAGE + 1;
                redraw = true;
            }
            delay(150);
        }
        if (up == LOW && lastUp == HIGH) {
            if (g_resultCursor > 0) {
                g_resultCursor--;
                if (g_resultCursor < g_resultScroll) g_resultScroll = g_resultCursor;
                redraw = true;
            }
            delay(150);
        }
        if ((click == LOW && lastClick == HIGH) || key == '\n' || key == '\r') {
            if (g_resultCursor < g_resultCount) {
                String url = String(g_results[g_resultCursor].url);
                if (url.length() > 0 && fetchPage(url)) {
                    if (historyCount < HISTORY_MAX) urlHistory[historyCount++] = url;
                    displayPage(); lastStatusMs = millis();
                    appState = STATE_PAGE_VIEW;
                } else drawResults();
            }
            goto end;
        }
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < g_resultCount) {
                g_resultCursor = idx;
                String url = String(g_results[idx].url);
                if (url.length() > 0 && fetchPage(url)) {
                    if (historyCount < HISTORY_MAX) urlHistory[historyCount++] = url;
                    displayPage(); lastStatusMs = millis(); appState = STATE_PAGE_VIEW;
                } else drawResults();
            }
        } else if (key == 's' || key == 'S' || key == '/') {
            appState = STATE_SEARCH_IDLE; drawIdleScreen();
        } else if (key == 'b' || key == 'B') {
            appState = STATE_SEARCH_IDLE; drawIdleScreen();
        } else if (key == 'n' || key == 'N') {
            String url = enterText("Enter URL","Type URL  ENTER=go  ESC=cancel","https://");
            if (url.length() > 0) {
                if (!url.startsWith("http")) url = "https://" + url;
                if (fetchPage(url)) {
                    if (historyCount < HISTORY_MAX) urlHistory[historyCount++] = url;
                    displayPage(); lastStatusMs = millis(); appState = STATE_PAGE_VIEW;
                } else drawResults();
            } else drawResults();
        }
        if (redraw) drawResults();

    } else if (appState == STATE_PAGE_VIEW) {
        if (up == LOW && lastUp == HIGH) {
            if (scrollPos > 0) { scrollPos--; displayPage(); } delay(80);
        }
        if (dn == LOW && lastDown == HIGH) {
            int ms = max(0, g_lineCount - CONT_ROWS);
            if (scrollPos < ms) { scrollPos++; displayPage(); } delay(80);
        }
        if (key >= '1' && key <= '9') {
            int li = key - '1';
            if (li < g_linkCount) {
                String lu = String(g_links[li].url);
                if (fetchPage(lu)) {
                    if (historyCount < HISTORY_MAX) urlHistory[historyCount++] = lu;
                    displayPage(); lastStatusMs = millis();
                } else displayPage();
            }
        } else if (key == 'b' || key == 'B') {
            if (historyCount > 1) {
                historyCount--;
                String prev = urlHistory[historyCount - 1];
                if (fetchPage(prev)) { displayPage(); lastStatusMs = millis(); }
            } else {
                if (g_resultCount > 0) { drawResults(); appState = STATE_RESULTS; }
                else { drawIdleScreen(); appState = STATE_SEARCH_IDLE; }
            }
        } else if (key == 'r' || key == 'R') {
            if (!currentURL.isEmpty() && fetchPage(currentURL)) { displayPage(); lastStatusMs = millis(); }
        } else if (key == 'n' || key == 'N') {
            String url = enterText("Enter URL","Type URL  ENTER=go  ESC=cancel","https://");
            if (url.length() > 0) {
                if (!url.startsWith("http")) url = "https://" + url;
                if (fetchPage(url)) {
                    if (historyCount < HISTORY_MAX) urlHistory[historyCount++] = url;
                    displayPage(); lastStatusMs = millis();
                } else displayPage();
            } else displayPage();
        } else if (key == 's' || key == 'S' || key == '/') {
            appState = STATE_SEARCH_IDLE; drawIdleScreen();
        } else if (key == 'q' || key == 'Q') {
            ESP.restart();
        }
    }

end:
    lastUp = up; lastDown = dn; lastClick = click;
    delay(10);
}
