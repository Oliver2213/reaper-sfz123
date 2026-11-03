// Polyphonic, multi-channel SF2/SF3 synthesizer: the voice management, MIDI handling and effects
// around sfl2, which renders individual voices.

use std::f32::consts::PI;
use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

use sfl2::hi::{EnvPhase, HostData, RenderContext, SoundFont, Voice, Zone};
use sfl2::lo::{Gen, ResampleMethod};

use crate::fx::{Chorus, Reverb};

pub const MAX_VOICES: usize = 256;
const BLOCK: usize = 64;
const DRUM_BANK: u16 = 128;
const DRUM_CHANNEL: usize = 9;

/// One preset of a loaded font, in the order presented to the user (sorted by bank, then program).
pub struct PresetEntry {
	pub sf_index: usize,
	pub bank: u16,
	pub program: u16,
	pub name: String,
	/// lowest and highest key that produces sound
	pub key_lo: u8,
	pub key_hi: u8,
}

pub struct Font {
	pub sf: Arc<SoundFont>,
	pub presets: Arc<Vec<PresetEntry>>,
	pub name: String,
	pub sample_count: usize,
	pub instrument_count: usize,
	/// presets left out because no key can make a sound
	pub silent_presets: usize,
	/// no preset passed the check, so all are listed (the check may be wrong for this file)
	pub all_silent: bool,
}

/// Peak below which a sample counts as silence (about -78 dBFS).
const SILENCE_THRESHOLD: i16 = 4;

fn key_range(zone: &Zone) -> (u8, u8) {
	match zone.get_key_range_gen() {
		Some(g) => {
			let r = g.amount.ranges();
			(r.lo.min(127), r.hi.min(127))
		}
		None => (0, 127),
	}
}

fn range_mask(lo: u8, hi: u8) -> u128 {
	if lo > hi {
		return 0
	}
	let width = (hi - lo) as u32 + 1;
	let bits = if width >= 128 { u128::MAX } else { (1u128 << width) - 1 };
	bits << lo
}

/// Key range of a list's global zone (the first zone, when it lacks the terminal generator).
fn global_range(zones: &[Arc<Zone>], is_global: impl Fn(&Zone) -> bool) -> (u8, u8) {
	match zones.first() {
		Some(z) if is_global(z) => key_range(z),
		_ => (0, 127),
	}
}

/// Which keys of a preset reach a real, non-silent sample.
fn audible_keys(sf: &SoundFont, preset: &sfl2::hi::Preset, audible_sample: &mut Vec<Option<bool>>) -> u128 {
	let mut mask = 0u128;
	let (pglo, pghi) = global_range(&preset.zones, |z| z.get_instrument_gen().is_none());

	for pzone in &preset.zones {
		let Some(inst_gen) = pzone.get_instrument_gen() else { continue };
		let Some(inst) = sf.instruments.get(inst_gen.amount.w_amount() as usize) else { continue };
		let (plo, phi) = key_range(pzone);
		let (iglo, ighi) = global_range(&inst.zones, |z| z.get_sample_id_gen().is_none());

		for izone in &inst.zones {
			let Some(sample_gen) = izone.get_sample_id_gen() else { continue };
			let id = sample_gen.amount.w_amount() as usize;
			let Some(header) = sf.sample_headers.get(id) else { continue };

			let audible = *audible_sample.get_mut(id).unwrap().get_or_insert_with(|| {
				if header.is_rom() || header.end <= header.start {
					return false
				}
				let (start, end) = (header.start as usize, (header.end as usize).min(sf.sample_data.len()));
				start < end && sf.sample_data[start..end].iter().any(|s| s.unsigned_abs() > SILENCE_THRESHOLD as u16)
			});
			if !audible {
				continue
			}

			let (ilo, ihi) = key_range(izone);
			mask |= range_mask(plo.max(ilo).max(pglo).max(iglo), phi.min(ihi).min(pghi).min(ighi));
		}
	}

	mask
}

impl Font {
	pub fn new(sf: SoundFont) -> Self {
		let mut audible_sample = vec![None; sf.sample_headers.len()];
		let total = sf.presets.len();

		let entry = |i: usize, p: &sfl2::hi::Preset, keys: u128| PresetEntry {
			sf_index: i,
			bank: p.bank,
			program: p.preset,
			name: p.name.trim().to_string(),
			key_lo: keys.trailing_zeros() as u8,
			key_hi: 127 - keys.leading_zeros() as u8,
		};

		let keys: Vec<u128> = sf.presets.iter().map(|p| audible_keys(&sf, p, &mut audible_sample)).collect();
		let all_silent = total > 0 && keys.iter().all(|&k| k == 0);

		let mut presets: Vec<PresetEntry> = sf
			.presets
			.iter()
			.enumerate()
			.filter(|&(i, _)| all_silent || keys[i] != 0)
			.map(|(i, p)| entry(i, p, if keys[i] == 0 { u128::MAX } else { keys[i] }))
			.collect();

		let silent_presets = total - presets.len();

		presets.sort_by_key(|p| (p.bank, p.program, p.sf_index));
		presets.dedup_by_key(|p| (p.bank, p.program));

		let name = sf.name.clone().unwrap_or_default().trim().to_string();
		let sample_count = sf.sample_headers.len();
		let instrument_count = sf.instruments.len();

		Font { sf: Arc::new(sf), presets: Arc::new(presets), name, sample_count, instrument_count, silent_presets, all_silent }
	}

	fn find(&self, bank: u16, program: u8) -> Option<usize> {
		self.presets.iter().position(|p| p.bank == bank && p.program == program as u16)
	}

	/// Finds the best preset for a bank/program pair, falling back the way GM players do.
	fn resolve(&self, bank: u16, program: u8) -> Option<usize> {
		if let Some(i) = self.find(bank, program) {
			return Some(i)
		}

		if bank >= DRUM_BANK {
			self.find(DRUM_BANK, 0)
				.or_else(|| self.presets.iter().position(|p| p.bank >= DRUM_BANK))
		} else {
			self.find(0, program)
				.or_else(|| self.presets.iter().position(|p| p.program == program as u16 && p.bank < DRUM_BANK))
				.or_else(|| self.presets.iter().position(|p| p.bank < DRUM_BANK))
		}
		.or(if self.presets.is_empty() { None } else { Some(0) })
	}
}

/// Settings pushed by the plugin before each render call. Negative values mean "use the instrument's own value".
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sf2Settings {
	pub polyphony: u32,
	pub interpolation: u32,
	pub multi_timbral: u32,
	pub follow_program_change: u32,
	pub mono: u32,
	pub filter_enabled: u32,
	pub selected_preset: i32,
	pub bend_range: f32,
	pub tuning_semitones: f32,
	pub glide_ms: f32,
	pub attack_ms: f32,
	pub decay_ms: f32,
	pub sustain_pct: f32,
	pub release_ms: f32,
	pub filter_pct: f32,
	pub vib_depth_cents: f32,
	pub vib_rate_hz: f32,
	pub vib_delay_s: f32,
	pub reverb_level: f32,
	pub reverb_room: f32,
	pub reverb_damp: f32,
	pub reverb_width: f32,
	pub chorus_level: f32,
	pub chorus_rate_hz: f32,
	pub chorus_depth_ms: f32,
}

impl Default for Sf2Settings {
	fn default() -> Self {
		Sf2Settings {
			polyphony: 128,
			interpolation: 1,
			multi_timbral: 0,
			follow_program_change: 1,
			mono: 0,
			filter_enabled: 1,
			selected_preset: 0,
			bend_range: 2.0,
			tuning_semitones: 0.0,
			glide_ms: 50.0,
			attack_ms: -1.0,
			decay_ms: -1.0,
			sustain_pct: -1.0,
			release_ms: -1.0,
			filter_pct: -1.0,
			vib_depth_cents: -1.0,
			vib_rate_hz: -1.0,
			vib_delay_s: -1.0,
			reverb_level: 0.5,
			reverb_room: 0.5,
			reverb_damp: 0.3,
			reverb_width: 1.0,
			chorus_level: 0.5,
			chorus_rate_hz: 0.3,
			chorus_depth_ms: 8.0,
		}
	}
}

struct ChannelControls {
	cc: [AtomicU8; 128],
	pressure: AtomicU8,
	poly: [AtomicU8; 128],
}

impl ChannelControls {
	fn new() -> Self {
		let c = ChannelControls {
			cc: std::array::from_fn(|_| AtomicU8::new(0)),
			pressure: AtomicU8::new(0),
			poly: std::array::from_fn(|_| AtomicU8::new(0)),
		};
		c.reset(true);
		c
	}

	/// Resets controllers; `all` also restores volume and pan (used at start-up).
	fn reset(&self, all: bool) {
		for (i, v) in self.cc.iter().enumerate() {
			let keep = !all && matches!(i, 0 | 7 | 8 | 10 | 32 | 91 | 93);
			if keep {
				continue
			}
			let def = match i {
				7 => 100,
				11 => 127,
				8 | 10 => 64,
				_ => 0,
			};
			v.store(def, Ordering::Relaxed);
		}
		self.pressure.store(0, Ordering::Relaxed);
		for p in &self.poly {
			p.store(0, Ordering::Relaxed);
		}
	}
}

#[derive(Clone, Copy)]
struct LegatoState {
	last_pitch: f32,
	target_pitch: f32,
	last_vel: f32,
	target_vel: f32,
	t: f32,
}

#[derive(Clone)]
pub struct VoiceData {
	id: u64,
	channel: u8,
	key: u8,
	/// key is physically held down
	held: bool,
	/// latched by the sostenuto pedal
	sostenuto: bool,
	controls: Arc<ChannelControls>,
	legato: Option<LegatoState>,
}

impl HostData for VoiceData {
	fn midi_cc_value(&self, cc: u8) -> u8 {
		self.controls.cc[cc as usize & 127].load(Ordering::Relaxed)
	}

	fn channel_pressure(&self) -> u8 {
		self.controls.pressure.load(Ordering::Relaxed)
	}

	fn poly_pressure(&self) -> u8 {
		self.controls.poly[self.key as usize & 127].load(Ordering::Relaxed)
	}
}

type SynthVoice = Voice<VoiceData>;

struct Channel {
	controls: Arc<ChannelControls>,
	bank_msb: u8,
	bank_lsb: u8,
	/// index into Font::presets used in multi-timbral mode
	preset: Option<usize>,
	bend: f32,
	bend_range: f32,
	fine_tune: f32,
	coarse_tune: f32,
	sustain: bool,
	sostenuto: bool,
	rpn: u16,
	nrpn_selected: bool,
}

const RPN_NULL: u16 = 0x3FFF;

impl Channel {
	fn new(bend_range: f32) -> Self {
		Channel {
			controls: Arc::new(ChannelControls::new()),
			bank_msb: 0,
			bank_lsb: 0,
			preset: None,
			bend: 0.0,
			bend_range,
			fine_tune: 0.0,
			coarse_tune: 0.0,
			sustain: false,
			sostenuto: false,
			rpn: RPN_NULL,
			nrpn_selected: false,
		}
	}

	fn bank(&self, channel: usize) -> u16 {
		if channel == DRUM_CHANNEL || self.bank_msb == 120 || self.bank_msb == 127 {
			DRUM_BANK
		} else if self.bank_msb == 0 && self.bank_lsb != 0 {
			self.bank_lsb as u16
		} else {
			self.bank_msb as u16
		}
	}
}

pub struct Synth {
	font: Arc<Font>,
	ctx: RenderContext,
	voices: Vec<Option<SynthVoice>>,
	channels: Vec<Channel>,
	sample_rate: f32,
	settings: Sf2Settings,
	applied_preset: i32,
	applied_bend_range: f32,
	applied_multi: u32,
	selected: Option<usize>,
	next_id: u64,
	reverb: Reverb,
	chorus: Chorus,
	env_buf: Vec<f32>,
	iv_buf: Vec<f32>,
	rev_send: [Vec<f32>; 2],
	cho_send: [Vec<f32>; 2],
	active_voices: u32,
	pub dead: bool,
}

fn pan_to_amp(pan: f32) -> [f32; 2] {
	let p = (pan.clamp(-1.0, 1.0) + 1.0) * 0.25 * PI;
	[p.cos(), p.sin()]
}

fn move_towards(cur: f32, target: f32, delta: f32) -> f32 {
	if (target - cur).abs() <= delta { target } else { cur + delta * (target - cur).signum() }
}

fn lerp(a: f32, b: f32, t: f32) -> f32 {
	a + (b - a) * t
}

impl Synth {
	pub fn new(font: Arc<Font>, sample_rate: f32) -> Self {
		let settings = Sf2Settings::default();
		let mut ctx = RenderContext::with_poly(MAX_VOICES);
		// enable every engine fix SFLT ships ("newest" behaviour)
		ctx.settings.tweaks = u64::MAX;

		let mut s = Synth {
			font,
			ctx,
			voices: (0..MAX_VOICES).map(|_| None).collect(),
			channels: (0..16).map(|_| Channel::new(settings.bend_range)).collect(),
			sample_rate,
			settings,
			applied_preset: -2,
			applied_bend_range: settings.bend_range,
			applied_multi: 0,
			selected: None,
			next_id: 0,
			reverb: Reverb::new(sample_rate),
			chorus: Chorus::new(sample_rate),
			env_buf: vec![0.0; BLOCK],
			iv_buf: vec![0.0; BLOCK],
			rev_send: [vec![0.0; BLOCK], vec![0.0; BLOCK]],
			cho_send: [vec![0.0; BLOCK], vec![0.0; BLOCK]],
			active_voices: 0,
			dead: false,
		};
		s.apply_settings(&settings);
		s
	}

	pub fn font(&self) -> &Arc<Font> {
		&self.font
	}

	pub fn set_sample_rate(&mut self, sr: f32) {
		self.sample_rate = sr;
		self.reverb.set_sample_rate(sr);
		self.chorus.set_sample_rate(sr);
	}

	pub fn active_voices(&self) -> u32 {
		self.active_voices
	}

	/// Index (into the sorted preset list) of the preset used in single-preset mode.
	pub fn selected_preset(&self) -> i32 {
		self.selected.map(|i| i as i32).unwrap_or(-1)
	}

	pub fn channel_preset(&self, channel: u8) -> i32 {
		let ch = &self.channels[channel as usize & 15];
		if self.settings.multi_timbral != 0 {
			ch.preset.map(|i| i as i32).unwrap_or(-1)
		} else {
			self.selected_preset()
		}
	}

	pub fn apply_settings(&mut self, s: &Sf2Settings) {
		let mode_changed = s.multi_timbral != self.applied_multi;

		if s.selected_preset != self.applied_preset || mode_changed {
			self.applied_preset = s.selected_preset;
			self.applied_multi = s.multi_timbral;

			let n = self.font.presets.len();
			self.selected = if n == 0 {
				None
			} else if s.selected_preset < 0 {
				Some(0)
			} else {
				Some((s.selected_preset as usize).min(n - 1))
			};

			// in multi-timbral mode the chosen preset goes to every melodic channel
			if s.multi_timbral != 0 {
				for (i, ch) in self.channels.iter_mut().enumerate() {
					if i == DRUM_CHANNEL {
						if ch.preset.is_none() || mode_changed {
							ch.preset = self.font.resolve(DRUM_BANK, 0);
						}
					} else {
						ch.preset = self.selected;
						if let Some(p) = self.selected {
							ch.bank_msb = self.font.presets[p].bank.min(127) as u8;
							ch.bank_lsb = 0;
						}
					}
				}
			}
		}

		if s.bend_range != self.applied_bend_range {
			self.applied_bend_range = s.bend_range;
			for ch in &mut self.channels {
				ch.bend_range = s.bend_range;
			}
		}

		if s.polyphony < self.settings.polyphony {
			// choke voices above the new limit
			for v in self.voices[(s.polyphony as usize).min(MAX_VOICES)..].iter_mut() {
				if let Some(voice) = v {
					voice.release_resources(&mut self.ctx);
					*v = None;
				}
			}
		}

		let o = &mut self.ctx.overrides;
		o.attack = if s.attack_ms < 0.0 { -1 } else { s.attack_ms.round() as i32 };
		// sfl2 scales decay/release overrides by 4 (FL Studio units)
		o.decay = if s.decay_ms < 0.0 { -1 } else { (s.decay_ms / 4.0).round().max(0.0) as i32 };
		o.release = if s.release_ms < 0.0 { -1 } else { (s.release_ms / 4.0).round().max(0.0) as i32 };
		o.sustain = if s.sustain_pct < 0.0 { -1 } else { (s.sustain_pct.min(100.0) * 1.27).round() as i32 };
		o.filter_fc = if s.filter_pct < 0.0 { -1 } else { (s.filter_pct.min(100.0) * 1.27).round() as i32 };
		o.filter_bypass = s.filter_enabled == 0;
		o.lfo_2_amt = if s.vib_depth_cents < 0.0 { -1 } else { 129 + (s.vib_depth_cents / 100.0 * 12.7).round() as i32 };
		o.lfo_2_spd = if s.vib_rate_hz <= 0.0 { -1 } else { (s.vib_rate_hz * 12.0).round().max(1.0) as i32 };
		o.lfo_2_del = if s.vib_delay_s < 0.0 { -1 } else { (s.vib_delay_s / 0.004).round() as i32 };
		o.modulation = -1;

		self.reverb.room = s.reverb_room;
		self.reverb.damp = s.reverb_damp;
		self.reverb.width = s.reverb_width;
		self.chorus.rate_hz = s.chorus_rate_hz;
		self.chorus.depth_ms = s.chorus_depth_ms;

		self.settings = *s;
	}

	fn poly(&self) -> usize {
		(self.settings.polyphony as usize).clamp(1, MAX_VOICES)
	}

	fn preset_for_channel(&self, channel: u8) -> Option<usize> {
		if self.settings.multi_timbral != 0 {
			self.channels[channel as usize].preset
		} else {
			self.selected
		}
	}

	// --- MIDI

	pub fn note_on(&mut self, channel: u8, key: u8, vel: u8) {
		let channel = channel & 15;
		let key = key & 127;

		if vel == 0 {
			self.note_off(channel, key);
			return
		}

		// a key re-struck while its previous voice is held only by a pedal releases that voice
		for v in self.voices.iter_mut().flatten() {
			if v.host_data.channel == channel && v.host_data.key == key && !v.released && !v.host_data.held {
				Self::release_voice(v, &mut self.ctx);
			}
		}

		let data = VoiceData {
			id: self.next_id,
			channel,
			key,
			held: true,
			sostenuto: false,
			controls: self.channels[channel as usize].controls.clone(),
			legato: None,
		};
		self.next_id = self.next_id.wrapping_add(1);

		let voice = match self.preset_for_channel(channel) {
			Some(p) => {
				let sf_index = self.font.presets[p].sf_index;
				self.font.sf.spawn_voice(data, sf_index, key as f32, vel.min(127), &mut self.ctx)
			}
			None => SoundFont::spawn_dummy_voice(data),
		};

		let poly = self.poly();
		let voices = &mut self.voices[0..poly];

		// exclusive classes (e.g. open/closed hi-hat) cut each other off
		for v in voices.iter_mut().flatten() {
			if v.host_data.channel != channel {
				continue
			}

			for iv in voice.inst_voices.as_ref().unwrap() {
				let class = iv.gen_values[Gen::ExclusiveClass].w_amount();

				if class == 0 {
					continue
				}

				for other in v.inst_voices.as_mut().unwrap() {
					if other.gen_values[Gen::ExclusiveClass].w_amount() == class {
						other.vol_env.phase = EnvPhase::Stop;
					}
				}
			}
		}

		let slot = match voices.iter().position(|v| v.is_none()) {
			Some(i) => i,
			None => {
				// steal: released voices first, then non-legato voices, then the oldest
				let pick = |f: &dyn Fn(&SynthVoice) -> bool| {
					voices
						.iter()
						.enumerate()
						.filter(|(_, v)| f(v.as_ref().unwrap()))
						.min_by_key(|(_, v)| v.as_ref().unwrap().host_data.id)
						.map(|(i, _)| i)
				};

				let i = pick(&|v| v.released)
					.or_else(|| pick(&|v| v.host_data.legato.is_none()))
					.or_else(|| pick(&|_| true))
					.unwrap();

				voices[i].as_mut().unwrap().release_resources(&mut self.ctx);
				i
			}
		};

		voices[slot] = Some(voice);
		self.update_legato();
	}

	fn release_voice(v: &mut SynthVoice, ctx: &mut RenderContext) {
		if v.released {
			return
		}

		// the sounding legato voice is released by update_legato once no key is held
		if v.host_data.legato.is_none() {
			for iv in v.inst_voices.as_mut().unwrap() {
				iv.release(ctx);
			}
			v.released = true;
		}
	}

	pub fn note_off(&mut self, channel: u8, key: u8) {
		let channel = channel & 15;
		let sustain = self.channels[channel as usize].sustain;

		for v in self.voices.iter_mut().flatten() {
			if v.host_data.channel != channel || v.host_data.key != key || !v.host_data.held {
				continue
			}

			v.host_data.held = false;

			if !sustain && !v.host_data.sostenuto {
				Self::release_voice(v, &mut self.ctx);
			}
		}

		self.update_legato();
	}

	pub fn choke_note(&mut self, channel: u8, key: u8) {
		for slot in self.voices.iter_mut() {
			if let Some(v) = slot
				&& v.host_data.channel == channel
				&& v.host_data.key == key
			{
				v.release_resources(&mut self.ctx);
				*slot = None;
			}
		}
	}

	pub fn choke_channel(&mut self, channel: u8) {
		for slot in self.voices.iter_mut() {
			if let Some(v) = slot
				&& v.host_data.channel == channel
			{
				v.release_resources(&mut self.ctx);
				*slot = None;
			}
		}
	}

	pub fn all_sound_off(&mut self) {
		for slot in self.voices.iter_mut() {
			if let Some(v) = slot {
				v.release_resources(&mut self.ctx);
				*slot = None;
			}
		}
		self.reverb.clear();
		self.chorus.clear();
	}

	pub fn all_notes_off(&mut self, channel: u8) {
		let ch = &mut self.channels[channel as usize];
		ch.sustain = false;
		ch.sostenuto = false;

		for v in self.voices.iter_mut().flatten() {
			if v.host_data.channel == channel {
				v.host_data.held = false;
				v.host_data.sostenuto = false;
				Self::release_voice(v, &mut self.ctx);
			}
		}

		self.update_legato();
	}

	fn release_unheld(&mut self, channel: u8) {
		let ch = &self.channels[channel as usize];
		let sustain = ch.sustain;

		for v in self.voices.iter_mut().flatten() {
			if v.host_data.channel == channel && !v.host_data.held && !v.host_data.sostenuto && !sustain {
				Self::release_voice(v, &mut self.ctx);
			}
		}

		self.update_legato();
	}

	pub fn pitch_bend(&mut self, channel: u8, value14: u16) {
		self.channels[channel as usize & 15].bend = (value14.min(16383) as f32 - 8192.0) / 8192.0;
	}

	pub fn channel_pressure(&mut self, channel: u8, value: u8) {
		self.channels[channel as usize & 15].controls.pressure.store(value & 127, Ordering::Relaxed);
	}

	pub fn poly_pressure(&mut self, channel: u8, key: u8, value: u8) {
		self.channels[channel as usize & 15].controls.poly[key as usize & 127].store(value & 127, Ordering::Relaxed);
	}

	/// Returns true when the selected preset changed (single-preset mode), so the host can be told.
	pub fn program_change(&mut self, channel: u8, program: u8) -> bool {
		let channel = channel & 15;
		let bank = self.channels[channel as usize].bank(channel as usize);

		if self.settings.multi_timbral != 0 {
			self.channels[channel as usize].preset = self.font.resolve(bank, program & 127);
			false
		} else if self.settings.follow_program_change != 0 {
			let bank = if bank == DRUM_BANK && channel as usize == DRUM_CHANNEL {
				// in single-preset mode channel 10 is not special
				self.channels[channel as usize].bank_msb as u16
			} else {
				bank
			};

			let new = self.font.find(bank, program & 127).or_else(|| self.font.find(0, program & 127));

			if new.is_some() && new != self.selected {
				self.selected = new;
				self.applied_preset = new.map(|i| i as i32).unwrap_or(-1);
				return true
			}
			false
		} else {
			false
		}
	}

	pub fn control_change(&mut self, channel: u8, cc: u8, value: u8) {
		let channel = channel & 15;
		let cc = cc & 127;
		let value = value & 127;
		let ch = &mut self.channels[channel as usize];

		ch.controls.cc[cc as usize].store(value, Ordering::Relaxed);

		match cc {
			0 => ch.bank_msb = value,
			32 => ch.bank_lsb = value,

			64 => {
				let down = value >= 64;
				if ch.sustain != down {
					ch.sustain = down;
					if !down {
						self.release_unheld(channel);
					}
				}
			}

			66 => {
				let down = value >= 64;
				if ch.sostenuto != down {
					ch.sostenuto = down;
					if down {
						for v in self.voices.iter_mut().flatten() {
							if v.host_data.channel == channel && v.host_data.held && !v.released {
								v.host_data.sostenuto = true;
							}
						}
					} else {
						for v in self.voices.iter_mut().flatten() {
							if v.host_data.channel == channel {
								v.host_data.sostenuto = false;
							}
						}
						self.release_unheld(channel);
					}
				}
			}

			// RPN / NRPN selection and data entry
			101 => {
				ch.rpn = (ch.rpn & 0x7F) | ((value as u16) << 7);
				ch.nrpn_selected = false;
			}
			100 => {
				ch.rpn = (ch.rpn & 0x3F80) | value as u16;
				ch.nrpn_selected = false;
			}
			98 | 99 => ch.nrpn_selected = true,
			6 => {
				if !ch.nrpn_selected {
					match ch.rpn {
						0 => ch.bend_range = value as f32 + (ch.bend_range.fract()),
						1 => {
							let raw = ((value as i32) << 7) - 8192;
							ch.fine_tune = raw as f32 / 8192.0;
						}
						2 => ch.coarse_tune = value as f32 - 64.0,
						_ => {}
					}
				}
			}
			38 => {
				if !ch.nrpn_selected && ch.rpn == 0 {
					ch.bend_range = ch.bend_range.trunc() + (value as f32 / 100.0).min(0.99);
				}
			}

			120 => self.choke_channel(channel),
			121 => {
				let ch = &mut self.channels[channel as usize];
				ch.controls.reset(false);
				ch.bend = 0.0;
				ch.rpn = RPN_NULL;
				ch.nrpn_selected = false;
				ch.sustain = false;
				ch.sostenuto = false;
				self.release_unheld(channel);
			}
			123..=127 => self.all_notes_off(channel),
			_ => {}
		}
	}

	/// Resets every channel (GM/GS reset SysEx).
	pub fn reset(&mut self) {
		self.all_sound_off();
		let bend_range = self.settings.bend_range;
		for ch in &mut self.channels {
			*ch = Channel::new(bend_range);
		}
		self.applied_preset = -2;
		let s = self.settings;
		self.apply_settings(&s);
	}

	// --- legato

	fn update_legato(&mut self) {
		if self.settings.mono == 0 {
			return
		}

		let mut oldest: [Option<usize>; 16] = [None; 16];
		let mut newest: [Option<usize>; 16] = [None; 16];

		for (i, v) in self.voices.iter().enumerate() {
			let Some(v) = v.as_ref() else { continue };
			if v.released {
				continue
			}
			let c = v.host_data.channel as usize;
			let id = v.host_data.id;

			match oldest[c] {
				Some(o) if self.voices[o].as_ref().unwrap().host_data.id <= id => {}
				_ => oldest[c] = Some(i),
			}
			match newest[c] {
				Some(n) if self.voices[n].as_ref().unwrap().host_data.id >= id => {}
				_ => newest[c] = Some(i),
			}
		}

		// glide to the newest held key; when it is released, glide back to the sounding voice's own key
		for c in 0..16 {
			let (Some(o), Some(_)) = (oldest[c], newest[c]) else { continue };

			let target = self
				.voices
				.iter()
				.enumerate()
				.filter_map(|(i, v)| v.as_ref().map(|v| (i, v)))
				.filter(|(_, v)| !v.released && v.host_data.channel as usize == c && v.host_data.held && v.host_data.legato.is_none())
				.max_by_key(|(_, v)| v.host_data.id)
				.map(|(i, _)| i);

			let (new_note, new_vel) = match target {
				Some(t) if t != o => {
					let v = self.voices[t].as_ref().unwrap();
					(v.host_data.key as f32, v.note_on_velocity)
				}
				_ => {
					let v = self.voices[o].as_ref().unwrap();
					(v.host_data.key as f32, v.note_on_velocity)
				}
			};

			let any_held = target.is_some() || self.voices[o].as_ref().unwrap().host_data.held;
			let channel_pedal = self.channels[c].sustain;

			let ov = self.voices[o].as_mut().unwrap();

			if !any_held && !channel_pedal && ov.host_data.legato.is_some() {
				// nothing held any more: release the sounding voice
				ov.host_data.legato = None;
				for iv in ov.inst_voices.as_mut().unwrap() {
					iv.release(&mut self.ctx);
				}
				ov.released = true;
				continue
			}

			ov.host_data.legato = Some(match ov.host_data.legato {
				None => LegatoState {
					last_pitch: ov.host_data.key as f32,
					target_pitch: new_note,
					last_vel: ov.note_on_velocity,
					target_vel: new_vel,
					t: 0.0,
				},
				Some(l) => LegatoState {
					last_pitch: lerp(l.last_pitch, l.target_pitch, l.t),
					target_pitch: new_note,
					last_vel: lerp(l.last_vel, l.target_vel, l.t),
					target_vel: new_vel,
					t: if l.target_pitch == new_note { l.t } else { 0.0 },
				},
			});
		}

		// voices other than the sounding legato voice are silent note-holders; drop released ones
		for slot in self.voices.iter_mut() {
			if let Some(v) = slot
				&& v.host_data.legato.is_none()
				&& !v.host_data.held
				&& !self.channels[v.host_data.channel as usize].sustain
			{
				v.release_resources(&mut self.ctx);
				*slot = None;
			}
		}
	}

	// --- render

	/// Renders `out_l.len()` frames, overwriting the output buffers.
	pub fn render(&mut self, out_l: &mut [f32], out_r: &mut [f32]) {
		out_l.fill(0.0);
		out_r.fill(0.0);

		if self.dead {
			return
		}

		let mut start = 0;
		while start < out_l.len() {
			let end = (start + BLOCK).min(out_l.len());
			self.render_block(&mut out_l[start..end], &mut out_r[start..end]);
			start = end;
		}
	}

	fn render_block(&mut self, out_l: &mut [f32], out_r: &mut [f32]) {
		let n = out_l.len();
		let sr = self.sample_rate;
		let method = match self.settings.interpolation {
			0 => ResampleMethod::Linear,
			1 => ResampleMethod::Hermite,
			2 => ResampleMethod::Sinc32,
			_ => ResampleMethod::Sinc64,
		};
		let mono = self.settings.mono != 0;
		let glide = if self.settings.glide_ms <= 0.0 { 0.0 } else { self.settings.glide_ms / 1000.0 };
		let tuning = self.settings.tuning_semitones;

		for b in [&mut self.rev_send, &mut self.cho_send] {
			b[0][..n].fill(0.0);
			b[1][..n].fill(0.0);
		}

		let mut active = 0;

		for slot in 0..MAX_VOICES {
			let Some(v) = self.voices[slot].as_mut() else { continue };
			let ch = &self.channels[v.host_data.channel as usize];

			let rendered = Self::render_voice(
				v,
				&mut self.ctx,
				&mut self.env_buf[..n],
				&mut self.iv_buf[..n],
				out_l,
				out_r,
				&mut self.rev_send,
				&mut self.cho_send,
				ch.bend * ch.bend_range + ch.fine_tune + ch.coarse_tune + tuning,
				sr,
				method,
				mono,
				glide,
			);

			if v.has_active_voices {
				active += 1;
			}

			// free finished voices (sample ended or release finished)
			if (v.released || rendered) && !v.has_active_voices {
				v.release_resources(&mut self.ctx);
				self.voices[slot] = None;
			}
		}

		self.active_voices = active;

		let s = self.settings;
		if s.reverb_level > 0.0 {
			self.reverb.process_add([&self.rev_send[0][..n], &self.rev_send[1][..n]], [&mut *out_l, &mut *out_r], s.reverb_level);
		}
		if s.chorus_level > 0.0 {
			self.chorus.process_add([&self.cho_send[0][..n], &self.cho_send[1][..n]], [&mut *out_l, &mut *out_r], s.chorus_level);
		}
	}

	/// Returns true if the voice was rendered this block (mono mode skips note-holder voices).
	#[allow(clippy::too_many_arguments)]
	fn render_voice(
		v: &mut SynthVoice,
		ctx: &mut RenderContext,
		env_buf: &mut [f32],
		iv_buf: &mut [f32],
		out_l: &mut [f32],
		out_r: &mut [f32],
		rev: &mut [Vec<f32>; 2],
		cho: &mut [Vec<f32>; 2],
		pitch_offset: f32,
		sample_rate: f32,
		method: ResampleMethod,
		mono: bool,
		glide: f32,
	) -> bool {
		v.has_active_voices = false;
		v.pitch_offset = pitch_offset;

		if mono {
			let Some(legato) = v.host_data.legato.as_mut() else {
				// keep note-holder voices alive without rendering them
				v.has_active_voices = !v.released;
				return false
			};

			let pitch = lerp(legato.last_pitch, legato.target_pitch, legato.t);
			let elapsed = env_buf.len() as f32 / sample_rate;

			v.pitch_offset += pitch - v.init_pitch;
			v.vel_offset = lerp(legato.last_vel, legato.target_vel, legato.t) - v.note_on_velocity;

			legato.t = if glide == 0.0 { 1.0 } else { (legato.t + elapsed / glide).clamp(0.0, 1.0) };
		}

		let ramp = 100.0 / sample_rate;
		let mut inst_voices = v.inst_voices.take().unwrap();

		for iv in &mut inst_voices {
			let pan = (iv.gen_values[Gen::Pan].sh_amount() as f32 / 500.0).clamp(-1.0, 1.0);
			let target = pan_to_amp(pan);

			if iv.vol_env.phase != EnvPhase::Stop {
				v.has_active_voices = true;
			}

			if v.first_rendering {
				iv.last_vol = (target[0], target[1]);
			}

			iv_buf.fill(0.0);
			env_buf.fill(0.0);

			iv.update(v, ctx);
			iv.render_vol_envelope(env_buf, sample_rate as u32, ctx);

			if iv.render(v, iv_buf, env_buf, sample_rate as u32, method, ctx) {
				let rev_amt = (iv.gen_values[Gen::ReverbEffectsSend].sh_amount() as f32 / 1000.0).clamp(0.0, 1.0);
				let cho_amt = (iv.gen_values[Gen::ChorusEffectsSend].sh_amount() as f32 / 1000.0).clamp(0.0, 1.0);

				for (i, s) in iv_buf.iter().enumerate() {
					let l = s * iv.last_vol.0;
					let r = s * iv.last_vol.1;

					out_l[i] += l;
					out_r[i] += r;
					rev[0][i] += l * rev_amt;
					rev[1][i] += r * rev_amt;
					cho[0][i] += l * cho_amt;
					cho[1][i] += r * cho_amt;

					iv.last_vol.0 = move_towards(iv.last_vol.0, target[0], ramp);
					iv.last_vol.1 = move_towards(iv.last_vol.1, target[1], ramp);
				}
			}

			iv.render_mod_envelope(env_buf.len(), sample_rate as u32, ctx);
		}

		v.first_rendering = false;
		v.inst_voices = Some(inst_voices);
		true
	}
}
