/*
 * Man on the Moon - weergave: planeten, audiogolf en sterrenhemel.
 * Port van prototypes/losse-draden.html.
 *
 * Snel tekenen op de ESP32-S3 (zonder LVGL):
 *  - beide kernen tekenen elk een helft van het beeld, in stroken in intern RAM;
 *  - terwijl beeld N naar het scherm gaat, wordt beeld N+1 al getekend (twee beeldbuffers).
 */
#pragma once
#include <array>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace jarvis {

enum class State { Idle, Listening, Thinking, Speaking, Making };
constexpr int StateCount = 5;

const char* stateName(State s);

class JarvisView {
public:
    static constexpr int Size    = 466;
    static constexpr int Strands = 26;   // prototype: 38; minder = sneller
    static constexpr int Points  = 91;   // prototype: 121
    static constexpr int BandH   = 24;   // strookhoogte in intern RAM

    ~JarvisView();
    bool create();                       // buffers en werktaken

    // Testbeeld (rooster, cirkels, gekleurde randen) om vervorming op het scherm te zien
    void showTestPattern();

    // Aanraking: de draden worden naar de vinger getrokken en veren er daarna organisch terug
    void setTouch(bool down, float x, float y)
    {
        _touchDown = down;
        if (down) { _tgx = x; _tgy = y; if (!_touchWas) { _tx = x; _ty = y; } }
        _touchWas = down;
    }

    // 0..2: heftigheid van de golf (1 = normaal); komt uit HA
    void setGain(float g) { _gain = g < 0 ? 0 : (g > 2 ? 2 : g); }
    // 0..2: hoe sterk de melkweg (sterrenhemel) op de muziek reageert; komt uit HA (schuif Melkweg)
    void setMelkweg(float m) { _melk = m < 0 ? 0 : (m > 2 ? 2 : m); }

    // Start als audiogolf: begint als een punt in het midden en groeit naar links en rechts uit
    void startAsWave() { _morph = 2.0f; _wave = 1.0f; _reveal = 0.0f; }

    // 20 spectrumbanden (laag -> hoog, 0..1): bas in het midden van de golf, hoge tonen aan de uiteinden
    void setBands(const float* b) { for (int i = 0; i < 20; i++) _bands[i] = b[i]; }
    // 12 tonen C..B (0..1): in de cirkel heeft elke draad een toon
    void setNotes(const float* n) { for (int i = 0; i < 12; i++) _notes[i] = n[i]; }

    // Kanteling (g, schermassen): het beeld is een vlak in de ruimte dat met de zwaartekracht meedraait (waterpas)
    // en bij voor-/achterover kantelen in perspectief komt; de zwaartekracht wordt door een veer gevolgd (vloeistof).
    // 0 rechte golf, 1 sterrenhemel (was cirkel), 2 water (niet meer in gebruik); water begint telkens als klodder in het midden
    // 3 = aarde (gestileerde aarde uit de NASA-kaart, belicht van linksboven, draait; vinger draait mee)
    // 4 = maan (voorkant; vinger draait mee, veert terug); 5 = Mars, 6 = Jupiter (zelfde tabel, vol licht, draaien vanzelf)
    void setMode(int m)
    {
        m = m < 0 ? 0 : (m > 6 ? 6 : m);
        if (m != _mode && _mode >= 2) _reveal = 0.0f;
        if (m == 2 && _mode != 2) _waterInit = false;
        _mode = m;
    }
    void setTiltStrength(float s) { _tiltStrength = s < 0 ? 0 : (s > 1 ? 1 : s); }
    void setTilt(float ax, float ay, float az) { _ax = ax; _ay = ay; _az = az; }
    void setKlok(bool on) { _klokOn = on; }   // tijd boven de bollen
    void setNacht(bool on) { _nacht = on; }   // nachtklok: alles amber, zonder puntjes
    static bool tijdBekend();                 // klok gezet via internet

    void setState(State s) { _state = s; }
    State state() const { return _state; }

    // 0..1: stemniveau; versterkt de energie van de draden
    void setLevel(float level) { _levelTarget = level < 0 ? 0 : (level > 1 ? 1 : level); }

    // Rekent de animatie een stap door, tekent het volgende beeld en toont het vorige.
    // Geeft de tijd (ms) die het wachten op het tekenen kostte, voor de meting.
    void tick(uint32_t nowMs);

    uint32_t lastRenderMs() const { return _renderMs; }
    uint32_t lastPushMs() const { return _pushMs; }
    uint32_t lastPointsMs() const { return _pointsMs; }
    uint32_t lastWorkMs() const { return _workMs[0] > _workMs[1] ? _workMs[0] : _workMs[1]; }

private:
    State _state = State::Idle;
    float _levelTarget = 0, _level = 0;
    float _gain = 1.0f, _reveal = 1.0f;
    int _mode = 0;
    float _ring = 0;   // 0..1: audiogolf wordt cirkel
    float _ax = 0, _ay = 0, _az = 1, _tiltStrength = 0.0f;
    float _g[3] = {0, 1, 0}, _gv[3] = {0, 0, 0}, _use3d = 0;   // gevolgde zwaartekracht (veer), snelheid, hoeveel 3D
    float _rm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float _ang = 0, _angV = 0, _angGoal = 0;   // waterpas: huidige hoek, snelheid en doel (rad)
    float _bands[20] = {};
    float _notes[12] = {};
    float _bassAvg = 0.05f, _beat = 0;
    uint32_t _lastBeat = 0;
    float _hist[48] = {};  int _histPos = 0;      // stemniveau van de laatste beelden: de golf loopt vanuit het midden naar buiten
    bool _touchDown = false, _touchWas = false;
    float _tgx = 0, _tgy = 0, _tx = 0, _ty = 0;   // doel en gevolgde positie van de vinger
    float _tAmt = 0, _tVel = 0;                   // sterkte van de vervorming (veer: wiebelt na loslaten)
    float _t = 0, _morph = 0, _wave = 0, _idleClock = 0;  // _morph = vorm 0..2: orb, rand-ring, lijn
    float _speed = 0.7f, _energy = 0.65f, _spread = 0.92f, _bright = 0.8f;
    uint32_t _last = 0;
    uint32_t _renderMs = 0, _pushMs = 0, _pointsMs = 0;
    volatile uint32_t _workMs[2] = {0, 0};

    struct Params {
        float t, morph, energy, spread, bright, wave, tx, ty, tAmt;
        float gain, reveal, beat, use3d, ring, water;
    float rm[9];
    float bd[20];
    float h[16];  // stemgeschiedenis, 0 = nu, daarna steeds ouder (voor het uitlopen van de golf)
    float nt[12]; // tonen C..B
    };
    Params _p{};

    int _fw = 466, _pad = 0;                  // schermbreedte en links-marge van het beeld
    uint16_t* _buf[2] = {nullptr, nullptr};  // volledige beelden in PSRAM
    int _render = 0, _show = 1;
    bool _haveFrame = false;

    struct Strand { int r, g, b; int gain; };  // kleur en intensiteit per draad (vaste komma)
    std::array<Strand, Strands> _col{}, _colB{};   // _colB: palet voor de cirkel, draad = toonbereik

    struct Worker {
        JarvisView* self;
        int half;
        TaskHandle_t task;
        SemaphoreHandle_t done;
        uint16_t* band;
    };
    // gedeeld: puntenwolk van alle draden, eenmalig per beeld berekend. Twee sets: de werktaken tekenen set
    // 1 - _setW (vorige keer berekend) terwijl de hoofdtaak al de volgende in set _setW rekent.
    float _sy0s[2][Strands] = {}, _sy1s[2][Strands] = {};   // laagste en hoogste y per draad (om lege stroken over te slaan)
    bool _blank[2][2 * 21] = {};              // per beeldbuffer: was deze strook al achtergrond?
    float _pxs[2][Strands][Points] = {};
    float _pys[2][Strands][Points] = {};
    int _setW = 0;
    struct RParams { float bright, ring, water, earth, ang; };   // ang: waterpashoek van de bollen (rad)
    RParams _rp{}, _rpNext{};                 // tekeninstellingen die bij de te tekenen set horen
    // vaste tabellen per punt langs de draad (u = 0..1)
    struct UTab { float s[6], c[6], env; };
    UTab _u[Points];
    void computePoints();
    Worker _w[2]{};

    // water: deeltjes-vloeistof (dubbele-dichtheid-relaxatie, Clavet e.a. 2005); getekend als korte lichtstreepjes
    static constexpr int WaterN = 420;
    float _wx[WaterN], _wy[WaterN], _wvx[WaterN], _wvy[WaterN], _wox[WaterN], _woy[WaterN], _wd[WaterN];
    int16_t _wNext[WaterN], _wHead[13 * 13];
    float _fPx = 0, _fPy = 0;                 // vorige vingerpositie (roeren in het water)
    bool _fOn = false;
    uint16_t _wOrder[WaterN];
    bool _waterInit = false;
    uint32_t _waterBeatSeen = 0, _rng = 0x9E3779B9u;
    void computeWater(float dt);

    // aarde: bol per pixel uit een vaste tabel (kaartplek + belichting per schermpixel), de kaart schuift met de draaiing
    static constexpr int EarthW = 1024, EarthH = 512, EarthR = 145, EarthCy = 233;
    uint16_t* _eDay = nullptr;                // kaart RGB565 (PSRAM)
    uint32_t* _ePix = nullptr;                // per pixel: kolom (10 bit) | rij (9) << 10 | licht (8) << 19 | waas (5) << 27
    int _eRow[Size + 1] = {}, _eX0[Size] = {};   // begin in _ePix en eerste x per schermregel
    bool _eReady = false, _eFailed = false;
    float _spin = 0, _spinV = 0, _ePrevX = 0, _ePrevY = 0;
    bool _ePrevDown = false;
    int _eSu = 0;                             // draaiing in kaartkolommen
    float _eGlowAmt = 1.0f;                   // sterkte van de waas (vast)
    bool _eGeom = false;                      // _eRow/_eX0 gevuld
    int _eBody = 0;                           // welke bol de draaiing nu volgt (3 aarde, 4 maan)
    // maan: eigen kaart en tabel (zelfde schijf), licht volgens de schijngestalte
    uint16_t* _bTex[3] = {};                  // kaarten maan, Mars, Jupiter (blijven geladen)
    bool _bFailed[3] = {};
    uint32_t* _mPix = nullptr;
    // klok: alleen de tijd, klein en wit boven de bol, eens per minuut getekend (native RGB565, Size x KlokH vanaf KlokY)
    static constexpr int KlokY = 36, KlokH = 52;
    static constexpr int DotY = 418, DotGap = 16;   // modus-puntjes onder de bol
    bool _klokOn = true;
    volatile bool _nacht = false;
    bool _eNightMap = false;                  // de aardkaart is nu de nachtkaart (stadslichtjes)
    uint16_t* _klok = nullptr;
    volatile bool _klokShow = false;
    int _klokMin = -1;
    void updateKlok();
    
    float _mFase = -1.0f, _eFase = -1.0f;    // schijngestalte waarmee de tabellen van maan en aarde gebouwd zijn
    uint32_t _mCheck = 0;
    void globeGeom();
    bool bodyInit(int b);
    bool globeReady() const;
    void buildMoon(float fase);
    void buildEarth(float fase);
    bool earthInit();
    void computeEarth(float dt);
    void renderEarthHalf(Worker& w);

    // sterrenhemel (modus 1): sterren + melkweg-stof, per beeld schermposities per strook gesorteerd
    static constexpr int SkyStars = 700, SkyDust = 3500, SkyMaxDraw = SkyStars + SkyDust + 200;
    static constexpr int SkyBands = (Size + BandH - 1) / BandH;
    // o/v: uitwijking en snelheid door het geluid (snelle veer); p/pv: door roeren met de vinger (trage veer)
    struct SkyStar { float x, y, m, ph, f, ox, oy, vx, vy, px, py, pvx, pvy, g; uint8_t n, r, gg, b; };
    struct SkyDot { float x, y, rad; uint8_t r, g, b, cross; };   // rad -1 = stofje (één pixel)
    SkyStar* _sky = nullptr;
    float* _dust = nullptr;                   // x, y, helderheid
    SkyDot* _skyDraw = nullptr, *_skyTmp = nullptr;
    int _skyBand[SkyBands + 1] = {}, _skyN = 0;
    bool _skyReady = false, _skyFailed = false, _skyDown = false;
    float _melk = 1.0f;
    float _skyT = 0, _skyRot = 0, _skyFx = 0, _skyFy = 0, _skyFvx = 0, _skyFvy = 0;
    float _skyShoot = 0, _skyShX = 0, _skyShY = 0, _skyShVx = 0, _skyShVy = 0;   // vallende ster
    float _skyShB = 0, _skyShLen = 0, _skyNext = 0, _skyLastShoot = -10.0f, _skyHiAvg = 0.05f;
    bool skyInit();
    void computeSky(float dt);
    void renderSkyHalf(Worker& w);

    static void workerMain(void* arg);
    void renderHalf(Worker& w);
};

}  // namespace jarvis
