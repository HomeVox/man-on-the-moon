/*
 * Modus Aarde en Maan: een draaiende bol midden op een zwart scherm, gestileerd uit NASA-kaarten (publiek domein).
 * Aarde (modus 3):
 *  - belicht van linksboven, zacht donkerder naar de rand rechtsonder; blauwe atmosfeer als waas op de bol;
 *  - draait langzaam; met de vinger draai je mee (loslaten geeft een zwiep).
 * Maan (modus 4):
 *  - voorkant naar ons toe, met de echte schijngestalte (tijd via SNTP; de RTC van de StopWatch klopt niet);
 *  - met de vinger draai je hem rond, daarna veert hij terug naar de voorkant.
 * Mars (modus 5) en Jupiter (modus 6): zelfde licht als de maan, draaien vanzelf (Jupiter sneller); vinger draait mee.
 * De bollen blijven waterpas als je de StopWatch draait; de tijd staat klein boven de bol (schakelaar Klok in HA).
 * Per schermpixel ligt eenmalig vast welke kaartplek erachter zit en hoeveel licht er valt; per beeld schuift alleen de kaart.
 */
#include "jarvis_view.h"
#include <hal/hal.h>
#include <M5GFX.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_timer.h>

extern const uint8_t aarde_dag_start[] asm("_binary_aarde_dag_jpg_start");
extern const uint8_t aarde_dag_end[] asm("_binary_aarde_dag_jpg_end");
extern const uint8_t aarde_nacht_start[] asm("_binary_aarde_nacht_jpg_start");   // NASA Black Marble (stadslichtjes)
extern const uint8_t aarde_nacht_end[] asm("_binary_aarde_nacht_jpg_end");
extern const uint8_t maan_start[] asm("_binary_maan_jpg_start");
extern const uint8_t maan_end[] asm("_binary_maan_jpg_end");
extern const uint8_t mars_start[] asm("_binary_mars_jpg_start");
extern const uint8_t mars_end[] asm("_binary_mars_jpg_end");
extern const uint8_t jupiter_start[] asm("_binary_jupiter_jpg_start");
extern const uint8_t jupiter_end[] asm("_binary_jupiter_jpg_end");

namespace jarvis {

static const char* TAG = "aarde";
static constexpr int kCx = JarvisView::Size / 2;
static constexpr float kTilt = 0.30f;          // noordpool iets naar de kijker
static constexpr uint16_t kBgE = 0x0000;       // zwart (AMOLED)
static constexpr float kEarthHome = 5.0f / 360.0f * 1024 + 512;   // begint met Europa/Afrika in beeld
static bool s_timeOk = false;                  // klok gezet via SNTP

// JPEG uitpakken (M5GFX/TJpgDec) naar RGB565 in een bestaand blok; het sprite-buffer staat byte-omgekeerd
static void decodeInto(const uint8_t* s, const uint8_t* e, uint16_t* dst, int W, int H)
{
    // rechtstreeks in de kaart uitpakken (geen tweede blok van 1 MB nodig), daarna de bytes omdraaien
    M5Canvas spr;
    spr.setBuffer(dst, W, H, 16);
    spr.drawJpg(s, e - s, 0, 0);
    spr.deleteSprite();   // de buffer is van ons: wordt niet vrijgegeven
    for (int i = 0; i < W * H; i++) { uint16_t v = dst[i]; dst[i] = (uint16_t)((v >> 8) | (v << 8)); }
}

static uint16_t* loadMap(const uint8_t* s, const uint8_t* e, int W, int H)
{
    uint16_t* dst = (uint16_t*)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst) decodeInto(s, e, dst, W, H);
    return dst;
}

// aardkaart rustiger: 40% richting grijs en iets donkerder, zodat het felle cyaan zacht wordt
static void zachter(uint16_t* p, int n)
{
    for (int i = 0; i < n; i++) {
        const uint16_t v = p[i];
        int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
        const int l = (r * 77 + g * 150 + b * 29) >> 8;
        r = (r * 6 + l * 4) * 9 / 100; g = (g * 6 + l * 4) * 9 / 100; b = (b * 6 + l * 4) * 9 / 100;
        p[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

// welke schermpixels in de schijf liggen (zelfde voor aarde en maan)
void JarvisView::globeGeom()
{
    if (_eGeom) return;
    const int R = EarthR;
    int n = 0;
    for (int y = 0; y < Size; y++) {
        float dy = y + 0.5f - EarthCy;
        _eRow[y] = n;
        if (fabsf(dy) >= R) { _eX0[y] = 0; continue; }
        float half = sqrtf(R * R - dy * dy);
        int x0 = (int)ceilf(kCx - half), x1 = (int)floorf(kCx + half);
        _eX0[y] = x0;
        n += x1 - x0 + 1;
    }
    _eRow[Size] = n;
    _eGeom = true;
}

// klok via internet (de RTC klopt niet); tot de tijd binnen is een wassende maan
static void startClock()
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb([](struct timeval*) { s_timeOk = true; });
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // Nederlandse tijd, met zomertijd
    tzset();
    esp_sntp_init();
}

bool JarvisView::earthInit()
{
    int64_t t0 = esp_timer_get_time();
    globeGeom();
    _eDay = loadMap(aarde_dag_start, aarde_dag_end, EarthW, EarthH);
    if (!_eDay) return false;
    zachter(_eDay, EarthW * EarthH);
    _ePix = (uint32_t*)heap_caps_malloc(_eRow[Size] * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_ePix) return false;
    startClock();
    buildEarth(0.35f);
    ESP_LOGI(TAG, "klaar in %d ms, %d pixels in de schijf", (int)((esp_timer_get_time() - t0) / 1000), _eRow[Size]);
    return true;
}

// aarde: licht als de maan (zelfde schijngestalte, zelfde donkere kant), plus een dunne atmosfeer aan de dagkant
void JarvisView::buildEarth(float fase)
{
    const int W = EarthW, H = EarthH, R = EarthR;
    const float cT = cosf(kTilt), sT = sinf(kTilt);
    for (int y = 0; y < Size; y++) {
        int cnt = _eRow[y + 1] - _eRow[y];
        float ny = -(y + 0.5f - EarthCy) / R;
        for (int k = 0; k < cnt; k++) {
            float nx = (_eX0[y] + k + 0.5f - kCx) / R;
            float nz2 = 1.0f - nx * nx - ny * ny;
            float nz = nz2 > 0 ? sqrtf(nz2) : 0.0f;
            float ey = ny * cT + nz * sT, ez = -ny * sT + nz * cT;
            float lat = asinf(ey > 1 ? 1 : (ey < -1 ? -1 : ey)), lon = atan2f(nx, ez);
            int col = (int)((lon + 3.14159265f) / 6.2831853f * W) & (W - 1);
            int row = (int)((1.5707963f - lat) / 3.14159265f * H);
            row = row < 0 ? 0 : (row >= H ? H - 1 : row);
            // vol licht, geen schaduwkant (wens Mathijs 30-10); naar de rand iets donkerder zodat hij rond blijft
            float lit = 0.72f + 0.28f * sqrtf(nz);
            float edge = 1.0f - nz;                                             // dunne atmosfeer naar de rand
            float haze = edge * edge * 0.45f;
            float aa = (R - sqrtf((nx * nx + ny * ny)) * R);                    // zachte rand (laatste pixel)
            if (aa < 1.0f) lit *= (aa < 0 ? 0 : aa);
            int L8 = (int)(lit * 255.0f), H5 = (int)(haze * 31.0f);
            _ePix[_eRow[y] + k] = (uint32_t)col | ((uint32_t)row << 10) | ((uint32_t)(L8 > 255 ? 255 : L8) << 19) | ((uint32_t)(H5 > 31 ? 31 : H5) << 27);
        }
    }
    _eFase = fase;
}

// maan: rechtop (geen kanteling), licht volgens de schijngestalte. fase 0 = nieuwe maan, 0,5 = vol; wassend = rechts verlicht
void JarvisView::buildMoon(float fase)
{
    int64_t t0 = esp_timer_get_time();
    const int W = EarthW, H = EarthH, R = EarthR;
    for (int y = 0; y < Size; y++) {
        int cnt = _eRow[y + 1] - _eRow[y];
        float ny = -(y + 0.5f - EarthCy) / R;
        for (int k = 0; k < cnt; k++) {
            float nx = (_eX0[y] + k + 0.5f - kCx) / R;
            float nz2 = 1.0f - nx * nx - ny * ny;
            float nz = nz2 > 0 ? sqrtf(nz2) : 0.0f;
            float lat = asinf(ny > 1 ? 1 : (ny < -1 ? -1 : ny)), lon = atan2f(nx, nz);
            int col = (int)((lon + 3.14159265f) / 6.2831853f * W) & (W - 1);
            int row = (int)((1.5707963f - lat) / 3.14159265f * H);
            row = row < 0 ? 0 : (row >= H ? H - 1 : row);
            float lit = 0.72f + 0.28f * sqrtf(nz);                              // vol licht, geen schaduwkant (30-10)
            float aa = (R - sqrtf((nx * nx + ny * ny)) * R);
            if (aa < 1.0f) lit *= (aa < 0 ? 0 : aa);
            int L8 = (int)(lit * 255.0f);
            _mPix[_eRow[y] + k] = (uint32_t)col | ((uint32_t)row << 10) | ((uint32_t)(L8 > 255 ? 255 : L8) << 19);
        }
    }
    _mFase = fase;
    ESP_LOGI(TAG, "maan fase %.3f in %d ms", fase, (int)((esp_timer_get_time() - t0) / 1000));
}

// maan, Mars, Jupiter: zelfde tabel (rechtop, vol licht), elk een eigen kaart; kaarten blijven geladen
bool JarvisView::bodyInit(int b)
{
    static const uint8_t* const s[3] = {maan_start, mars_start, jupiter_start};
    static const uint8_t* const e[3] = {maan_end, mars_end, jupiter_end};
    globeGeom();
    if (!_mPix) {
        _mPix = (uint32_t*)heap_caps_malloc(_eRow[Size] * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_mPix) return false;
        startClock();
        buildMoon(0.35f);
    }
    if (!_bTex[b] && (_bTex[b] = loadMap(s[b], e[b], EarthW, EarthH))) zachter(_bTex[b], EarthW * EarthH);
    ESP_LOGI(TAG, "kaart bol %d: %s, PSRAM vrij %u KB, grootste blok %u KB", b, _bTex[b] ? "ok" : "MISLUKT",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    return _bTex[b] != nullptr;
}

bool JarvisView::globeReady() const
{
    return _mode >= 4 ? (_mPix && _bTex[_mode - 4]) : _eReady;
}

// schijngestalte uit de tijd: dagen sinds de nieuwe maan van 6 jan 2000 18:14 UTC, synodische maand 29,53 dagen
[[maybe_unused]] static float moonPhase(time_t now)   // niet meer gebruikt sinds de bollen vol verlicht zijn (30-10)
{
    double days = now / 86400.0 - 10962.6;
    double age = fmod(days, 29.530588853);
    if (age < 0) age += 29.530588853;
    return (float)(age / 29.530588853);
}

void JarvisView::computeEarth(float dt)
{
    const bool moon = _mode >= 4;                      // maan of planeet (zelfde tabel)
    if (moon) {
        const int b = _mode - 4;
        if (!_bTex[b] && !_bFailed[b]) {
            _bFailed[b] = !bodyInit(b);
            if (_bFailed[b]) ESP_LOGE(TAG, "geen geheugen of kaart voor bol %d", _mode);
        }
    } else if (!_eReady && !_eFailed) {
        _eReady = earthInit();
        _eFailed = !_eReady;
        if (_eFailed) ESP_LOGE(TAG, "geen geheugen of kaart");
    }
    if (!globeReady()) return;
    const int W = EarthW;

    if (_eBody != _mode) {   // andere bol: draaiing opnieuw
        _eBody = _mode;
        _spin = moon ? 0.0f : kEarthHome;
        _spinV = 0;
        _mCheck = 0;         // schijngestalte meteen bijwerken
    }
    // schijngestalte niet meer bijwerken: de bollen zijn vol verlicht (30-10)
    // nachtstand: de aarde 's nachts met stadslichtjes (zelfde blok als de dagkaart; wisselen kost ~0,3 s,
    // maar gebeurt als het scherm uit gaat of weer aan)
    if (!moon && _nacht != _eNightMap) {
        const int64_t t0 = esp_timer_get_time();
        _eNightMap = _nacht;
        if (_eNightMap) decodeInto(aarde_nacht_start, aarde_nacht_end, _eDay, EarthW, EarthH);
        else { decodeInto(aarde_dag_start, aarde_dag_end, _eDay, EarthW, EarthH); zachter(_eDay, EarthW * EarthH); }
        ESP_LOGI(TAG, "aardkaart %s in %d ms", _eNightMap ? "nacht" : "dag", (int)((esp_timer_get_time() - t0) / 1000));
    }

    // aarde: draait vanzelf langzaam (oppervlak loopt naar rechts, zoals de echte aarde); maan: veert terug naar de voorkant.
    // Slepen draait mee, loslaten geeft een zwiep. Kantelen laat hem rollen als een knikker: hoe schever, hoe sneller.
    const float kAuto = -(float)W / 90.0f;            // één rondje per anderhalve minuut
    const float pxToCol = (float)W / (6.2831853f * EarthR);
    const float th = _ang * _use3d, kc = cosf(th), ks = sinf(th);   // zelfde draaiing als het beeld (waterpas)
    const float kantel = ks;                          // -1..1: hoe scheef het apparaat staat
    if (_touchDown) {
        if (_ePrevDown && dt > 0) {
            // vingerbeweging in de richting van de bol, niet van het scherm: klopt ook als hij scheef staat
            float dc = -(kc * (_tgx - _ePrevX) - ks * (_tgy - _ePrevY)) * pxToCol;
            _spin += dc;
            _spinV = 0.6f * _spinV + 0.4f * (dc / dt);
        }
        _ePrevX = _tgx;
        _ePrevY = _tgy;
    } else if (_mode == 4) {
        float d = kantel * W * 0.2f - _spin;           // kortste weg terug naar de voorkant (gekanteld: wat opzij)
        while (d > W / 2) d -= W;
        while (d < -W / 2) d += W;
        _spinV += (2.5f * d - 3.2f * _spinV) * dt;
        _spin += _spinV * dt;
    } else {
        const float auto_ = (_mode == 6 ? kAuto * 2.5f : kAuto) + kantel * W * 0.6f;   // Jupiter draait snel
        _spinV += (auto_ - _spinV) * (1.0f - expf(-dt * 0.7f));
        _spin += _spinV * dt;
    }
    _ePrevDown = _touchDown;
    _spin = fmodf(_spin, (float)W);
    if (_spin < 0) _spin += W;
    _eSu = (int)_spin;
    _eGlowAmt = 1.0f;   // atmosfeer vast: reageert niet op geluid (wens Mathijs 28-10)
}

bool JarvisView::tijdBekend() { return s_timeOk; }

// klok boven de bol: alleen de tijd, klein en wit; alleen als de tijd via internet binnen is
void JarvisView::updateKlok()
{
    if (!_klokOn || !s_timeOk) { _klokShow = false; return; }
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    if (_klokShow && tm.tm_min == _klokMin) return;
    if (!_klok) _klok = (uint16_t*)heap_caps_malloc((size_t)Size * KlokH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_klok) return;
    char tijd[8];
    snprintf(tijd, sizeof(tijd), "%02d:%02d", tm.tm_hour, tm.tm_min);
    // dubbel zo groot tekenen en 2x2 middelen: zachte randen
    const int W2 = Size * 2, H2 = KlokH * 2;
    M5Canvas spr;
    spr.setPsram(true);
    spr.setColorDepth(16);
    if (!spr.createSprite(W2, H2)) return;
    spr.fillSprite(0);
    spr.setTextDatum(textdatum_t::top_center);
    spr.setFont(&fonts::DejaVu72);   // strak schreefloos, zoals een horlogeklok
    spr.setTextSize(1.1f);
    spr.setTextColor(spr.color565(225, 230, 235));
    spr.drawString(tijd, W2 / 2, 4);
    const uint16_t* sb = (const uint16_t*)spr.getBuffer();
    auto px = [&](int x, int y, int& r, int& g, int& b) {
        uint16_t v = sb[y * W2 + x];
        v = (uint16_t)((v >> 8) | (v << 8));
        r += (v >> 11) & 31; g += (v >> 5) & 63; b += v & 31;
    };
    for (int y = 0; y < KlokH; y++)
        for (int x = 0; x < Size; x++) {
            int r = 0, g = 0, b = 0;
            px(2 * x, 2 * y, r, g, b); px(2 * x + 1, 2 * y, r, g, b);
            px(2 * x, 2 * y + 1, r, g, b); px(2 * x + 1, 2 * y + 1, r, g, b);
            _klok[y * Size + x] = (uint16_t)(((r >> 2) << 11) | ((g >> 2) << 5) | (b >> 2));
        }
    spr.deleteSprite();
    _klokMin = tm.tm_min;
    _klokShow = true;
}

void JarvisView::renderEarthHalf(Worker& w)
{
    const int W = EarthW, RG = EarthR + 1;
    const int rows0 = w.half * (Size / 2), rows1 = w.half ? Size : Size / 2;
    uint16_t* dst = _buf[_render];
    const bool moon = _rp.earth > 1.5f;                // maan of planeet: earth = 2 + nummer
    const uint16_t* tex = moon ? _bTex[(int)(_rp.earth + 0.5f) - 2] : _eDay;
    const uint32_t* pix = moon ? _mPix : _ePix;
    const int su = _eSu;
    const int g8 = (int)(_eGlowAmt * 256.0f);
    const int mb = (int)(_rp.bright * 330.0f);   // helderheid van de toestand
    const bool rot = fabsf(_rp.ang) > 0.002f;           // waterpas: bol draait tegen het apparaat in
    const int rc = (int)(cosf(_rp.ang) * 65536.0f), rs = (int)(sinf(_rp.ang) * 65536.0f);
    // klok erover (vast op het scherm, niet waterpas: zo blijft hij leesbaar)
    const uint16_t* klok = _klokShow ? _klok : nullptr;
    // puntjes onderaan: welke planeet (Aarde, Maan, Mars, Jupiter), de huidige wit
    const int dotOn = _mode - 3;
    auto addDots = [&](uint16_t* row, int y) {
        const float dy = y + 0.5f - DotY;
        for (int i = 0; i < 4; i++) {
            const int cx = Size / 2 - DotGap * 3 / 2 + i * DotGap;
            for (int x = cx - 5; x <= cx + 5; x++) {
                const float dx = x + 0.5f - cx, a = 4.0f - sqrtf(dx * dx + dy * dy);   // straal 3.5 met zachte rand
                if (a <= 0.0f) continue;
                const int l = (int)((a > 1.0f ? 1.0f : a) * (i == dotOn ? 225.0f : 70.0f));
                row[x] = (uint16_t)(((l >> 3) << 11) | ((l >> 2) << 5) | (l >> 3));
            }
        }
    };
    // nachtklok: elke pixel naar warm amber op dezelfde helderheid, bijna geen blauw licht
    const bool nacht = _nacht;
    auto amber = [&](uint16_t* row) {
        for (int x = 0; x < Size; x++) {
            const uint16_t c = row[x];
            if (!c) continue;
            const int R6 = ((c >> 11) & 31) << 1, G6 = (c >> 5) & 63, B6 = (c & 31) << 1;
            const int l = (R6 * 3 + G6 * 6 + B6) / 10;   // 0..63
            row[x] = (uint16_t)(((l >> 1) << 11) | (((l * 9) >> 4) << 5) | (l >> 4));   // R 1, G 0,56, B 0,1
        }
    };
    auto addKlok = [&](uint16_t* row, int y) {
        if (!nacht && y >= DotY - 5 && y <= DotY + 5) addDots(row, y);
        if (nacht) {
            if (klok && y >= KlokY && y < KlokY + KlokH) {
                const uint16_t* kr = klok + (y - KlokY) * Size;
                for (int x = 0; x < Size; x++) if (kr[x]) row[x] = kr[x];   // in het zwart boven de bol: geen optellen nodig
            }
            amber(row);
            return;
        }
        if (!klok || y < KlokY || y >= KlokY + KlokH) return;
        const uint16_t* kr = klok + (y - KlokY) * Size;
        for (int x = 0; x < Size; x++) {
            const uint16_t a = kr[x];
            if (!a) continue;
            const uint16_t c = row[x];
            int R5 = ((c >> 11) & 31) + ((a >> 11) & 31), G6 = ((c >> 5) & 63) + ((a >> 5) & 63), B5 = (c & 31) + (a & 31);
            row[x] = (uint16_t)(((R5 > 31 ? 31 : R5) << 11) | ((G6 > 63 ? 63 : G6) << 5) | (B5 > 31 ? 31 : B5));
        }
    };
    for (int i = 0; i < 21; i++) _blank[_render][w.half * 21 + i] = false;

    for (int y0 = rows0; y0 < rows1; y0 += BandH) {
        const int y1 = (y0 + BandH < rows1) ? y0 + BandH : rows1, bh = y1 - y0;
        if (y1 <= EarthCy - RG || y0 >= EarthCy + RG) {   // strook zonder bol: zwart (met klok)
            uint32_t* p = (uint32_t*)(dst + (size_t)y0 * _fw);
            for (int i = 0; i < _fw * bh / 2; i++) p[i] = 0;
            for (int y = y0; y < y1; y++) addKlok(dst + (size_t)y * _fw + _pad, y);
            continue;
        }
        memset(w.band, 0, (size_t)_fw * bh * 2);
        for (int y = y0; y < y1; y++) {
            uint16_t* row = w.band + (y - y0) * _fw + _pad;
            const int dy = y - EarthCy;
            if (dy <= -RG || dy >= RG) continue;
            const int cnt = _eRow[y + 1] - _eRow[y];
            if (cnt <= 0) continue;
            const uint32_t* pp = pix + _eRow[y];
            uint16_t* out = row + _eX0[y];
            // gedraaid: schermpunt terug naar het rechtopstaande beeld (16.16), daar de tabel opzoeken
            const int sx = _eX0[y] - kCx;
            int wx = rc * sx - rs * dy + (kCx << 16) + 0x8000, wy = rs * sx + rc * dy + (EarthCy << 16) + 0x8000;
            for (int k = 0; k < cnt; k++) {
                uint32_t e;
                if (rot) {
                    int iy = wy >> 16, ix = wx >> 16;
                    wx += rc; wy += rs;
                    iy = iy < EarthCy - EarthR + 1 ? EarthCy - EarthR + 1 : (iy > EarthCy + EarthR - 1 ? EarthCy + EarthR - 1 : iy);
                    const int n = _eRow[iy + 1] - _eRow[iy];
                    if (n <= 0) { out[k] = 0; continue; }
                    ix -= _eX0[iy];
                    e = pix[_eRow[iy] + (ix < 0 ? 0 : (ix >= n ? n - 1 : ix))];
                } else e = pp[k];
                int col = (int)((e & 1023) + su) & (W - 1), r = (int)((e >> 10) & 511);
                int L = ((int)((e >> 19) & 255) * mb) >> 8;
                int hz = (int)(e >> 27) * g8 >> 8;
                uint16_t c = tex[r * W + col];
                int R5 = (((c >> 11) & 31) * L) >> 8, G6 = (((c >> 5) & 63) * L) >> 8, B5 = ((c & 31) * L) >> 8;
                R5 += hz >> 3; G6 += (hz * 3) >> 2; B5 += hz;
                out[k] = (uint16_t)(((R5 > 31 ? 31 : R5) << 11) | ((G6 > 63 ? 63 : G6) << 5) | (B5 > 31 ? 31 : B5));
            }
        }
        for (int y = y0; y < y1; y++) addKlok(w.band + (y - y0) * _fw + _pad, y);
        memcpy(dst + (size_t)y0 * _fw, w.band, (size_t)bh * _fw * 2);
    }
}

}  // namespace jarvis
