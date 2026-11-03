// C interface to the Rust SF2/SF3 engine (engine-sf2/src/lib.rs).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Sf2Progress Sf2Progress;
typedef struct Sf2Font Sf2Font;
typedef struct Sf2Synth Sf2Synth;

// Must match `Sf2Settings` in engine-sf2/src/synth.rs. Negative values mean "use the instrument's value".
typedef struct Sf2Settings {
    uint32_t polyphony;
    uint32_t interpolation; // 0 linear, 1 hermite, 2 sinc32, 3 sinc64
    uint32_t multi_timbral;
    uint32_t follow_program_change;
    uint32_t mono;
    uint32_t filter_enabled;
    int32_t selected_preset;
    float bend_range;
    float tuning_semitones;
    float glide_ms;
    float attack_ms;
    float decay_ms;
    float sustain_pct;
    float release_ms;
    float filter_pct;
    float vib_depth_cents;
    float vib_rate_hz;
    float vib_delay_s;
    float reverb_level;
    float reverb_room;
    float reverb_damp;
    float reverb_width;
    float chorus_level;
    float chorus_rate_hz;
    float chorus_depth_ms;
} Sf2Settings;

Sf2Progress* sf2_progress_new(void);
void sf2_progress_free(Sf2Progress* p);
uint32_t sf2_progress_get(const Sf2Progress* p, uint32_t* phase);

Sf2Font* sf2_font_load(const char* path, const Sf2Progress* progress, char* err, size_t err_len);
void sf2_font_free(Sf2Font* f);
void sf2_font_name(const Sf2Font* f, char* buf, size_t len);
void sf2_font_counts(const Sf2Font* f, uint32_t* samples, uint32_t* instruments);
uint32_t sf2_font_preset_count(const Sf2Font* f);
bool sf2_font_preset(const Sf2Font* f, uint32_t index, uint16_t* bank, uint16_t* program, char* name, size_t name_len);
bool sf2_font_preset_keys(const Sf2Font* f, uint32_t index, uint8_t* lo, uint8_t* hi);
uint32_t sf2_font_silent_presets(const Sf2Font* f);
bool sf2_font_all_silent(const Sf2Font* f);

Sf2Synth* sf2_synth_new(const Sf2Font* f, float sample_rate);
void sf2_synth_free(Sf2Synth* s);
void sf2_synth_set_sample_rate(Sf2Synth* s, float sample_rate);
void sf2_synth_apply_settings(Sf2Synth* s, const Sf2Settings* settings);
void sf2_synth_note_on(Sf2Synth* s, uint8_t channel, uint8_t key, uint8_t velocity);
void sf2_synth_note_off(Sf2Synth* s, uint8_t channel, uint8_t key);
void sf2_synth_choke(Sf2Synth* s, uint8_t channel, uint8_t key);
void sf2_synth_control_change(Sf2Synth* s, uint8_t channel, uint8_t cc, uint8_t value);
bool sf2_synth_program_change(Sf2Synth* s, uint8_t channel, uint8_t program);
void sf2_synth_pitch_bend(Sf2Synth* s, uint8_t channel, uint16_t value14);
void sf2_synth_channel_pressure(Sf2Synth* s, uint8_t channel, uint8_t value);
void sf2_synth_poly_pressure(Sf2Synth* s, uint8_t channel, uint8_t key, uint8_t value);
void sf2_synth_all_sound_off(Sf2Synth* s);
void sf2_synth_reset(Sf2Synth* s);
void sf2_synth_render(Sf2Synth* s, float* left, float* right, uint32_t frames);
uint32_t sf2_synth_active_voices(Sf2Synth* s);
int32_t sf2_synth_selected_preset(Sf2Synth* s);
int32_t sf2_synth_channel_preset(Sf2Synth* s, uint8_t channel);
bool sf2_synth_is_dead(const Sf2Synth* s);

#ifdef __cplusplus
}
#endif
