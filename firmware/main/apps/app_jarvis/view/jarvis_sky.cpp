/*
 * Modus Sterrenhemel (1): een langzaam draaiende sterrenhemel met melkweg, op de muziek (vervangt Cirkel en Water;
 * die staan in tag backup/neon-audio).
 *  - 700 sterren in echte sterkleuren (blauwwit, wit, lichtgeel, wat oranje), meest klein en zwak, enkele fel met een
 *    lichtkruisje; ze fonkelen onregelmatig. Elke ster hoort bij een toon en licht op als die klinkt.
 *  - melkweg: een schuine band met meer sterren en ~3500 zwakke stofpuntjes; het stof licht op met de bas en de beat,
 *    met een trage golf langs de band (sterkte: schuif Melkweg in HA).
 *  - de hemel draait heel langzaam en blijft waterpas.
 *  - vallende sterren: subtiel, overal en in elke richting, ~3 per minuut op willekeurige momenten; een klap
 *    (plotseling veel hoog geluid) geeft er een extra (minstens 5 s na de vorige).
 *  - elke ster hangt aan een veer naar zijn eigen plek: op de beat zwiepen ze naar buiten (aan de rand meer),
 *    met hun eigen toon wiebelen ze licht, en daarna veren ze terug.
 *  - vinger: je roert door de sterren (ze nemen de vingerbeweging over); daarna glijden ze in ~3 s rustig
 *    terug naar hun plek (een tweede, trage veer zonder naschommelen). Sterren onder de vinger worden groter.
 * Per beeld rekent de hoofdtaak alle schermposities uit (gesorteerd per strook), de werktaken tekenen alleen.
 */
#include "jarvis_view.h"
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

namespace jarvis {

static const char* TAG = "hemel";
static constexpr float kC = JarvisView::Size / 2;   // midden van het scherm
static constexpr float kSc = 233.0f / 170.0f;       // het voorbeeld was 340 px breed
static constexpr float kR = 233.0f;

static inline float rnd() { return (esp_random() >> 8) * (1.0f / 16777216.0f); }

// snelle sinus voor het fonkelen (fout < 0,1%): sinf werd traag naarmate de tijd opliep (~4 us per aanroep)
static inline float fsin(float x)
{
    x -= 6.2831853f * floorf(x * 0.15915494f + 0.5f);   // naar [-pi, pi]
    const float y = 1.2732395f * x - 0.40528473f * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y;
}

bool JarvisView::skyInit()
{
    _sky = (SkyStar*)heap_caps_malloc(sizeof(SkyStar) * SkyStars, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _dust = (float*)heap_caps_malloc(sizeof(float) * 3 * SkyDust, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _skyDraw = (SkyDot*)heap_caps_malloc(sizeof(SkyDot) * SkyMaxDraw, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _skyTmp = (SkyDot*)heap_caps_malloc(sizeof(SkyDot) * SkyMaxDraw, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_sky || !_dust || !_skyDraw || !_skyTmp) { ESP_LOGE(TAG, "geen geheugen"); return false; }
    // melkweg: schuine band (richting 0,8 / 0,6), breedte als som van drie toevalsgetallen (zacht naar de rand)
    const float R = kR * 1.45f;   // groter dan het scherm: bij het draaien blijft de cirkel gevuld
    for (int i = 0; i < SkyStars; i++) {
        SkyStar& s = _sky[i];
        float x, y;
        if (i < SkyStars * 54 / 100) {
            const float u = (rnd() - 0.5f) * 2.0f * R, v = (rnd() + rnd() + rnd() - 1.5f) * 28.0f * kSc;
            x = u * 0.8f - v * 0.6f; y = u * 0.6f + v * 0.8f;
        } else {
            const float a = rnd() * 6.2831853f, r = sqrtf(rnd()) * R;
            x = r * cosf(a); y = r * sinf(a);
        }
        s.x = x; s.y = y;
        s.m = powf(rnd(), 3.2f);                 // helderheid: bijna allemaal zwak, een paar fel
        s.ph = rnd() * 7.0f; s.f = 1.0f + rnd() * 3.0f;
        s.ox = s.oy = s.vx = s.vy = s.px = s.py = s.pvx = s.pvy = s.g = 0;
        s.n = (uint8_t)(i % 12);
        const float c = rnd();                   // sterkleuren
        const uint8_t pal[5][3] = {{170, 195, 255}, {235, 240, 255}, {255, 244, 220}, {255, 214, 160}, {255, 180, 130}};
        const int k = c < 0.18f ? 0 : c < 0.6f ? 1 : c < 0.85f ? 2 : c < 0.96f ? 3 : 4;
        s.r = pal[k][0]; s.gg = pal[k][1]; s.b = pal[k][2];
    }
    for (int i = 0; i < SkyDust; i++) {   // stof: alleen in de band, iets breder dan de sterren
        const float u = (rnd() - 0.5f) * 2.0f * R, v = (rnd() + rnd() + rnd() + rnd() - 2.0f) * 30.0f * kSc;
        _dust[3 * i] = u * 0.8f - v * 0.6f;
        _dust[3 * i + 1] = u * 0.6f + v * 0.8f;
        _dust[3 * i + 2] = 0.08f + 0.14f * rnd();
    }
    ESP_LOGI(TAG, "%d sterren, %d stofjes", SkyStars, SkyDust);
    return true;
}

void JarvisView::computeSky(float dt)
{
    if (!_skyReady) {
        if (_skyFailed) return;
        _skyFailed = !skyInit();
        _skyReady = !_skyFailed;
        if (!_skyReady) return;
    }
    _skyT += dt;
    _skyRot += dt * 0.012f;
    const float th = _skyRot + _ang * _use3d, ca = cosf(th), sa = sinf(th);   // waterpas zoals de bollen
    const float gain = _gain;

    // vinger: snelheid (gevolgd) voor het roeren
    float fvx = 0, fvy = 0;
    if (_touchDown && _skyDown && dt > 0) {
        const float k = dt * 12.0f > 1 ? 1 : dt * 12.0f;
        _skyFvx += ((_tgx - _skyFx) / dt - _skyFvx) * k;
        _skyFvy += ((_tgy - _skyFy) / dt - _skyFvy) * k;
        fvx = _skyFvx; fvy = _skyFvy;
    } else _skyFvx = _skyFvy = 0;
    if (_touchDown) { _skyFx = _tgx; _skyFy = _tgy; }
    _skyDown = _touchDown;
    const int64_t t0 = esp_timer_get_time();

    int n = 0;
    SkyDot* out = _skyTmp;
    const float lim2 = (kR + 6) * (kR + 6);
    // stof: licht op met de bas en de beat, met een golf die langs de band loopt
    const float bass = (_bands[0] + _bands[1] + _bands[2] + _bands[3]) * 0.25f;
    const float mw = _melk * (bass * 1.8f + _beat * 0.8f);
    for (int i = 0; i < SkyDust && n < SkyMaxDraw; i++) {
        const float x = _dust[3 * i], y = _dust[3 * i + 1];
        const float X = kC + x * ca - y * sa, Y = kC + x * sa + y * ca;
        if ((X - kC) * (X - kC) + (Y - kC) * (Y - kC) > lim2) continue;
        float b = _dust[3 * i + 2];
        if (mw > 0.01f) {
            const float u = x * 0.8f + y * 0.6f;   // plek langs de band
            b *= 1.0f + mw * (0.4f + 0.6f * (0.5f + 0.5f * fsin(u * 0.025f - _skyT * 2.2f)));
            b = b > 0.75f ? 0.75f : b;
        }
        out[n++] = {X, Y, -1.0f, (uint8_t)(120 * b), (uint8_t)(130 * b), (uint8_t)(170 * b), 0};   // -1: één pixel
    }
    const int64_t t1 = esp_timer_get_time();
    // sterren
    const float stirR = 70.0f * kSc, hoverR = 60.0f * kSc;
    const bool roer = fvx * fvx + fvy * fvy > 4.0f;
    const float beat = _beat, kd = dt > 0.05f ? 0.05f : dt;   // veer: stijf 14, demping 4,5 (wiebelt licht na)
    for (int i = 0; i < SkyStars && n < SkyMaxDraw; i++) {
        SkyStar& s = _sky[i];
        float X = kC + s.x * ca - s.y * sa, Y = kC + s.x * sa + s.y * ca;
        float nt = _notes[s.n] * gain * (s.m > 0.25f ? 1.0f : 0.25f);
        nt = nt > 1.2f ? 1.2f : nt;
        {   // roeren: binnen stirR de vingerbeweging overnemen (met een beetje draaikolk); trage veer terug
            if (roer) {
                const float dx = X + s.px - _tgx, dy = Y + s.py - _tgy, d2 = dx * dx + dy * dy;
                if (d2 < stirR * stirR) {
                    const float d = sqrtf(d2), w = (1.0f - d / stirR) * (kd * 10.0f > 1 ? 1 : kd * 10.0f);
                    const float sw = 0.35f * sqrtf(fvx * fvx + fvy * fvy) / (d + 20.0f);   // draaikolk rond de vinger
                    s.pvx += (fvx - dy * sw - s.pvx) * w;
                    s.pvy += (fvy + dx * sw - s.pvy) * w;
                }
            }
            s.pvx += (-1.2f * s.px - 2.4f * s.pvx) * kd;   // trage veer, net iets meer dan kritisch gedempt
            s.pvy += (-1.2f * s.py - 2.4f * s.pvy) * kd;
            s.px += s.pvx * kd; s.py += s.pvy * kd;
        }
        {   // naar buiten op de beat en de eigen toon (aan de rand meer), veer terug naar de eigen plek
            const float rx = (X - kC) * (1.0f / kR), ry = (Y - kC) * (1.0f / kR);
            const float F = (beat * 90.0f + nt * 30.0f) * kSc * (gain > 1.5f ? 1.5f : gain);
            s.vx += (F * rx - 14.0f * s.ox - 4.5f * s.vx) * kd;
            s.vy += (F * ry - 14.0f * s.oy - 4.5f * s.vy) * kd;
            s.ox += s.vx * kd; s.oy += s.vy * kd;
        }
        X += s.ox + s.px; Y += s.oy + s.py;
        if ((X - kC) * (X - kC) + (Y - kC) * (Y - kC) > lim2) { s.g = 0; continue; }
        float want = 0;
        if (_touchDown) {
            const float md = sqrtf((X - _tgx) * (X - _tgx) + (Y - _tgy) * (Y - _tgy));
            if (md < hoverR) want = (hoverR - md) / hoverR;
        }
        s.g += (want - s.g) * (dt * 6.0f > 1 ? 1 : dt * 6.0f);
        // fonkelen; bij de zwakste sterren (meer dan een derde) is het verschil < 2%: overslaan
        const float tw = s.m < 0.05f ? 1.0f : 1.0f - 0.35f * s.m * (0.5f + 0.5f * fsin(_skyT * s.f * 2.0f + s.ph)) * (0.5f + 0.5f * fsin(_skyT * s.f * 3.3f + s.ph * 2.0f));
        float b = (0.12f + s.m * 0.9f) * tw + nt * 0.6f + s.g * 0.35f;
        b = b > 1.0f ? 1.0f : b;
        const float rad = (0.45f + s.m * 1.7f + nt * 1.2f + s.g * 1.8f) * 1.15f;
        out[n++] = {X, Y, rad, (uint8_t)(s.r * b), (uint8_t)(s.gg * b), (uint8_t)(s.b * b), (uint8_t)(s.m > 0.55f ? 1 : 0)};
    }
    const int64_t t2 = esp_timer_get_time();
    // vallende ster: willekeurig elke 10-30 s, of op een klap (hoge tonen schieten plots omhoog)
    {
        float hi = 0;
        for (int k = 8; k < 20; k++) hi += _bands[k];
        hi *= 1.0f / 12.0f;
        const bool klap = hi > _skyHiAvg * 2.2f + 0.15f;
        _skyHiAvg += (hi - _skyHiAvg) * (dt * 2.0f > 1 ? 1 : dt * 2.0f);
        if (_skyNext == 0) _skyNext = _skyT + 4.0f + rnd() * 10.0f;
        if (_skyShoot <= 0 && (_skyT >= _skyNext || (klap && _skyT - _skyLastShoot > 5.0f))) {
            const float a = rnd() * 6.2831853f, r = sqrtf(rnd()) * kR * 0.7f;   // ergens in het beeld
            const float dir = rnd() * 6.2831853f, sp = (200.0f + rnd() * 140.0f) * kSc;
            _skyShX = kC + cosf(a) * r; _skyShY = kC + sinf(a) * r;
            _skyShVx = cosf(dir) * sp; _skyShVy = sinf(dir) * sp;
            _skyShB = 0.4f + rnd() * 0.3f;        // subtiel: nooit vol wit
            _skyShLen = 0.08f + rnd() * 0.1f;     // staartlengte in seconden vlucht
            _skyShoot = 1.0f;
            _skyLastShoot = _skyT;
            _skyNext = _skyT + 10.0f + rnd() * 20.0f;   // gemiddeld ~3 per minuut
        }
    }
    if (_skyShoot > 0) {
        _skyShX += _skyShVx * dt; _skyShY += _skyShVy * dt;
        _skyShoot -= dt * (1.2f + 0.6f * _skyShLen * 5.0f);
        // in- en uitfaden: snel op, langzaam weg
        const float life = _skyShoot > 0 ? _skyShoot : 0, env = (life > 0.85f ? (1.0f - life) / 0.15f : life / 0.85f) * _skyShB;
        const float len = sqrtf(_skyShVx * _skyShVx + _skyShVy * _skyShVy) * _skyShLen;
        const int steps = (int)len;
        for (int k = 0; k < steps && n < SkyMaxDraw; k++) {
            const float f = k / (float)steps, X = _skyShX - _skyShVx * _skyShLen * f, Y = _skyShY - _skyShVy * _skyShLen * f;
            if ((X - kC) * (X - kC) + (Y - kC) * (Y - kC) > kR * kR) continue;
            const float b = env * (1.0f - f) * (1.0f - f);
            out[n++] = {X, Y, 0.0f, (uint8_t)(255 * b), (uint8_t)(250 * b), (uint8_t)(235 * b), 0};
        }
    }

    // per strook sorteren (tellen en verdelen), zodat elke strook alleen zijn eigen sterren langsloopt
    int cnt[SkyBands + 1] = {};
    for (int i = 0; i < n; i++) { int b = (int)out[i].y / BandH; b = b < 0 ? 0 : (b >= SkyBands ? SkyBands - 1 : b); cnt[b + 1]++; }
    for (int b = 0; b < SkyBands; b++) cnt[b + 1] += cnt[b];
    for (int b = 0; b <= SkyBands; b++) _skyBand[b] = cnt[b];
    for (int i = 0; i < n; i++) { int b = (int)out[i].y / BandH; b = b < 0 ? 0 : (b >= SkyBands ? SkyBands - 1 : b); _skyDraw[cnt[b]++] = out[i]; }
    _skyN = n;
    static int cnt2 = 0;   // meting (tijdelijk)
    if (++cnt2 % 150 == 0)
        ESP_LOGI(TAG, "%d punten; stof %d us, sterren %d us, rest %d us", n, (int)(t1 - t0), (int)(t2 - t1), (int)(esp_timer_get_time() - t2));
}

static inline void add565(uint16_t* p, int r, int g, int b)
{
    const uint16_t v = *p;
    int R = ((v >> 11) & 31) + r, G = ((v >> 5) & 63) + g, B = (v & 31) + b;
    *p = (uint16_t)(((R > 31 ? 31 : R) << 11) | ((G > 63 ? 63 : G) << 5) | (B > 31 ? 31 : B));
}

void JarvisView::renderSkyHalf(Worker& w)
{
    const int rows0 = w.half * (Size / 2), rows1 = w.half ? Size : Size / 2;
    uint16_t* dst = _buf[_render];
    for (int i = 0; i < 21; i++) _blank[_render][w.half * 21 + i] = false;
    constexpr uint16_t bg = 0x0001;
    for (int y0 = rows0; y0 < rows1; y0 += BandH) {
        const int y1 = (y0 + BandH < rows1) ? y0 + BandH : rows1, bh = y1 - y0;
        {
            uint32_t v = bg | ((uint32_t)bg << 16), *p = (uint32_t*)w.band;
            for (int i = 0; i < _fw * bh / 2; i++) p[i] = v;
        }
        // helper: één pixel optellen, binnen deze strook en binnen het beeld (kleur in 0..255 maal gewicht/256)
        auto put = [&](int x, int y, int r, int g, int b, int wgt) {
            if (y < y0 || y >= y1 || x < 0 || x >= Size || wgt <= 0) return;
            add565(w.band + (y - y0) * _fw + _pad + x, (r * wgt + 1024) >> 11, (g * wgt + 512) >> 10, (b * wgt + 1024) >> 11);
        };
        // sterren van deze strook en de buren (grote sterren en kruisjes lopen over de rand)
        const int bs = y0 / BandH;
        for (int bb = bs - 1; bb <= bs + 1; bb++) {
            if (bb < 0 || bb >= SkyBands) continue;
            for (int i = _skyBand[bb]; i < _skyBand[bb + 1]; i++) {
                const SkyDot& d = _skyDraw[i];
                if (d.rad < 0.0f || (d.rad < 0.9f && d.r + d.g + d.b < 240)) {   // stof en zwakke sterren: één pixel
                    put((int)d.x, (int)d.y, d.r, d.g, d.b, 256);   // (verdeeld over vier zouden ze wegvallen in RGB565)
                    continue;
                }
                if (d.rad < 0.9f) {   // klein: over vier pixels verdeeld (draait zonder te verspringen)
                    const int ix = (int)floorf(d.x), iy = (int)floorf(d.y);
                    const int fx = (int)((d.x - ix) * 256), fy = (int)((d.y - iy) * 256);
                    put(ix, iy, d.r, d.g, d.b, ((256 - fx) * (256 - fy)) >> 8);
                    put(ix + 1, iy, d.r, d.g, d.b, (fx * (256 - fy)) >> 8);
                    put(ix, iy + 1, d.r, d.g, d.b, ((256 - fx) * fy) >> 8);
                    put(ix + 1, iy + 1, d.r, d.g, d.b, (fx * fy) >> 8);
                    continue;
                }
                // schijfje met zachte rand
                const int e = (int)(d.rad + 1.5f);
                const int cx = (int)d.x, cy = (int)d.y;
                if (cy + e < y0 || cy - e >= y1) { if (!d.cross) continue; }
                for (int yy = cy - e; yy <= cy + e; yy++) {
                    if (yy < y0 || yy >= y1) continue;
                    for (int xx = cx - e; xx <= cx + e; xx++) {
                        const float dx = xx + 0.5f - d.x, dy = yy + 0.5f - d.y;
                        float cov = d.rad + 0.5f - sqrtf(dx * dx + dy * dy);
                        if (cov <= 0) continue;
                        put(xx, yy, d.r, d.g, d.b, cov >= 1 ? 256 : (int)(cov * 256));
                    }
                }
                if (d.cross) {   // lichtkruisje, loopt zacht uit
                    const int L = (int)(d.rad * 3.0f);
                    for (int k = e; k <= L; k++) {
                        const int wgt = 64 * (L - k) / L;
                        put(cx + k, cy, d.r, d.g, d.b, wgt); put(cx - k, cy, d.r, d.g, d.b, wgt);
                        put(cx, cy + k, d.r, d.g, d.b, wgt); put(cx, cy - k, d.r, d.g, d.b, wgt);
                    }
                }
            }
        }
        memcpy(dst + (size_t)y0 * _fw, w.band, (size_t)bh * _fw * 2);
    }
}

}  // namespace jarvis
