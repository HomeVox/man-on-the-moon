#include "jarvis_view.h"
#include <hal/hal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace jarvis {

static const char* TAG = "JVIEW";
static constexpr float kMid = JarvisView::Size / 2.0f;
static constexpr float kTau = 6.2831853f;
static constexpr uint16_t kBg = 0x0001;  // #030509 in RGB565
// Tekenkosten: stappen per pixel lengte en hoe vaak een stap gloed krijgt (lager = sneller, iets minder rijk)
static constexpr float kStepsPerPx = 0.85f;
static constexpr int kGlowEvery    = 3;
static constexpr bool kDirectDma = true;  // rechtstreeks per DMA (beeldbuffers zijn schermbreed)

// Frequenties (in u) van de sinussen langs een draad: golf 19 en 31, orb 3 en 5 (x theta), drift 6, hoek 1 (x theta)
static const float kFreq[6] = {19.0f, 31.0f, 3.0f * kTau, 5.0f * kTau, 6.0f, kTau};

struct Preset {
    float speed, energy, spread, bright;
};

static const Preset kPresets[StateCount] = {
    {0.70f, 0.65f, 0.92f, 0.80f},  // Idle
    {1.15f, 1.35f, 1.00f, 1.25f},  // Listening
    {1.45f, 1.10f, 0.78f, 1.00f},  // Thinking
    {1.25f, 1.55f, 1.08f, 1.40f},  // Speaking
    {1.70f, 1.70f, 1.25f, 1.30f},  // Making
};

const char* stateName(State s)
{
    switch (s) {
        case State::Idle: return "rust";
        case State::Listening: return "luisteren";
        case State::Thinking: return "denken";
        case State::Speaking: return "praten";
        case State::Making: return "maken";
    }
    return "?";
}

static float ease(float x)
{
    return x * x * (3 - 2 * x);
}

JarvisView::~JarvisView()
{
    for (auto& w : _w) {
        if (w.task) vTaskDelete(w.task);
        if (w.done) vSemaphoreDelete(w.done);
        if (w.band) heap_caps_free(w.band);
    }
    for (auto* b : _buf) if (b) heap_caps_free(b);
    if (_eDay) heap_caps_free(_eDay);
    if (_ePix) heap_caps_free(_ePix);
    for (auto* t : _bTex) if (t) heap_caps_free(t);
    if (_mPix) heap_caps_free(_mPix);
    if (_sky) heap_caps_free(_sky);
    if (_dust) heap_caps_free(_dust);
    if (_skyDraw) heap_caps_free(_skyDraw);
    if (_skyTmp) heap_caps_free(_skyTmp);
    if (_klok) heap_caps_free(_klok);
}

bool JarvisView::create()
{
    // Het paneel is breder dan het beeld (468 i.p.v. 466): buffers hebben de breedte van het scherm,
    // zodat ze zonder kopie naar het scherm kunnen. Beeld staat gecentreerd (_pad pixels links).
    _fw  = GetHAL().getDisplay().width();
    _pad = (_fw - Size) / 2;
    for (auto& b : _buf) {
        b = (uint16_t*)heap_caps_malloc((size_t)_fw * Size * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b) return false;
        for (int i = 0; i < _fw * Size; i++) b[i] = kBg;
    }

    for (int j = 0; j < Strands; j++) {
        float strand = j / (float)(Strands - 1);
        // Man on the moon: kleuren van de bollen in plaats van neon (neon bewaard in tag backup/neon-audio).
        // t 0 = zacht aardblauw (bas), 0,5 = maanzilver, 1 = warm zonlicht (hoog)
        static const float kPal[3][3] = {{0.42f, 0.60f, 0.88f}, {0.80f, 0.82f, 0.86f}, {0.96f, 0.78f, 0.52f}};
        const float t  = 1.0f - strand;
        const int seg  = t < 0.5f ? 0 : 1;
        const float f  = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
        float r = kPal[seg][0] + (kPal[seg + 1][0] - kPal[seg][0]) * f;
        float g = kPal[seg][1] + (kPal[seg + 1][1] - kPal[seg][1]) * f;
        float b = kPal[seg][2] + (kPal[seg + 1][2] - kPal[seg][2]) * f;
        float s2    = sinf(j * 2.7f);
        float alpha = 0.13f + 0.18f * s2 * s2;
        float gain  = (j % 5 == 0) ? 1.7f : 1.0f;
        float a     = alpha * gain;
        // vaste komma: kanaalwaarde * 256 per stap, nog te schalen met de helderheid
        _col[j].r    = (int)(r * 31.0f * a * 256.0f);
        _col[j].g    = (int)(g * 63.0f * a * 256.0f);
        _col[j].b    = (int)(b * 31.0f * a * 256.0f);
        _col[j].gain = 0;
    }
    // cirkel: hetzelfde palet als de rechte golf, omgekeerd: draad 0 = bas (aardblauw, binnen) ... laatste = hoog (zonlicht, buiten)
    for (int j = 0; j < Strands; j++) _colB[j] = _col[Strands - 1 - j];

    for (int i = 0; i < Points; i++) {
        float u = i / (float)(Points - 1);
        for (int k = 0; k < 6; k++) { _u[i].s[k] = sinf(kFreq[k] * u); _u[i].c[k] = cosf(kFreq[k] * u); }
        float e = 1.0f - (2 * u - 1) * (2 * u - 1);
        _u[i].env = e <= 0 ? 0 : powf(e, 1.3f);
    }

    for (int i = 0; i < 2; i++) {
        Worker& w = _w[i];
        w.self = this;
        w.half = i;
        w.done = xSemaphoreCreateBinary();
        w.band = (uint16_t*)heap_caps_malloc(_fw * BandH * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!w.band) {
            ESP_LOGW(TAG, "geen intern RAM voor stroken");
            return false;
        }
        xTaskCreatePinnedToCore(workerMain, i ? "jv_w1" : "jv_w0", 4096, &w, 1, &w.task, i);  // gelijk aan de hoofdlus: eerlijk delen met het versturen
    }
    ESP_LOGI(TAG, "klaar: %d draden x %d punten, intern vrij %u KB", Strands, Points,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

static inline uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void JarvisView::showTestPattern()
{
    uint16_t* f = _buf[0];
    for (int i = 0; i < _fw * Size; i++) f[i] = 0;
    const int S = Size, C = S / 2;
    for (int y = 0; y < S; y++) {
        for (int x = 0; x < S; x++) {
            uint16_t px = rgb(0, 0, 0);
            if (x % 50 == 0 || y % 50 == 0) px = rgb(70, 70, 70);                 // rooster 50 px
            if (x == C || y == C) px = rgb(160, 160, 160);                          // middenkruis
            if (x == y || x + y == S - 1) px = rgb(60, 30, 90);                    // diagonalen
            int d2 = (x - C) * (x - C) + (y - C) * (y - C);
            int dd = (int)sqrtf((float)d2);
            if (dd == 232 || dd == 233) px = rgb(255, 255, 255);                    // buitencirkel (volle schermbreedte)
            if (dd == 116) px = rgb(0, 220, 255);                                   // halve cirkel
            if (y < 6) px = rgb(255, 0, 0);                                         // boven rood
            if (y >= S - 6) px = rgb(0, 255, 0);                                    // onder groen
            if (x < 6) px = rgb(0, 80, 255);                                        // links blauw
            if (x >= S - 6) px = rgb(255, 255, 0);                                  // rechts geel
            f[y * _fw + _pad + x] = px;
        }
    }
    // hoekblokjes: linksboven wit, rechtsboven magenta, linksonder oranje, rechtsonder cyaan
    auto box = [&](int x0, int y0, uint16_t c) {
        for (int y = y0; y < y0 + 40; y++) for (int x = x0; x < x0 + 40; x++) f[y * _fw + _pad + x] = c;
    };
    box(60, 60, rgb(255, 255, 255));
    box(S - 100, 60, rgb(255, 0, 255));
    box(60, S - 100, rgb(255, 140, 0));
    box(S - 100, S - 100, rgb(0, 255, 255));
    GetHAL().pushFrame(f);
}

void JarvisView::workerMain(void* arg)
{
    Worker* w = (Worker*)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        w->self->renderHalf(*w);
        w->self->_workMs[w->half] = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        xSemaphoreGive(w->done);
    }
}

// sin(A+B) = sinA cosB + cosA sinB: tabellen per punt, per draad maar zes sincos
void JarvisView::computePoints()
{
    auto& _px = _pxs[_setW]; auto& _py = _pys[_setW]; auto& _sy0 = _sy0s[_setW]; auto& _sy1 = _sy1s[_setW];
    const Params& P = _p;
    const float t   = P.t;
    const float sh  = P.morph;                                  // 0..2
    const float ex  = sh < 1.0f ? sh : 1.0f;                    // 0 = klein, 1 = tegen de buitenrand
    const float q   = sh <= 1.0f ? 1.0f : 1.0f - (sh - 1.0f);  // 1 = rond, 0 = platgedrukt
    const float ysq = 0.9f * q + 0.02f * (1.0f - q);
    const float wv  = P.wave * (1.0f - q);                      // 0..1: hoeveel audiogolf
    const float kMidR = 138.0f, kHalfR = 76.0f;               // ring: binnenste draad ~62, buitenste ~214 (vanaf de kleine orb uitgezet)
    float lvl = 0.0f;   // gemiddeld volume over alle banden (voor de cirkel)
    for (int k = 0; k < 20; k++) lvl += P.bd[k];
    lvl = lvl * 0.1f > 1.0f ? 1.0f : lvl * 0.1f;
    const float tAmt = P.tAmt;
    const float tr  = 1.0f / (2.0f * 105.0f * 105.0f);        // bereik van de vingertrek
                                 // straal van de ring (midden van de draden)
    for (int j = 0; j < Strands; j++) {
        float strand = j / (float)(Strands - 1);
        float off    = (strand - 0.5f) * 2.0f;
        float jj     = j;
        float ymin = 1e9f, ymax = -1e9f;
        // fase per sinus: B = c2 * t + c3 * j
        const float B[6] = {2.3f * t + 0.22f * jj, -1.7f * t + 0.4f * jj, 1.8f * t + 0.27f * jj,
                            -1.3f * t + 0.15f * jj, 1.2f * t + 0.67f * jj, 0.15f * t + 0.065f * jj};
        float sb[6], cb[6];
        for (int k = 0; k < 6; k++) { sb[k] = sinf(B[k]); cb[k] = cosf(B[k]); }

        for (int i = 0; i < Points; i++) {
            const UTab& U = _u[i];
            float u = i / (float)(Points - 1);
            float S0 = U.s[0] * cb[0] + U.c[0] * sb[0];
            float S1 = U.s[1] * cb[1] + U.c[1] * sb[1];
            float S2 = U.s[2] * cb[2] + U.c[2] * sb[2];
            float S3 = U.s[3] * cb[3] + U.c[3] * sb[3];
            float S4 = U.s[4] * cb[4] + U.c[4] * sb[4];
            float cosA = U.c[5] * cb[5] - U.s[5] * sb[5];   // cos(theta + B)
            float sinA = U.s[5] * cb[5] + U.c[5] * sb[5];   // sin(theta + B)

            // Vorm: shape 0 = kleine orb, 1 = ring langs de buitenrand, 2 = die ring verticaal ingekort tot een horizontale lijn
            float r = ((92.0f + (kMidR - 92.0f) * ex) + off * (29.0f + (kHalfR - 29.0f) * ex)) * P.spread +
                      (S2 * 13 + S3 * 9) * P.energy * (1.0f - 0.4f * ex);
            r *= 1.0f + 0.055f * P.energy * (S2 * 0.5f + 2.0f * sinA * cosA * 0.5f);   // zachte uitstulpingen: minder een perfecte cirkel
            float xOrb = kMid + cosA * r;
            float yOrb = kMid + sinA * r * ysq;
            // bij de lijn blijven de draden golven: de verticale spreiding die de ring kwijtraakt komt als golfje terug
            float yWave = (off * 30.0f * U.env * P.spread + S0 * 16 * U.env * P.energy + S1 * 8 * U.env * P.energy) * (1.0f - q);
            float drift = S4 * 5 * P.energy;
            (void)u;

            // audiogolven: bij de lijn vouwt de ring open tot een golf over de breedte (het oude golfbeeld)
            float xAud = 4.0f + u * 458.0f;   // van rand tot rand
            // stem: het niveau van "nu" zit in het midden, oudere niveaus lopen naar de uiteinden (rimpel van de stem)
            float dc = fabsf(u - 0.5f) * 2.0f * 15.0f;
            int d0 = (int)dc; float df = dc - d0;
            float hv = P.h[d0] * (1.0f - df) + P.h[d0 + 1 > 15 ? 15 : d0 + 1] * df;
            float bf = fabsf(u - 0.5f) * 2.0f * 19.0f;   // toonhoogte: bas in het midden, hoog aan de uiteinden
            int b0 = (int)bf; float bfr = bf - b0;
            float bv = P.bd[b0] * (1.0f - bfr) + P.bd[b0 + 1 > 19 ? 19 : b0 + 1] * bfr;
            hv = 0.7f * bv + 0.3f * hv;
            float amp = 0.30f + 2.3f * hv * P.gain + 0.55f * P.beat;
            float yAud = kMid + (off * 62.0f * U.env * P.spread + S0 * 17 * U.env + S1 * 9 * U.env) * amp;
            float px = (xOrb + drift) * (1.0f - wv) + xAud * wv;
            float py = (yOrb + yWave) * (1.0f - wv) + yAud * wv;
            if (P.ring > 0.002f) {   // dezelfde golf, dan rond: bas onderaan, hoge tonen boven waar de uiteinden samenkomen
                // hoektabellen (eenmalig): sin/cos van de hoek en van de golven k = 3, 5, 2, 7 rond de ring
                static float tS[4][Points], tC[4][Points], tCos[Points], tSin[Points];
                static bool tInit = false;
                if (!tInit) {
                    static const int kk[4] = {3, 5, 2, 7};
                    for (int n = 0; n < Points; n++) {
                        float a = n / (float)(Points - 1) * 6.2831853f;   // 0..2pi, bovenaan beginnen en eindigen
                        float p = a - 1.5707963f;
                        tCos[n] = cosf(p); tSin[n] = sinf(p);
                        for (int k = 0; k < 4; k++) { tS[k][n] = sinf(kk[k] * a); tC[k][n] = cosf(kk[k] * a); }
                    }
                    tInit = true;
                }
                // precies de bundel en beweging van de rechte golf (zelfde golfjes, zelfde amp), alleen rondgebogen;
                // spiegeling links/rechts (tabel van de eerste helft) houdt de naad bovenaan gesloten
                const UTab& M = _u[i < Points / 2 ? i : Points - 1 - i];
                float m0 = M.s[0] * cb[0] + M.c[0] * sb[0], m1 = M.s[1] * cb[1] + M.c[1] * sb[1];
                // elke kleurlijn heeft een noot: draad 0 (paars) = C ... laatste draad (cyaan) = B, twee à drie draden per noot.
                // De bundel (off-term) volgt het gemiddelde volume en blijft zo bij elkaar; alleen de piek van een draad volgt zijn noot.
                float lv  = lvl * 3.0f > 1.0f ? 1.0f : lvl * 3.0f;
                float bs  = P.nt[(j * 12) / Strands] * (0.4f + 0.6f * lv);   // noten zijn relatief: hardheid erbij
                float bsq = sqrtf(bs * P.gain > 1.5f ? 1.5f : bs * P.gain);
                float ampB = 0.30f + 2.3f * (lvl * P.gain > 1.0f ? 1.0f : lvl * P.gain) * 0.6f;
                // de ring blijft rond (kleine golving), de toon maakt alleen pieken naar buiten op plekken die wisselen
                float wk  = m0 * 0.65f + m1 * 0.35f;
                float pk  = wk > 0.0f ? wk * wk : 0.0f;
                float rr  = (108.0f + 10.0f * lvl + 10.0f * P.beat * (1.0f - strand)) + off * 62.0f * P.spread * ampB * 0.4f
                            + (0.5f + 0.5f * m0) * 8.0f * P.energy + 160.0f * bsq * pk;   // alles beweegt vanaf de ring naar buiten
                if (rr > 226.0f) rr = 226.0f;
                if (rr < 25.0f) rr = 25.0f;
                // de ring ontstaat als punt in het midden (straal groeit met P.ring); de rechte golf trekt eerst naar het midden weg
                float sc  = P.ring;
                float wg  = P.ring * 4.0f > 1.0f ? 1.0f : P.ring * 4.0f;
                px = px * (1.0f - wg) + (kMid + tCos[i] * rr * sc) * wg;
                py = py * (1.0f - wg) + (kMid + tSin[i] * rr * sc) * wg;
            }
            if (tAmt > 0.002f || tAmt < -0.002f) {   // vinger: punten dichtbij worden meegetrokken, zacht uitlopend
                float dx = P.tx - px, dy = P.ty - py;
                float infl = expf(-(dx * dx + dy * dy) * tr);
                px += dx * infl * 0.62f * tAmt;
                py += dy * infl * 0.62f * tAmt;
            }
            if (P.reveal < 0.999f) {   // uitgroeien vanuit het midden (bij de cirkel in alle richtingen)
                px = kMid + (px - kMid) * P.reveal;
                py = kMid + (py - kMid) * (1.0f - (1.0f - P.reveal) * P.ring);
            }
            if (P.use3d > 0.003f) {   // vlak in de ruimte: draden liggen op verschillende diepte, kantelen geeft perspectief
                float X = px - kMid, Y = py - kMid, Z = 0.0f;   // geen diepte: alleen waterpas draaien
                const float* M = P.rm;
                float dx = X * M[0] + Y * M[3] + Z * M[6];
                float dy = X * M[1] + Y * M[4] + Z * M[7];
                float dz = X * M[2] + Y * M[5] + Z * M[8];
                (void)dz;
                float s3 = 1.0f;   // geen perspectief
                px = kMid + dx * s3;
                py = kMid + dy * s3;
            }
            _px[j][i] = px;
            _py[j][i] = py;
            if (py < ymin) ymin = py;
            if (py > ymax) ymax = py;
        }
        _sy0[j] = ymin;
        _sy1[j] = ymax;
    }
}

// Water: deeltjes-vloeistof met dubbele-dichtheid-relaxatie (Clavet, Beaudoin, Poulin 2005).
// Zwaartekracht komt uit de kantelsensor, de ronde schermrand is het glas, een beat geeft een plons,
// muziek houdt het oppervlak onrustig en de vinger duwt het water opzij. Elk deeltje wordt een kort
// lichtstreepje (positie -> waar het net was); oppervlak cyaan, diep paars (de kleuren van de draden).
void JarvisView::computeWater(float dt)
{
    auto& _px = _pxs[_setW]; auto& _py = _pys[_setW]; auto& _sy0 = _sy0s[_setW]; auto& _sy1 = _sy1s[_setW];
    constexpr int N    = WaterN;
    constexpr int G    = 13;                  // rastercellen per as (cel >= H)
    constexpr float H  = 34.0f;             // bereik van een deeltje (px); groter = meer water met evenveel deeltjes
    constexpr float R  = 222.0f;              // straal van het glas
    constexpr float C0 = Size * 0.5f;
    constexpr float cell = Size / (float)G;
    constexpr float kRho0 = 1.35f, kStiff = 0.08f, kNear = 0.12f;  // rustdichtheid (~13 px tussenruimte), stijfheid
    constexpr float kVisc = 0.6f;                                  // stroperigheid: buren trekken elkaars snelheid gelijk
    auto rnd = [this]() {
        _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
        return (_rng & 0xFFFF) / 65535.0f;
    };

    if (!_waterInit) {   // klodder in het midden die daarna uitvalt
        for (int i = 0; i < N; i++) {
            float a = rnd() * 6.2831853f, r = sqrtf(rnd()) * 64.0f;
            _wx[i] = C0 + cosf(a) * r; _wy[i] = C0 + sinf(a) * r;
            _wvx[i] = _wvy[i] = 0.0f;
        }
        _waterInit     = true;
        _waterBeatSeen = _lastBeat;
    }

    // zwaartekracht in het schermvlak (zelfde assen als waterpas); plat op tafel bijna nul: zwevende klodder
    float gl = sqrtf(_ax * _ax + _ay * _ay + _az * _az);
    // de sensor meet "omhoog" (rechtop, USB onder: ay = -0.96, gemeten 27-10), dus zwaartekracht = -a
    float gx = gl > 0.2f ? -_ax / gl : 0.0f, gy = gl > 0.2f ? -_ay / gl : 1.0f;
    float gm = sqrtf(gx * gx + gy * gy);
    float dx0 = gm > 0.15f ? gx / gm : 0.0f, dy0 = gm > 0.15f ? gy / gm : 1.0f;   // richting "omlaag"
    // plat op tafel (je kijkt van bovenaf in het glas): geen samentrekken, het water ligt als laag over de bodem
    float flat = 1.0f - (gm - 0.2f) / 0.3f;
    flat = flat < 0 ? 0 : (flat > 1 ? 1 : flat);
    const float grav = 1000.0f;   // lichter: minder samendrukken onderin (krioelen)

    // geluid
    float lvl = 0.0f;
    for (int k = 0; k < 20; k++) lvl += _bands[k];
    lvl = lvl / 20.0f * _gain;
    if (lvl > 1.0f) lvl = 1.0f;
    float dmin = 1e9f, dmax = -1e9f;
    for (int i = 0; i < N; i++) {
        _wd[i] = _wx[i] * dx0 + _wy[i] * dy0;   // diepte langs de zwaartekracht
        if (_wd[i] < dmin) dmin = _wd[i];
        if (_wd[i] > dmax) dmax = _wd[i];
    }
    float surf = dmin + 0.35f * (dmax - dmin);
    if (_lastBeat != _waterBeatSeen && flat >= 0.5f) {   // plat: een druppel op de beat, het water wijkt uit in een kring
        _waterBeatSeen = _lastBeat;
        float a = rnd() * 6.2831853f, r = sqrtf(rnd()) * 120.0f;
        float px = C0 + cosf(a) * r, py = C0 + sinf(a) * r;
        for (int i = 0; i < N; i++) {
            float ex = _wx[i] - px, ey = _wy[i] - py, d2 = ex * ex + ey * ey;
            if (d2 > 90.0f * 90.0f || d2 < 0.01f) continue;
            float d = sqrtf(d2), f = 260.0f * _gain * (1.0f - d / 90.0f);
            _wvx[i] += ex / d * f; _wvy[i] += ey / d * f;
        }
    }
    if (_lastBeat != _waterBeatSeen) {   // rechtop/schuin: plons, het oppervlak springt omhoog
        _waterBeatSeen = _lastBeat;
        for (int i = 0; i < N; i++) {
            if (_wd[i] > surf) continue;
            float up = (140.0f + 180.0f * rnd()) * _gain, side = (rnd() - 0.5f) * 100.0f * _gain;
            _wvx[i] += -dx0 * up + -dy0 * side;
            _wvy[i] += -dy0 * up + dx0 * side;
        }
    }
    float jit = 50.0f * lvl * (1.0f - 0.8f * flat);   // muziek: onrustig oppervlak (plat: alleen een lichte trilling)
    for (int i = 0; i < N; i++) {
        float w = _wd[i] < surf ? 1.0f : 0.0f;   // alleen het oppervlak; diep water blijft rustig
        _wvx[i] += (rnd() - 0.5f) * jit * w;
        _wvy[i] += (rnd() - 0.5f) * jit * w;
    }
    // vinger: roeren. Het water vlak bij de vinger neemt de snelheid van de vinger over (meesleuren, draaikolken)
    // en wordt een beetje opzij geduwd, zodat de vinger een "gat" in het water maakt.
    {
        float fdt = dt > 0.05f ? 0.05f : dt;
        if (_touchDown) {
            float fvx = 0.0f, fvy = 0.0f;
            if (_fOn && fdt > 0.0f) { fvx = (_tgx - _fPx) / fdt; fvy = (_tgy - _fPy) / fdt; }
            float fv2 = fvx * fvx + fvy * fvy;
            if (fv2 > 1500.0f * 1500.0f) { float k = 1500.0f / sqrtf(fv2); fvx *= k; fvy *= k; }
            for (int i = 0; i < N; i++) {
                float ex = _wx[i] - _tgx, ey = _wy[i] - _tgy, d2 = ex * ex + ey * ey;
                if (d2 >= 60.0f * 60.0f || d2 < 0.01f) continue;
                float d = sqrtf(d2), w = 1.0f - d / 60.0f;
                _wvx[i] += (fvx - _wvx[i]) * 0.55f * w + ex / d * 900.0f * w * fdt;
                _wvy[i] += (fvy - _wvy[i]) * 0.55f * w + ey / d * 900.0f * w * fdt;
            }
            _fPx = _tgx; _fPy = _tgy; _fOn = true;
        } else {
            _fOn = false;
        }
    }

    const int steps = 1;   // één stap per beeld: twee kostte te veel tijd
    const float sdt = (dt > 0.045f ? 0.045f : dt) / steps;
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < N; i++) {
            _wvx[i] = (_wvx[i] + grav * gx * sdt) * 0.996f;
            _wvy[i] = (_wvy[i] + grav * gy * sdt) * 0.996f;
            _wox[i] = _wx[i]; _woy[i] = _wy[i];
            _wx[i] += _wvx[i] * sdt; _wy[i] += _wvy[i] * sdt;
        }
        // raster voor de buren
        for (int c = 0; c < G * G; c++) _wHead[c] = -1;
        for (int i = 0; i < N; i++) {
            int cx = (int)(_wx[i] / cell), cy = (int)(_wy[i] / cell);
            cx = cx < 0 ? 0 : (cx >= G ? G - 1 : cx); cy = cy < 0 ? 0 : (cy >= G ? G - 1 : cy);
            int c = cy * G + cx;
            _wNext[i] = _wHead[c]; _wHead[c] = (int16_t)i;
        }
        // dubbele-dichtheid-relaxatie: drukt samengepakte deeltjes uit elkaar, trekt losse licht aan (oppervlaktespanning)
        for (int i = 0; i < N; i++) {
            int cx = (int)(_wx[i] / cell), cy = (int)(_wy[i] / cell);
            cx = cx < 0 ? 0 : (cx >= G ? G - 1 : cx); cy = cy < 0 ? 0 : (cy >= G ? G - 1 : cy);
            int nb[40]; float nq[40], nux[40], nuy[40]; int nn = 0;
            float rho = 0.0f, rhoN = 0.0f;
            for (int yy = cy - 1; yy <= cy + 1; yy++) {
                if (yy < 0 || yy >= G) continue;
                for (int xx = cx - 1; xx <= cx + 1; xx++) {
                    if (xx < 0 || xx >= G) continue;
                    for (int j = _wHead[yy * G + xx]; j >= 0; j = _wNext[j]) {
                        if (j == i) continue;
                        float ex = _wx[j] - _wx[i], ey = _wy[j] - _wy[i], r2 = ex * ex + ey * ey;
                        if (r2 >= H * H || r2 < 1e-6f) continue;
                        float r = sqrtf(r2), q = 1.0f - r / H;
                        rho += q * q; rhoN += q * q * q;
                        if (nn < 40) { nb[nn] = j; nq[nn] = q; nux[nn] = ex / r; nuy[nn] = ey / r; nn++; }
                    }
                }
            }
            float P = kStiff * (rho - kRho0), PN = kNear * rhoN;
            if (P < 0.0f) P *= 1.0f - flat;   // plat: geen oppervlaktespanning, het water spreidt zich uit
            float mx = 0.0f, my = 0.0f, vxs = 0.0f, vys = 0.0f, qs = 0.0f;
            for (int k = 0; k < nn; k++) {
                float q = nq[k];
                vxs += (_wvx[nb[k]] - _wvx[i]) * q; vys += (_wvy[nb[k]] - _wvy[i]) * q; qs += q;
                float D = (P * q + PN * q * q) * H * 0.5f;
                D = D > 2.8f ? 2.8f : (D < -2.8f ? -2.8f : D);
                _wx[nb[k]] += nux[k] * D; _wy[nb[k]] += nuy[k] * D;
                mx -= nux[k] * D; my -= nuy[k] * D;
            }
            if (qs > 0.0f) { mx += vxs / qs * kVisc * sdt; my += vys / qs * kVisc * sdt; }   // stroperigheid
            _wx[i] += mx; _wy[i] += my;
        }
        // glas (ronde rand) en snelheid uit de verplaatsing
        for (int i = 0; i < N; i++) {
            float ex = _wx[i] - C0, ey = _wy[i] - C0, d2 = ex * ex + ey * ey;
            if (d2 > R * R) { float k = R / sqrtf(d2); _wx[i] = C0 + ex * k; _wy[i] = C0 + ey * k; }
            float vx = (_wx[i] - _wox[i]) / sdt, vy = (_wy[i] - _woy[i]) / sdt, v2 = vx * vx + vy * vy;
            if (v2 > 1200.0f * 1200.0f) { float k = 1200.0f / sqrtf(v2); vx *= k; vy *= k; }
            else if (v2 < 45.0f * 45.0f) { vx *= 0.85f; vy *= 0.85f; }   // bijna stil: laten uitdempen (geen krioelen)
            _wvx[i] = vx; _wvy[i] = vy;
        }
    }

    // tekenen: deeltjes op diepte gesorteerd over de draden verdeeld (draad 0 = oppervlak, cyaan; laatste = diep, paars)
    for (int i = 0; i < N; i++) { _wOrder[i] = (uint16_t)i; _wd[i] = _wx[i] * dx0 + _wy[i] * dy0; }
    std::sort(_wOrder, _wOrder + N, [this](uint16_t a, uint16_t b) { return _wd[a] < _wd[b]; });

    for (int j = 0; j < Strands; j++) {
        int r0 = j * N / Strands, r1 = (j + 1) * N / Strands, c = 0;
        float ymin = 1e9f, ymax = -1e9f;
        for (int r = r0; r < r1 && 2 * c + 1 < Points; r++, c++) {
            int i = _wOrder[r];
            float ax = _wx[i], ay = _wy[i];
            float bx = ax - _wvx[i] * 0.03f, by = ay - _wvy[i] * 0.03f;   // streepje: waar het deeltje net was
            if (fabsf(bx - ax) + fabsf(by - ay) < 1.5f) { bx = ax + 1.2f; by = ay + 0.8f; }
            _px[j][2 * c] = ax; _py[j][2 * c] = ay;
            _px[j][2 * c + 1] = bx; _py[j][2 * c + 1] = by;
            ymin = fminf(ymin, fminf(ay, by)); ymax = fmaxf(ymax, fmaxf(ay, by));
        }
        for (int k = 2 * c; k < Points; k++) { _px[j][k] = -1000.0f; _py[j][k] = -1000.0f; }
        _sy0[j] = ymin; _sy1[j] = ymax;
    }
}

// Optellen op een pixel in een strook, per kanaal begrensd
static inline void addPx(uint16_t* p, int r, int g, int b)
{
    uint16_t v = *p;
    int R = ((v >> 11) & 31) + r;
    int G = ((v >> 5) & 63) + g;
    int B = (v & 31) + b;
    if (R > 31) R = 31;
    if (G > 63) G = 63;
    if (B > 31) B = 31;
    *p = (uint16_t)((R << 11) | (G << 5) | B);
}

void JarvisView::renderHalf(Worker& w)
{
    if (_rp.earth > 50.0f) { renderSkyHalf(w); return; }   // ver boven de bollen (1 aarde, 2..4 maan, Mars, Jupiter)
    if (_rp.earth > 0.5f) { renderEarthHalf(w); return; }
    const int set = 1 - _setW;   // de set die vorige keer is berekend
    const auto& _px = _pxs[set]; const auto& _py = _pys[set]; const auto& _sy0 = _sy0s[set]; const auto& _sy1 = _sy1s[set];
    const float bright = _rp.bright;
    const int rows0    = w.half * (Size / 2);
    const int rows1    = w.half ? Size : Size / 2;
    uint16_t* dst      = _buf[_render];
    int64_t tClear = 0, tDraw = 0, tCopy = 0, tm = esp_timer_get_time();   // meting (tijdelijk)

    for (int y0 = rows0; y0 < rows1; y0 += BandH) {
        const int y1 = (y0 + BandH < rows1) ? y0 + BandH : rows1;
        const int bh = y1 - y0;

        // Strook zonder draden: alleen achtergrond. Stond hij in deze buffer al zo, dan niets doen.
        {
            bool has = false;
            for (int j = 0; j < Strands && !has; j++) has = !(_sy1[j] < y0 - 4 || _sy0[j] > y1 + 4);
            bool* blank = &_blank[_render][w.half * 21 + (y0 - rows0) / BandH];
            if (!has) {
                if (!*blank) {
                    uint32_t v  = kBg | ((uint32_t)kBg << 16);
                    uint32_t* p = (uint32_t*)(dst + (size_t)y0 * _fw);
                    for (int i = 0; i < _fw * bh / 2; i++) p[i] = v;
                    *blank = true;
                }
                continue;
            }
            *blank = false;
        }

        // strook wissen
        {
            uint32_t v  = kBg | ((uint32_t)kBg << 16);
            uint32_t* p = (uint32_t*)w.band;
            for (int i = 0; i < _fw * bh / 2; i++) p[i] = v;
        }

        { int64_t n2 = esp_timer_get_time(); tClear += n2 - tm; tm = n2; }
        for (int j = 0; j < Strands; j++) {
            // kleur per stap: helderheid meegeschaald (kern en zachte gloed)
            const bool water = _rp.water > 0.5f;   // water: losse streepjes (punt 2c -> 2c+1), feller want kort
            const Strand& C = (_rp.ring > 0.5f && !water) ? _colB[j] : _col[j];   // cirkel: aardblauw (bas) -> zonlicht (hoog)
            const float boost = water ? 3.0f : 1.5f;
            const int cr = (int)(C.r * bright * boost), cg = (int)(C.g * bright * boost), cb = (int)(C.b * bright * boost);
            const int gr = cr / 6, gg = cg / 6, gb = cb / 6;   // zachte gloed: fijne lijnen, geen neonbuizen

            for (int i = 0; i + 1 < Points; i++) {
                if (water && (i & 1)) continue;
                float ax = _px[j][i], ay = _py[j][i];
                float bx = _px[j][i + 1], by = _py[j][i + 1];
                float lo = ay < by ? ay : by, hi = ay < by ? by : ay;
                if (hi < y0 - 3 || lo > y1 + 3) continue;

                float dx = bx - ax, dy = by - ay;
                float len = sqrtf(dx * dx + dy * dy);
                int n     = (int)(len * kStepsPerPx) + 1;
                float inv = 1.0f / n;
                for (int k = 0; k < n; k++) {
                    float x = ax + dx * k * inv, y = ay + dy * k * inv;
                    int ix = (int)x, iy = (int)y;
                    if (x < 3 || x >= Size - 4) continue;

                    // kern: subpixel over vier pixels
                    int fx = (int)((x - ix) * 256), fy = (int)((y - iy) * 256);
                    int w00 = ((256 - fx) * (256 - fy)) >> 8, w10 = (fx * (256 - fy)) >> 8,
                        w01 = ((256 - fx) * fy) >> 8, w11 = (fx * fy) >> 8;
                    int ry = iy - y0;
                    if (ry >= 0 && ry < bh) {
                        uint16_t* p = w.band + ry * _fw + _pad + ix;
                        addPx(p, (cr * w00 + 0x8000) >> 16, (cg * w00 + 0x8000) >> 16, (cb * w00 + 0x8000) >> 16);
                        addPx(p + 1, (cr * w10 + 0x8000) >> 16, (cg * w10 + 0x8000) >> 16, (cb * w10 + 0x8000) >> 16);
                    }
                    if (ry + 1 >= 0 && ry + 1 < bh) {
                        uint16_t* p = w.band + (ry + 1) * _fw + _pad + ix;
                        addPx(p, (cr * w01 + 0x8000) >> 16, (cg * w01 + 0x8000) >> 16, (cb * w01 + 0x8000) >> 16);
                        addPx(p + 1, (cr * w11 + 0x8000) >> 16, (cg * w11 + 0x8000) >> 16, (cb * w11 + 0x8000) >> 16);
                    }
                    // gloed: kruisje op 2 px, elke tweede stap
                    if (k % kGlowEvery == 0) {
                        int gR = (gr + 128) >> 8, gG = (gg + 128) >> 8, gB = (gb + 128) >> 8;
                        if (gR | gG | gB) {
                            int cy = ry, cx = ix;
                            if (cy >= 0 && cy < bh) {
                                addPx(w.band + cy * _fw + _pad + cx + 2, gR, gG, gB);
                                addPx(w.band + cy * _fw + _pad + cx - 2, gR, gG, gB);
                            }
                            if (cy + 2 >= 0 && cy + 2 < bh) addPx(w.band + (cy + 2) * _fw + _pad + cx, gR, gG, gB);
                            if (cy - 2 >= 0 && cy - 2 < bh) addPx(w.band + (cy - 2) * _fw + _pad + cx, gR, gG, gB);
                        }
                    }
                }
            }
        }

        { int64_t n2 = esp_timer_get_time(); tDraw += n2 - tm; tm = n2; }
        // strook naar het beeld
        memcpy(dst + (size_t)y0 * _fw, w.band, (size_t)bh * _fw * 2);
        { int64_t n2 = esp_timer_get_time(); tCopy += n2 - tm; tm = n2; }
    }
    static int cnt = 0;
    if (w.half == 0 && ++cnt % 100 == 0) ESP_LOGI(TAG, "half0: wissen %d us, tekenen %d us, kopieren %d us", (int)tClear, (int)tDraw, (int)tCopy);
}

void JarvisView::tick(uint32_t now)
{
    float dt = _last ? (now - _last) / 1000.0f : 0.016f;
    _last    = now;
    if (dt > 0.1f) dt = 0.1f;

    const Preset& p = kPresets[(int)_state];
    float k         = 1.0f - expf(-dt * 1.2f);
    _speed += (p.speed - _speed) * k;
    _energy += (p.energy - _energy) * k;
    _spread += (p.spread - _spread) * k;
    _bright += (p.bright - _bright) * k;
    _level += (_levelTarget - _level) * (1.0f - expf(-dt * 14.0f));
    _t += 0.48f * _speed * dt;

    // vorm 0 = kleine orb, 1 = ring langs de rand, 2 = horizontale lijn; golf 0..1 = lijn wordt audiogolf.
    float target, wtarget = 0.0f;
    float rate = 0.9f;
    _ring += ((_mode == 1 ? 1.0f : 0.0f) - _ring) * (1.0f - expf(-dt * 1.6f));   // modus 0 = rechte golf, 1 = cirkel
    switch (_state) {
        case State::Listening:
        case State::Speaking: target = 2.0f; wtarget = 1.0f; rate = 1.4f; break;
        case State::Thinking:
        case State::Making: target = 0.0f; rate = 1.4f; break;
        default: {
            _idleClock += dt;
            // 36 s: orb 4 | naar rand 6 | ring 3 | naar lijn 6 | golf op 3 | golf 5 | golf af 3 | terug naar orb 6
            float c = fmodf(_idleClock, 36.0f);
            target = c < 4.0f ? 0.0f : c < 10.0f ? ease((c - 4.0f) / 6.0f) : c < 13.0f ? 1.0f
                     : c < 19.0f ? 1.0f + ease((c - 13.0f) / 6.0f) : c < 30.0f ? 2.0f
                     : 2.0f - 2.0f * ease((c - 30.0f) / 6.0f);
            wtarget = c < 19.0f ? 0.0f : c < 22.0f ? ease((c - 19.0f) / 3.0f) : c < 27.0f ? 1.0f
                      : c < 30.0f ? 1.0f - ease((c - 27.0f) / 3.0f) : 0.0f;
            rate = 6.0f;   // het schema is zelf al zacht; de demping volgt het bijna direct
        }
    }
    _morph += (target - _morph) * (1.0f - expf(-dt * rate));
    _reveal += (1.0f - _reveal) * (1.0f - expf(-dt * 1.6f));
    _wave += (wtarget - _wave) * (1.0f - expf(-dt * (rate > 3.0f ? 6.0f : 1.4f)));

    // aanraking: de vinger volgt met vertraging; de sterkte is een veer (wiebelt licht na bij loslaten)
    _tx += (_tgx - _tx) * (1.0f - expf(-dt * 14.0f));
    _ty += (_tgy - _ty) * (1.0f - expf(-dt * 14.0f));
    {
        float want = _touchDown ? 1.0f : 0.0f;
        _tVel += ((want - _tAmt) * 90.0f - _tVel * 9.0f) * dt;
        _tAmt += _tVel * dt;
    }

    // zwaartekracht (veer = vloeistof) en daaruit een draaiing van het beeldvlak
    {
        // waterpas: één draaihoek uit de zwaartekracht in het schermvlak, met een veer (rustig naschommelen).
        // De fout wordt naar [-pi, pi] gebracht, dus de hoek neemt altijd de kortste weg en klapt niet om.
        float gl = sqrtf(_ax * _ax + _ay * _ay + _az * _az);
        float gx = gl > 0.2f ? -_ax / gl : 0.0f, gy = gl > 0.2f ? -_ay / gl : 0.0f;   // zwaartekracht = -a (zie water)
        float m  = sqrtf(gx * gx + gy * gy);   // 0 = plat op tafel, 1 = rechtop
        if (m > 0.35f) _angGoal = atan2f(gx, gy);   // bijna plat: de hoek is onbetrouwbaar, hou de laatste vast
        float err = _angGoal - _ang;
        while (err > 3.14159265f) err -= 6.2831853f;
        while (err < -3.14159265f) err += 6.2831853f;
        _angV += (err * 60.0f - _angV * 7.0f) * dt;
        _ang  += _angV * dt;
        while (_ang > 3.14159265f) _ang -= 6.2831853f;
        while (_ang < -3.14159265f) _ang += 6.2831853f;
        float use = (m - 0.35f) / 0.3f;
        use       = (use < 0 ? 0 : (use > 1 ? 1 : use)) * _tiltStrength;
        _use3d += (use - _use3d) * (1.0f - expf(-dt * 4.0f));
        float th = _ang * _use3d, c = cosf(th), s = sinf(th);
        const float Mw[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
        for (int k = 0; k < 9; k++) _rm[k] = Mw[k];
    }

    _p = {_t, _morph, _energy * (1.0f + 0.9f * _level), _spread, _bright, _wave, _tx, _ty, _tAmt, _gain, _reveal, _beat, _use3d, _ring, _mode == 2 ? 1.0f : 0.0f, {}, {}, {}};
    // maat: een bastoot ver boven het gemiddelde laat het geheel kort opspringen en oplichten
    {
        float bass = (_bands[0] + _bands[1] + _bands[2] + _bands[3]) * 0.25f;
        if (bass > _bassAvg * 1.45f + 0.12f && now - _lastBeat > 180) { _beat = 1.0f; _lastBeat = now; }
        _bassAvg += (bass - _bassAvg) * 0.05f;
        _beat *= expf(-dt * 7.0f);
    }
    for (int k = 0; k < 20; k++) _p.bd[k] = _bands[k];
    for (int k = 0; k < 12; k++) _p.nt[k] = _notes[k];
    for (int k = 0; k < 9; k++) _p.rm[k] = _rm[k];
    _p.bright = _bright * (1.0f + 0.25f * _beat);   // rustiger opflitsen op de maat
    _histPos = (_histPos + 1) % 48;
    _hist[_histPos] = _level;
    for (int k = 0; k < 16; k++) _p.h[k] = _hist[(_histPos - k * 2 + 96) % 48];

    // Volgend beeld laten tekenen, ondertussen het vorige naar het scherm sturen
    // Eerst de punten rekenen, dan tekenen (tegelijk rekenen hielp niet: beide kernen tekenen al)
    int64_t t0 = esp_timer_get_time();
    bool earth = false, sky = false;
    if (_mode >= 3) { computeEarth(dt); earth = globeReady(); }
    else if (_mode == 1) { computeSky(dt); sky = _skyReady; }
    if (_mode == 2) computeWater(dt); else if (!earth && !sky) computePoints();
    // bol: vaste helderheid, niet op de beat; earth 1 = aarde, 2 + nummer = overige bollen, 99 = sterrenhemel
    _rp       = {earth ? 0.8f : _p.bright, _p.ring, _p.water, sky ? 99.0f : earth ? (_mode >= 4 ? 2.0f + (_mode - 4) : 1.0f) : 0.0f, earth ? atan2f(_rm[3], _rm[0]) : 0.0f};
    if (earth) updateKlok();
    _setW     = 1 - _setW;   // de werktaken tekenen de net berekende set (1 - _setW)
    _pointsMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    for (auto& w : _w) xTaskNotifyGive(w.task);

    if (kDirectDma) {
        // Rechtstreeks per DMA naar het paneel: sneller, maar gaf een plat gedrukt beeld (23-10)
        if (_haveFrame) GetHAL().pushFrameStart(_buf[_show]);
        for (auto& w : _w) xSemaphoreTake(w.done, portMAX_DELAY);
        if (_haveFrame) GetHAL().pushFrameWait();
    } else {
        if (_haveFrame) GetHAL().pushFrame(_buf[_show]);  // via het M5GFX-framebuffer: beeld klopt
        for (auto& w : _w) xSemaphoreTake(w.done, portMAX_DELAY);
    }
    if (_haveFrame) _pushMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    _renderMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    _show      = _render;
    _render    = 1 - _render;
    _haveFrame = true;
}

}  // namespace jarvis
