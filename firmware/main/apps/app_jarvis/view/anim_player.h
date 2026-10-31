/*
 * Jarvis - speelt voorgerenderde lussen af (JPEG-frames in de flashpartitie "anim").
 * Gemaakt door tools/render_loops.py --build, geflasht met esptool op de partitie-offset.
 */
#pragma once
#include <cstdint>
#include <cstddef>

namespace jarvis {

class AnimPlayer {
public:
    static constexpr int Size = 466;

    ~AnimPlayer();
    bool open();                    // partitie koppelen en header lezen
    bool ready() const { return _data != nullptr; }
    int clipCount() const { return _clips; }

    void setClip(int clip);         // wisselt bij het eerstvolgende beeld, begint op beeld 0
    int clip() const { return _clip; }

    // Decodeer en toon het volgende beeld; geeft de gemeten tijden terug (ms)
    bool step(uint32_t* decodeMs = nullptr, uint32_t* pushMs = nullptr);
    int fps() const;

private:
    const uint8_t* _data = nullptr;
    int _clips           = 0;
    int _clip            = 0;
    int _pending         = -1;
    int _frame           = 0;
    uint16_t* _buf       = nullptr;

    struct ClipInfo { char name[13]; uint16_t frames, fps; uint32_t table; };
    ClipInfo _info[8]{};
};

}  // namespace jarvis
