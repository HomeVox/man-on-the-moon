#include "anim_player.h"
#include <hal/hal.h>
#include <jpeg_decoder.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <cstring>

static const char* TAG = "JVAN";

namespace jarvis {

AnimPlayer::~AnimPlayer()
{
    if (_buf) heap_caps_free(_buf);
}

bool AnimPlayer::open()
{
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "anim");
    if (!part) {
        ESP_LOGW(TAG, "partitie 'anim' niet gevonden");
        return false;
    }
    const void* ptr = nullptr;
    esp_partition_mmap_handle_t h;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &h) != ESP_OK) {
        ESP_LOGW(TAG, "mmap van 'anim' mislukt");
        return false;
    }
    const uint8_t* d = (const uint8_t*)ptr;
    if (memcmp(d, "JVAN", 4) != 0) {
        ESP_LOGW(TAG, "geen animatiedata in 'anim' (nog niet geflasht?)");
        return false;
    }
    _clips = *(const uint16_t*)(d + 4);
    if (_clips > 8) _clips = 8;
    for (int i = 0; i < _clips; i++) {
        const uint8_t* c = d + 8 + i * 24;
        memcpy(_info[i].name, c, 12);
        _info[i].name[12] = 0;
        _info[i].frames   = *(const uint16_t*)(c + 12);
        _info[i].fps      = *(const uint16_t*)(c + 14);
        _info[i].table    = *(const uint32_t*)(c + 16);
        ESP_LOGI(TAG, "clip %d %s: %u frames @ %u fps", i, _info[i].name, _info[i].frames, _info[i].fps);
    }
    _buf = (uint16_t*)heap_caps_malloc(Size * Size * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_buf) return false;
    memset(_buf, 0, Size * Size * 2);
    _data = d;

    // Meting: decoderen op volle grootte en op 1/2 (logt een keer)
    {
        const uint32_t* ent = (const uint32_t*)(d + _info[0].table);
        for (int sc = 0; sc < 2; sc++) {
            esp_jpeg_image_cfg_t cfg = {};
            cfg.indata = (uint8_t*)(d + ent[0]);
            cfg.indata_size = ent[1];
            cfg.outbuf = (uint8_t*)_buf;
            cfg.outbuf_size = Size * Size * 2;
            cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
            cfg.out_scale = sc ? JPEG_IMAGE_SCALE_1_2 : JPEG_IMAGE_SCALE_0;
            esp_jpeg_image_output_t out;
            int64_t t0 = esp_timer_get_time();
            for (int k = 0; k < 5; k++) esp_jpeg_decode(&cfg, &out);
            ESP_LOGI(TAG, "meting schaal %d: %d ms per beeld (%ux%u)", sc,
                     (int)((esp_timer_get_time() - t0) / 5000), out.width, out.height);
        }
    }
    return true;
}

void AnimPlayer::setClip(int clip)
{
    if (clip >= 0 && clip < _clips) _pending = clip;
}

int AnimPlayer::fps() const
{
    return _clips ? _info[_clip].fps : 20;
}

bool AnimPlayer::step(uint32_t* decodeMs, uint32_t* pushMs)
{
    if (!_data) return false;
    if (_pending >= 0) {
        _clip    = _pending;
        _frame   = 0;
        _pending = -1;
    }
    const ClipInfo& ci = _info[_clip];
    const uint32_t* ent = (const uint32_t*)(_data + ci.table) + _frame * 2;
    const uint8_t* jpg  = _data + ent[0];

    int64_t t0 = esp_timer_get_time();
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata       = (uint8_t*)jpg;
    cfg.indata_size  = ent[1];
    cfg.outbuf       = (uint8_t*)_buf;
    cfg.outbuf_size  = Size * Size * 2;
    cfg.out_format   = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale    = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_output_t out;
    esp_err_t err = esp_jpeg_decode(&cfg, &out);
    int64_t t1 = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decodeerror %d (clip %d frame %d)", (int)err, _clip, _frame);
        return false;
    }
    GetHAL().pushFrame(_buf);
    int64_t t2 = esp_timer_get_time();
    if (decodeMs) *decodeMs = (uint32_t)((t1 - t0) / 1000);
    if (pushMs) *pushMs = (uint32_t)((t2 - t1) / 1000);

    _frame = (_frame + 1) % ci.frames;
    return true;
}

}  // namespace jarvis
