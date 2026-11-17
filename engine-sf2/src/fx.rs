// Reverb and stereo chorus fed by the SoundFont's per-voice send amounts. The reverb follows
// Freeverb by Jezar at Dreampoint (public domain), with its comb and allpass tunings.

use std::f32::consts::TAU;

const COMB_TUNING: [usize; 8] = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617];
const ALLPASS_TUNING: [usize; 4] = [556, 441, 341, 225];
const STEREO_SPREAD: usize = 23;
const FIXED_GAIN: f32 = 0.015;
const WET_SCALE: f32 = 3.0;

struct Comb {
	buf: Vec<f32>,
	pos: usize,
	filter_store: f32,
}

impl Comb {
	fn new(len: usize) -> Self {
		Comb { buf: vec![0.0; len.max(1)], pos: 0, filter_store: 0.0 }
	}

	#[inline]
	fn process(&mut self, input: f32, feedback: f32, damp: f32) -> f32 {
		let out = self.buf[self.pos];
		self.filter_store = out * (1.0 - damp) + self.filter_store * damp;
		self.buf[self.pos] = input + self.filter_store * feedback;
		self.pos += 1;
		if self.pos >= self.buf.len() {
			self.pos = 0;
		}
		out
	}
}

struct Allpass {
	buf: Vec<f32>,
	pos: usize,
}

impl Allpass {
	fn new(len: usize) -> Self {
		Allpass { buf: vec![0.0; len.max(1)], pos: 0 }
	}

	#[inline]
	fn process(&mut self, input: f32) -> f32 {
		let buffered = self.buf[self.pos];
		self.buf[self.pos] = input + buffered * 0.5;
		self.pos += 1;
		if self.pos >= self.buf.len() {
			self.pos = 0;
		}
		buffered - input
	}
}

pub struct Reverb {
	combs: [Vec<Comb>; 2],
	allpasses: [Vec<Allpass>; 2],
	sample_rate: f32,
	pub room: f32,
	pub damp: f32,
	pub width: f32,
}

impl Reverb {
	pub fn new(sample_rate: f32) -> Self {
		let mut r = Reverb {
			combs: [vec![], vec![]],
			allpasses: [vec![], vec![]],
			sample_rate: 0.0,
			room: 0.5,
			damp: 0.3,
			width: 1.0,
		};
		r.set_sample_rate(sample_rate);
		r
	}

	pub fn set_sample_rate(&mut self, sample_rate: f32) {
		if sample_rate == self.sample_rate {
			return
		}

		self.sample_rate = sample_rate;
		let scale = sample_rate / 44100.0;

		for ch in 0..2 {
			let spread = if ch == 1 { STEREO_SPREAD } else { 0 };
			self.combs[ch] = COMB_TUNING
				.iter()
				.map(|&t| Comb::new(((t + spread) as f32 * scale) as usize))
				.collect();
			self.allpasses[ch] = ALLPASS_TUNING
				.iter()
				.map(|&t| Allpass::new(((t + spread) as f32 * scale) as usize))
				.collect();
		}
	}

	pub fn clear(&mut self) {
		for ch in 0..2 {
			for c in &mut self.combs[ch] {
				c.buf.fill(0.0);
				c.filter_store = 0.0;
			}
			for a in &mut self.allpasses[ch] {
				a.buf.fill(0.0);
			}
		}
	}

	/// Processes the send bus `input` and adds the wet signal (scaled by `level`) into `out`.
	pub fn process_add(&mut self, input: [&[f32]; 2], out: [&mut [f32]; 2], level: f32) {
		let feedback = self.room.clamp(0.0, 1.0) * 0.28 + 0.7;
		let damp = self.damp.clamp(0.0, 1.0) * 0.4;
		let wet = level * WET_SCALE;
		let wet1 = wet * (self.width / 2.0 + 0.5);
		let wet2 = wet * ((1.0 - self.width) / 2.0);

		let [out_l, out_r] = out;

		for i in 0..out_l.len() {
			let x = (input[0][i] + input[1][i]) * FIXED_GAIN;
			let mut l = 0.0;
			let mut r = 0.0;

			for c in &mut self.combs[0] {
				l += c.process(x, feedback, damp);
			}
			for c in &mut self.combs[1] {
				r += c.process(x, feedback, damp);
			}
			for a in &mut self.allpasses[0] {
				l = a.process(l);
			}
			for a in &mut self.allpasses[1] {
				r = a.process(r);
			}

			out_l[i] += l * wet1 + r * wet2;
			out_r[i] += r * wet1 + l * wet2;
		}
	}
}

const CHORUS_VOICES: usize = 3;
const CHORUS_MAX_MS: f32 = 60.0;

pub struct Chorus {
	buf: [Vec<f32>; 2],
	pos: usize,
	phase: f32,
	sample_rate: f32,
	pub rate_hz: f32,
	pub depth_ms: f32,
}

impl Chorus {
	pub fn new(sample_rate: f32) -> Self {
		let mut c = Chorus {
			buf: [vec![], vec![]],
			pos: 0,
			phase: 0.0,
			sample_rate: 0.0,
			rate_hz: 0.3,
			depth_ms: 8.0,
		};
		c.set_sample_rate(sample_rate);
		c
	}

	pub fn set_sample_rate(&mut self, sample_rate: f32) {
		if sample_rate == self.sample_rate {
			return
		}

		self.sample_rate = sample_rate;
		let len = (CHORUS_MAX_MS * 0.001 * sample_rate) as usize + 4;
		self.buf = [vec![0.0; len], vec![0.0; len]];
		self.pos = 0;
	}

	pub fn clear(&mut self) {
		self.buf[0].fill(0.0);
		self.buf[1].fill(0.0);
	}

	#[inline]
	fn read(buf: &[f32], pos: usize, delay: f32) -> f32 {
		let len = buf.len();
		let d = delay.clamp(1.0, (len - 2) as f32);
		let di = d as usize;
		let frac = d - di as f32;
		let a = buf[(pos + len - di) % len];
		let b = buf[(pos + len - di - 1) % len];
		a + (b - a) * frac
	}

	/// Processes the send bus `input` and adds the wet signal (scaled by `level`) into `out`.
	pub fn process_add(&mut self, input: [&[f32]; 2], out: [&mut [f32]; 2], level: f32) {
		let sr = self.sample_rate;
		let depth = (self.depth_ms.clamp(0.0, 20.0) * 0.001 * sr).max(1.0);
		let base = 0.007 * sr; // 7 ms centre delay
		let inc = self.rate_hz.clamp(0.01, 10.0) / sr;
		let gain = level / CHORUS_VOICES as f32 * 1.5;

		let [out_l, out_r] = out;

		for i in 0..out_l.len() {
			self.buf[0][self.pos] = input[0][i];
			self.buf[1][self.pos] = input[1][i];

			let mut l = 0.0;
			let mut r = 0.0;

			for v in 0..CHORUS_VOICES {
				let ph = self.phase + v as f32 / CHORUS_VOICES as f32;
				let m = (ph * TAU).sin();
				// left and right taps move in opposite directions for width
				l += Self::read(&self.buf[0], self.pos, base + depth * (0.5 + 0.5 * m));
				r += Self::read(&self.buf[1], self.pos, base + depth * (0.5 - 0.5 * m));
			}

			out_l[i] += l * gain;
			out_r[i] += r * gain;

			self.pos = (self.pos + 1) % self.buf[0].len();
			self.phase += inc;
			if self.phase >= 1.0 {
				self.phase -= 1.0;
			}
		}
	}
}
