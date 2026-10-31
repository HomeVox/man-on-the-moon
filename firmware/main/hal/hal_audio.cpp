/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/settings/settings.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mooncake_log.h>
#include <driver/i2s_std.h>
#include <esp_dsp.h>
#include <esp_heap_caps.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <array>
#include <atomic>
#include <functional>

static const std::string_view _tag = "HAL-Audio";

#define I2S_PORT         I2S_NUM_0
#define I2S_MCLK_PIN     (gpio_num_t)18
#define I2S_BCLK_PIN     (gpio_num_t)17
#define I2S_DADC_IN_PIN  (gpio_num_t)16
#define I2S_LRCK_PIN     (gpio_num_t)15
#define I2S_DDAC_OUT_PIN (gpio_num_t)21

static class AudioCodec {
public:
    static constexpr int sample_rate       = 44100;
    static constexpr int spectrum_fft_size = 512;
    static constexpr int spectrum_hop_size = 256;

    void init(i2c_master_bus_handle_t i2c_bus)
    {
        _silence_buffer.resize(sample_rate * 0.1);
        _silence_buffer.assign(_silence_buffer.size(), 0);
        _spectrum_init();
        xTaskCreate([](void* obj) { static_cast<AudioCodec*>(obj)->_task_entry(); }, "audio_task", 4 * 1024, this, 5,
                    &_task_handle);

        _i2s_init();

        audio_codec_i2s_cfg_t i2s_cfg = {
            .rx_handle = _rx_handle,
            .tx_handle = _tx_handle,
        };
        _data_if = audio_codec_new_i2s_data(&i2s_cfg);

        audio_codec_i2c_cfg_t i2c_cfg = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus};
        _ctrl_if                      = audio_codec_new_i2c_ctrl(&i2c_cfg);

        _gpio_if = audio_codec_new_gpio();

        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if     = _ctrl_if,
            .gpio_if     = _gpio_if,
            .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin      = GPIO_NUM_NC,
            .pa_reverted = false,
            .use_mclk    = true,
        };
        _codec_if = es8311_codec_new(&es8311_cfg);

        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = _codec_if,
            .data_if  = _data_if,
        };
        _codec_dev = esp_codec_dev_new(&dev_cfg);

        esp_codec_dev_set_in_gain(_codec_dev, 30.0);  // 36 dB gaf klippen (piek 32768) en slechte herkenning

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = 1,
            .sample_rate     = sample_rate,
        };
        esp_codec_dev_open(_codec_dev, &fs);

        xTaskCreatePinnedToCore([](void* obj) { static_cast<AudioCodec*>(obj)->_mic_task(); }, "mic_task", 4 * 1024,
                                this, 4, nullptr, 0);
    }

    void updateSpectrum(Hal::AudioSpectrumFrame& frame)
    {
        if (_spectrum_available == false) {
            return;
        }

        if (_read_spectrum_hop() == false) {
            return;
        }

        if (_spectrum_samples_ready < spectrum_fft_size) {
            return;
        }

        _process_spectrum_frame(frame);
    }

    void setVolume(int volume)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        esp_codec_dev_set_out_vol(_codec_dev, volume);
    }

    int getVolume()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        int volume = 0;
        esp_codec_dev_get_out_vol(_codec_dev, &volume);
        return volume;
    }

    void setMicGain(float gain)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        esp_codec_dev_set_in_gain(_codec_dev, gain);
    }

    void play(std::vector<int16_t>& data, bool async)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (async) {
            // Support interruption: overwrite data and notify task
            _audio_data = data;
            _is_playing = true;
            xTaskNotifyGive(_task_handle);
        } else {
            if (_is_playing) {
                mclog::tagWarn(_tag, "audio is playing");
                return;
            }
            _write(data);
        }
    }

    // Microfoon-monitor: leest doorlopend (11 ms blokken) zolang actief, berekent een niveau (0..1)
    // en geeft de ruwe blokken (mono, 44,1 kHz) aan een optionele afnemer.
    void setMicActive(bool on) { _mic_active = on; }
    float micLevel() const { return _mic_level; }
    // 20 spectrumbanden (laag -> hoog, 0..1) van de laatste microfoonblok; alleen bijgewerkt terwijl de microfoon actief is
    void micBands(float* out) const
    {
        for (std::size_t i = 0; i < Hal::AudioSpectrumFrame::bandCount; i++) out[i] = _mic_bands[i];
    }
    // 12 tonen (C, C#, ... B; alle octaven opgeteld), 0..1 ten opzichte van de sterkste toon; 0 bij stilte
    void micNotes(float* out) const
    {
        for (int i = 0; i < 12; i++) out[i] = _mic_notes[i];
    }
    void setMicSink(std::function<void(const int16_t*, size_t)> sink)
    {
        std::lock_guard<std::mutex> lock(_sink_mutex);
        _mic_sink = std::move(sink);
    }

    void record(std::vector<int16_t>& data, uint16_t durationMs, float gain)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        esp_codec_dev_set_in_gain(_codec_dev, gain);

        size_t sample_count = (size_t)(sample_rate * durationMs / 1000);
        size_t byte_size    = sample_count * sizeof(int16_t);

        data.resize(sample_count);

        esp_err_t ret = esp_codec_dev_read(_codec_dev, data.data(), byte_size);
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "record failed: {}", ret);
            data.clear();
        }
    }

private:
    std::atomic<bool> _mic_active{false};
    std::atomic<float> _mic_level{0.0f};
    std::array<std::atomic<float>, 20> _mic_bands{};
    std::array<std::atomic<float>, 12> _mic_notes{};
    std::mutex _sink_mutex;

    // Tonen: 4x teruggesampled (11025 Hz) en een FFT van 1024 punten = 10,8 Hz per bin, venster 93 ms.
    // Elke bin tussen ~86 Hz en 2 kHz telt mee voor de dichtstbijzijnde toon (driehoeksgewicht).
    static constexpr int note_fft  = 1024;
    static constexpr int note_dec  = 4;
    static constexpr int note_bin0 = 8, note_bin1 = 186;
    float* _note_ring = nullptr;    // laatste 1024 teruggesamplede samples (PSRAM)
    float* _note_win  = nullptr;
    float* _note_fft  = nullptr;    // complex, 2 x 1024
    int _note_pos = 0, _note_blocks = 0;
    int8_t _note_pc[note_bin1 + 1] = {};
    float _note_w[note_bin1 + 1]   = {};
    float _note_peak = 1e-4f;
    float _note_out[12] = {};

    void _notes_init()
    {
        _note_ring = (float*)heap_caps_calloc(note_fft, sizeof(float), MALLOC_CAP_SPIRAM);
        _note_win  = (float*)heap_caps_calloc(note_fft, sizeof(float), MALLOC_CAP_SPIRAM);
        _note_fft  = (float*)heap_caps_calloc(note_fft * 2, sizeof(float), MALLOC_CAP_SPIRAM);
        if (!_note_ring || !_note_win || !_note_fft) { mclog::tagError(_tag, "notes: geen geheugen"); return; }
        dsps_wind_hann_f32(_note_win, note_fft);
        const float fs = (float)sample_rate / note_dec;
        for (int b = note_bin0; b <= note_bin1; b++) {
            float f    = b * fs / note_fft;
            float midi = 69.0f + 12.0f * log2f(f / 440.0f);
            float near = roundf(midi);
            _note_pc[b] = (int8_t)(((int)near % 12 + 12) % 12);
            _note_w[b]  = 1.0f - 2.0f * fabsf(midi - near);
        }
    }

    // per microfoonblok (512 samples): terugsamplen; elk tweede blok de tonen opnieuw berekenen
    void _notes_block(const int16_t* buf, size_t n, float level)
    {
        if (!_note_fft || !_spectrum_available) return;
        for (size_t i = 0; i + note_dec <= n; i += note_dec) {
            float s = 0.0f;
            for (int k = 0; k < note_dec; k++) s += buf[i + k];
            _note_ring[_note_pos] = s / (note_dec * 32768.0f);
            _note_pos = (_note_pos + 1) % note_fft;
        }
        if (++_note_blocks % 2) return;
        for (int i = 0; i < note_fft; i++) {
            _note_fft[i * 2]     = _note_ring[(_note_pos + i) % note_fft] * _note_win[i];
            _note_fft[i * 2 + 1] = 0.0f;
        }
        if (dsps_fft2r_fc32(_note_fft, note_fft) != ESP_OK) return;
        dsps_bit_rev_fc32(_note_fft, note_fft);
        float e[12] = {};
        for (int b = note_bin0; b <= note_bin1; b++) {
            float re = _note_fft[b * 2], im = _note_fft[b * 2 + 1];
            e[_note_pc[b]] += (re * re + im * im) * _note_w[b];
        }
        float mx = 0.0f;
        for (int k = 0; k < 12; k++) { e[k] = sqrtf(e[k]); if (e[k] > mx) mx = e[k]; }
        _note_peak = fmaxf(_note_peak * 0.995f, mx);
        float gate = fminf(1.0f, level * 3.0f);   // stilte: geen tonen
        for (int k = 0; k < 12; k++) {
            float v = e[k] / _note_peak;
            v       = v * v * gate;   // contrast: de duidelijke tonen springen eruit
            _note_out[k] += (v - _note_out[k]) * (v > _note_out[k] ? 0.6f : 0.15f);
            _mic_notes[k] = _note_out[k];
        }
    }
    std::function<void(const int16_t*, size_t)> _mic_sink;

    void _mic_task()
    {
        static constexpr size_t block = 512;  // 11,6 ms bij 44,1 kHz
        std::vector<int16_t> buf(block);
        float floor_rms = 80.0f;  // ruisvloer, past zich langzaam aan
        float level     = 0.0f;
        bool was_active = false;
        for (;;) {
            if (!_mic_active) {
                if (was_active) {
                    was_active = false;
                    _mic_level = 0.0f;
                }
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
            was_active = true;
            if (esp_codec_dev_read(_codec_dev, buf.data(), block * sizeof(int16_t)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            double acc = 0;
            for (int16_t v : buf) acc += (double)v * v;
            float rms = sqrtf((float)(acc / block));
            // ruisvloer volgt omlaag snel, omhoog heel langzaam
            floor_rms += (rms < floor_rms ? 0.2f : 0.002f) * (rms - floor_rms);
            float above = rms - floor_rms * 1.5f;
            float target = above <= 0 ? 0.0f : fminf(1.0f, powf(above / 3500.0f, 0.6f));
            level += (target - level) * (target > level ? 0.6f : 0.15f);  // snel op, rustig af
            _mic_level = level;
            if (_spectrum_available) {   // spectrum uit hetzelfde blok (512 samples = FFT-grootte)
                for (int i = 0; i < spectrum_fft_size; ++i) _spectrum_time_domain[i] = (float)buf[i] / 32768.0f;
                _spectrum_samples_ready = spectrum_fft_size;
                Hal::AudioSpectrumFrame fr;
                _process_spectrum_frame(fr);
                for (std::size_t b = 0; b < Hal::AudioSpectrumFrame::bandCount; b++) _mic_bands[b] = fr.bands[b];
                _notes_block(buf.data(), block, level);
            }
            std::lock_guard<std::mutex> lock(_sink_mutex);
            if (_mic_sink) _mic_sink(buf.data(), block);
        }
    }

    void _task_entry()
    {
        mclog::tagInfo(_tag, "start audio play task");
        std::vector<int16_t> current_data;

        while (1) {
            // Wait for play request
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            while (true) {
                // Fetch data safely
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    if (_audio_data.empty()) {
                        _is_playing = false;
                        break;
                    }
                    current_data.swap(_audio_data);   // overnemen zonder kopie
                    std::vector<int16_t>().swap(_audio_data);   // en het geheugen echt vrijgeven
                    _is_playing = true;
                }

                if (current_data.empty()) {
                    break;
                }

                size_t offset        = 0;
                size_t total_samples = current_data.size();
                bool interrupted     = false;
                // Chunk size in samples (e.g. 1024 bytes = 512 samples)
                const size_t CHUNK_SAMPLES = 512;

                while (offset < total_samples) {
                    // Check for interruption (new play request)
                    if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
                        // mclog::tagInfo(_tag, "playback interrupted");
                        interrupted = true;
                        break;
                    }

                    size_t remain        = total_samples - offset;
                    size_t write_samples = (remain > CHUNK_SAMPLES) ? CHUNK_SAMPLES : remain;

                    esp_codec_dev_write(_codec_dev, (void*)&current_data[offset], write_samples * sizeof(int16_t));
                    offset += write_samples;
                }

                if (interrupted) {
                    // Stop current playback immediately and flush DMA
                    i2s_channel_disable(_tx_handle);
                    i2s_channel_enable(_tx_handle);
                    continue;
                }

                // Normal finish, play silence to avoid pop/waiting
                esp_codec_dev_write(_codec_dev, (void*)_silence_buffer.data(),
                                    _silence_buffer.size() * sizeof(int16_t));
                std::vector<int16_t>().swap(current_data);   // klaar: geluid niet in het geheugen laten staan
            }
        }
    }

    void _write(const std::vector<int16_t>& data)
    {
        esp_codec_dev_write(_codec_dev, (void*)data.data(), data.size() * sizeof(int16_t));
        esp_codec_dev_write(_codec_dev, (void*)_silence_buffer.data(), _silence_buffer.size() * sizeof(int16_t));
    }

    void _i2s_init()
    {
        mclog::tagInfo(_tag, "i2s init");

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
        i2s_std_config_t std_cfg   = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg =
                {
                    .mclk = I2S_MCLK_PIN,
                    .bclk = I2S_BCLK_PIN,
                    .ws   = I2S_LRCK_PIN,
                    .dout = I2S_DDAC_OUT_PIN,
                    .din  = I2S_DADC_IN_PIN,
                },
        };

        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &_tx_handle, &_rx_handle));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(_tx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(_rx_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(_tx_handle));
        ESP_ERROR_CHECK(i2s_channel_enable(_rx_handle));
    }

    void _spectrum_init()
    {
        esp_err_t ret = dsps_fft2r_init_fc32(nullptr, note_fft);   // tabel voor de grootste FFT (tonen); 512 past erin
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "fft init failed: {}", ret);
            return;
        }
        _notes_init();

        dsps_wind_hann_f32(_spectrum_window.data(), spectrum_fft_size);

        constexpr int max_bin = spectrum_fft_size / 2;
        const float nyquist   = static_cast<float>(sample_rate) * 0.5f;
        const float min_hz    = static_cast<float>(sample_rate) / static_cast<float>(spectrum_fft_size);
        const float log_min   = std::log10(min_hz);
        const float log_max   = std::log10(nyquist);

        _band_bin_edges[0] = 1;
        for (std::size_t i = 1; i < Hal::AudioSpectrumFrame::bandCount; ++i) {
            float t            = static_cast<float>(i) / static_cast<float>(Hal::AudioSpectrumFrame::bandCount);
            float edge_hz      = std::pow(10.0f, log_min + (log_max - log_min) * t);
            int edge_bin       = static_cast<int>(std::lround(edge_hz * spectrum_fft_size / sample_rate));
            int min_edge       = _band_bin_edges[i - 1] + 1;
            int max_edge       = max_bin - static_cast<int>(Hal::AudioSpectrumFrame::bandCount - i);
            _band_bin_edges[i] = std::clamp(edge_bin, min_edge, max_edge);
        }
        _band_bin_edges[Hal::AudioSpectrumFrame::bandCount] = max_bin;
        _spectrum_available                                 = true;
    }

    bool _read_spectrum_hop()
    {
        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (lock.owns_lock() == false) {
            return false;
        }

        esp_err_t ret = esp_codec_dev_read(_codec_dev, _spectrum_pcm_hop.data(), sizeof(int16_t) * spectrum_hop_size);
        if (ret != ESP_OK) {
            return false;
        }

        std::move(_spectrum_time_domain.begin() + spectrum_hop_size, _spectrum_time_domain.end(),
                  _spectrum_time_domain.begin());

        for (int i = 0; i < spectrum_hop_size; ++i) {
            _spectrum_time_domain[spectrum_fft_size - spectrum_hop_size + i] =
                static_cast<float>(_spectrum_pcm_hop[i]) / 32768.0f;
        }

        _spectrum_samples_ready = std::min<std::size_t>(_spectrum_samples_ready + spectrum_hop_size, spectrum_fft_size);
        return true;
    }

    void _process_spectrum_frame(Hal::AudioSpectrumFrame& frame)
    {
        float mean = 0.0f;
        for (float sample : _spectrum_time_domain) {
            mean += sample;
        }
        mean /= static_cast<float>(spectrum_fft_size);

        for (int i = 0; i < spectrum_fft_size; ++i) {
            float sample                    = (_spectrum_time_domain[i] - mean) * _spectrum_window[i];
            _spectrum_fft_buffer[i * 2]     = sample;
            _spectrum_fft_buffer[i * 2 + 1] = 0.0f;
        }

        if (dsps_fft2r_fc32(_spectrum_fft_buffer.data(), spectrum_fft_size) != ESP_OK) {
            return;
        }
        if (dsps_bit_rev_fc32(_spectrum_fft_buffer.data(), spectrum_fft_size) != ESP_OK) {
            return;
        }

        float peak_bin_magnitude = 0.0f;
        int peak_bin_index       = 0;
        float top1               = 0.0f;
        float top2               = 0.0f;
        float top3               = 0.0f;

        for (std::size_t band = 0; band < Hal::AudioSpectrumFrame::bandCount; ++band) {
            int start_bin = _band_bin_edges[band];
            int end_bin   = _band_bin_edges[band + 1];
            float energy  = 0.0f;
            float peak    = 0.0f;
            int count     = 0;

            for (int bin = start_bin; bin < end_bin; ++bin) {
                float re  = _spectrum_fft_buffer[bin * 2];
                float im  = _spectrum_fft_buffer[bin * 2 + 1];
                float mag = std::sqrt(re * re + im * im) * (2.0f / static_cast<float>(spectrum_fft_size));
                if (mag > peak_bin_magnitude) {
                    peak_bin_magnitude = mag;
                    peak_bin_index     = bin;
                }
                peak = std::max(peak, mag);
                energy += mag * mag;
                ++count;
            }

            float rms = count > 0 ? std::sqrt(energy / static_cast<float>(count)) : 0.0f;
            float raw = rms * 0.48f + peak * 0.52f;
            float low_emphasis =
                1.12f - 0.22f * (static_cast<float>(band) / static_cast<float>(Hal::AudioSpectrumFrame::bandCount - 1));
            raw *= low_emphasis;

            float floor_alpha = raw < _spectrum_noise_floor[band] ? 0.45f : 0.004f;
            _spectrum_noise_floor[band] += (raw - _spectrum_noise_floor[band]) * floor_alpha;
            raw                       = std::max(raw - (_spectrum_noise_floor[band] * 2.20f + 0.0018f), 0.0f);
            _spectrum_raw_bands[band] = raw;

            if (raw >= top1) {
                top3 = top2;
                top2 = top1;
                top1 = raw;
            } else if (raw >= top2) {
                top3 = top2;
                top2 = raw;
            } else if (raw > top3) {
                top3 = raw;
            }
        }

        float frame_reference = std::max(top1 * 0.80f + top2 * 0.14f + top3 * 0.06f, 0.0015f);
        float norm_alpha      = frame_reference > _spectrum_normalization_level ? 0.44f : 0.16f;
        _spectrum_normalization_level += (frame_reference - _spectrum_normalization_level) * norm_alpha;
        _spectrum_normalization_level = std::clamp(_spectrum_normalization_level, 0.0015f, 1.0f);

        if (peak_bin_magnitude > 0.0f) {
            float refined_bin = static_cast<float>(peak_bin_index);
            if (peak_bin_index > 1 && peak_bin_index < (spectrum_fft_size / 2 - 1)) {
                float left_re      = _spectrum_fft_buffer[(peak_bin_index - 1) * 2];
                float left_im      = _spectrum_fft_buffer[(peak_bin_index - 1) * 2 + 1];
                float right_re     = _spectrum_fft_buffer[(peak_bin_index + 1) * 2];
                float right_im     = _spectrum_fft_buffer[(peak_bin_index + 1) * 2 + 1];
                float center_power = peak_bin_magnitude * peak_bin_magnitude;
                float left_power   = left_re * left_re + left_im * left_im;
                float right_power  = right_re * right_re + right_im * right_im;
                float denom        = left_power - 2.0f * center_power + right_power;

                if (std::fabs(denom) > 1e-9f) {
                    float offset = 0.5f * (left_power - right_power) / denom;
                    refined_bin += std::clamp(offset, -0.5f, 0.5f);
                }
            }
            frame.peakFrequencyHz =
                refined_bin * static_cast<float>(sample_rate) / static_cast<float>(spectrum_fft_size);
        } else {
            frame.peakFrequencyHz = 0.0f;
        }

        for (std::size_t band = 0; band < Hal::AudioSpectrumFrame::bandCount; ++band) {
            float ratio      = _spectrum_raw_bands[band] / _spectrum_normalization_level;
            float normalized = std::clamp(std::pow(ratio, 0.55f), 0.0f, 1.0f);
            if (normalized < 0.035f) {
                normalized = 0.0f;
            }

            float smooth_alpha = normalized > _spectrum_smoothed_bands[band] ? 0.82f : 0.40f;
            _spectrum_smoothed_bands[band] += (normalized - _spectrum_smoothed_bands[band]) * smooth_alpha;
            frame.bands[band] = std::clamp(_spectrum_smoothed_bands[band], 0.0f, 1.0f);
        }
    }

    i2s_chan_handle_t _tx_handle          = NULL;
    i2s_chan_handle_t _rx_handle          = NULL;
    esp_codec_dev_handle_t _codec_dev     = NULL;
    const audio_codec_data_if_t* _data_if = NULL;
    const audio_codec_ctrl_if_t* _ctrl_if = NULL;
    const audio_codec_gpio_if_t* _gpio_if = NULL;
    const audio_codec_if_t* _codec_if     = NULL;

    TaskHandle_t _task_handle;
    std::mutex _mutex;
    std::vector<int16_t> _audio_data;
    std::vector<int16_t> _silence_buffer;
    std::array<int16_t, spectrum_hop_size> _spectrum_pcm_hop                       = {};
    std::array<float, spectrum_fft_size> _spectrum_time_domain                     = {};
    std::array<float, spectrum_fft_size> _spectrum_window                          = {};
    std::array<float, spectrum_fft_size * 2> _spectrum_fft_buffer                  = {};
    std::array<float, Hal::AudioSpectrumFrame::bandCount> _spectrum_raw_bands      = {};
    std::array<float, Hal::AudioSpectrumFrame::bandCount> _spectrum_smoothed_bands = {};
    std::array<float, Hal::AudioSpectrumFrame::bandCount> _spectrum_noise_floor    = {};
    std::array<int, Hal::AudioSpectrumFrame::bandCount + 1> _band_bin_edges        = {};
    std::size_t _spectrum_samples_ready                                            = 0;
    float _spectrum_normalization_level                                            = 0.03f;
    bool _spectrum_available                                                       = false;
    bool _is_playing                                                               = false;
} _audio_codec;

// Jarvis: microfoon (en straks speaker) aan. (Kantoor had dit uit: scheelt DMA-buffers en een taak.)
// Oud commentaar Kantoor: speaker en microfoon worden niet gebruikt. Uit laten scheelt I2S-DMA-buffers
// en een taak in het krappe interne RAM. Alle audiofuncties doen dan niets.
static constexpr bool _audioEnabled = true;

void Hal::audio_init()
{
    mclog::tagInfo(_tag, "init");

    if (!_audioEnabled) {
        ioe_speaker_enable(false);
        mclog::tagInfo(_tag, "audio disabled");
        return;
    }

    _audio_codec.init(i2c_bus_get_internal_bus_handle(_i2c_bus));

    ioe_speaker_enable(true);

    // Load volume from settings
    setSpeakerVolume(getSpeakerVolume(true), false);
}

void Hal::setSpeakerVolume(int volume, bool saveToSettings)
{
    _spk_volume = volume;
    _spk_volume = uitk::clamp(_spk_volume, 0, 100);

    mclog::tagInfo(_tag, "set speaker volume to {}", _spk_volume);
    if (_audioEnabled) _audio_codec.setVolume(_spk_volume);

    if (saveToSettings) {
        Settings settings(std::string(Hal::SettingsNs), true);
        settings.SetInt("spk_vol", _spk_volume);
        mclog::tagInfo(_tag, "volume saved to settings: {}", _spk_volume);
    }
}

int Hal::getSpeakerVolume(bool loadFromSettings)
{
    if (_audioEnabled) _spk_volume = _audio_codec.getVolume();

    if (loadFromSettings) {
        Settings settings(std::string(Hal::SettingsNs), false);
        _spk_volume = settings.GetInt("spk_vol", 80);
        _spk_volume = uitk::clamp(_spk_volume, 0, 100);
        mclog::tagInfo(_tag, "volume loaded from settings: {}", _spk_volume);
    }

    return _spk_volume;
}

void Hal::setMicActive(bool on)
{
    if (!_audioEnabled) return;
    _audio_codec.setMicActive(on);
}

void Hal::getMicBands(float* out20)
{
    if (_audioEnabled) _audio_codec.micBands(out20);
    else for (int i = 0; i < 20; i++) out20[i] = 0.0f;
}

void Hal::getMicNotes(float* out12)
{
    if (_audioEnabled) _audio_codec.micNotes(out12);
    else for (int i = 0; i < 12; i++) out12[i] = 0.0f;
}

float Hal::getMicLevel()
{
    return _audioEnabled ? _audio_codec.micLevel() : 0.0f;
}

void Hal::setMicSink(std::function<void(const int16_t*, size_t)> sink)
{
    if (!_audioEnabled) return;
    _audio_codec.setMicSink(std::move(sink));
}

void Hal::audioRecord(std::vector<int16_t>& data, uint16_t durationMs, float gain)
{
    if (!_audioEnabled) return;
    _audio_codec.record(data, durationMs, gain);
}

void Hal::audioPlay(std::vector<int16_t>& data, bool async)
{
    if (!_audioEnabled) return;
    _audio_codec.play(data, async);
}

int Hal::getAudioSampleRate()
{
    return _audio_codec.sample_rate;
}

void Hal::updateAudioSpectrum()
{
    if (!_audioEnabled) return;
    _audio_codec.updateSpectrum(_audio_spectrum);
}

namespace {

extern const uint8_t _boot_sfx_start[] asm("_binary_boot_sfx_bin_start");
extern const uint8_t _boot_sfx_end[] asm("_binary_boot_sfx_bin_end");

}  // namespace

void Hal::playBootSfx()
{
    if (!_audioEnabled) return;
    mclog::tagInfo(_tag, "play boot sfx");

    const std::size_t byte_count = _boot_sfx_end - _boot_sfx_start;
    if (byte_count == 0 || (byte_count % sizeof(int16_t)) != 0) {
        mclog::tagError(_tag, "boot sfx binary has invalid size: {}", byte_count);
        return;
    }

    const auto* samples     = reinterpret_cast<const int16_t*>(_boot_sfx_start);
    const std::size_t count = byte_count / sizeof(int16_t);
    std::vector<int16_t> pcm(samples, samples + count);

    audioPlay(pcm, true);
}
