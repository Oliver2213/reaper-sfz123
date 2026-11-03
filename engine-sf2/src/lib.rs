// C ABI for the SF2/SF3 engine (declared in include/sf2engine.h). Calls that touch SoundFont data
// catch panics, so a malformed file can at worst silence the synth rather than crash the host.

mod fx;
pub mod synth;

use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

use sfl2::hi::SoundFont;
use sfl2::lo::Sf2Data;
pub use synth::{Font, Sf2Settings, Synth};

pub struct Progress(Arc<AtomicU32>);
pub struct FontHandle(Arc<Font>);

fn write_str(s: &str, buf: *mut c_char, len: usize) {
	if buf.is_null() || len == 0 {
		return
	}
	let bytes = s.as_bytes();
	let mut n = bytes.len().min(len - 1);
	// don't cut a UTF-8 sequence in half
	while n > 0 && !s.is_char_boundary(n) {
		n -= 1;
	}
	unsafe {
		std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
		*buf.add(n) = 0;
	}
}

/// Loads a font from disk (blocking). Public for tests and tools.
pub fn load_font(path: PathBuf, progress: Arc<AtomicU32>) -> Result<Font, String> {
	let data = Sf2Data::load_from_file(path, true, progress).map_err(|e| match e.kind() {
		std::io::ErrorKind::UnexpectedEof => "the file is incomplete or is not a SoundFont".to_string(),
		std::io::ErrorKind::OutOfMemory => "there is not enough memory to load it".to_string(),
		std::io::ErrorKind::PermissionDenied => "permission to read the file was denied".to_string(),
		_ => {
			let msg = e.to_string();
			if msg.contains("smpl") || msg.contains("RIFF") || msg.contains("chunk") {
				format!("the file is damaged or is not a SoundFont ({msg})")
			} else {
				msg
			}
		}
	})?;

	if data.phdr.len() < 2 || data.inst.len() < 2 {
		return Err("the file contains no presets".into())
	}

	let result = catch_unwind(AssertUnwindSafe(|| Font::new(SoundFont::from_sf2_data(data))));
	result.map_err(|_| "the file is damaged or uses an unsupported layout".to_string())
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_progress_new() -> *mut Progress {
	Box::into_raw(Box::new(Progress(Arc::new(AtomicU32::new(0)))))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_progress_free(p: *mut Progress) {
	if !p.is_null() {
		drop(unsafe { Box::from_raw(p) });
	}
}

/// Returns load progress in tenths of a percent (0-1000) and writes the phase:
/// 0 = reading samples, 1 = reading 24-bit data, 2 = decoding compressed (SF3) samples.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_progress_get(p: *const Progress, phase: *mut u32) -> u32 {
	if p.is_null() {
		return 0
	}
	let v = unsafe { &*p }.0.load(Ordering::Relaxed);
	if !phase.is_null() {
		unsafe {
			*phase = if v & sfl2::OGG_FLAG != 0 { 2 } else if v & sfl2::SM24_FLAG != 0 { 1 } else { 0 };
		}
	}
	(v & !(sfl2::OGG_FLAG | sfl2::SM24_FLAG)).min(1000)
}

/// Loads an SF2 or SF3 file. On failure returns null and writes a message into `err`.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_load(path: *const c_char, progress: *const Progress, err: *mut c_char, err_len: usize) -> *mut FontHandle {
	if path.is_null() {
		write_str("no path", err, err_len);
		return std::ptr::null_mut()
	}

	let path = PathBuf::from(unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned());
	let progress = if progress.is_null() { Arc::new(AtomicU32::new(0)) } else { unsafe { &*progress }.0.clone() };

	match catch_unwind(AssertUnwindSafe(|| load_font(path, progress))) {
		Ok(Ok(font)) => Box::into_raw(Box::new(FontHandle(Arc::new(font)))),
		Ok(Err(e)) => {
			write_str(&e, err, err_len);
			std::ptr::null_mut()
		}
		Err(_) => {
			write_str("the file is damaged or uses an unsupported layout", err, err_len);
			std::ptr::null_mut()
		}
	}
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_free(f: *mut FontHandle) {
	if !f.is_null() {
		drop(unsafe { Box::from_raw(f) });
	}
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_name(f: *const FontHandle, buf: *mut c_char, len: usize) {
	if f.is_null() {
		return write_str("", buf, len)
	}
	write_str(&unsafe { &*f }.0.name, buf, len);
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_counts(f: *const FontHandle, samples: *mut u32, instruments: *mut u32) {
	if f.is_null() {
		return
	}
	let font = &unsafe { &*f }.0;
	unsafe {
		if !samples.is_null() {
			*samples = font.sample_count.saturating_sub(1) as u32;
		}
		if !instruments.is_null() {
			*instruments = font.instrument_count as u32;
		}
	}
}

/// Lowest and highest key of a preset that produce sound.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_preset_keys(f: *const FontHandle, index: u32, lo: *mut u8, hi: *mut u8) -> bool {
	if f.is_null() || lo.is_null() || hi.is_null() {
		return false
	}
	let Some(p) = unsafe { &*f }.0.presets.get(index as usize) else { return false };
	unsafe {
		*lo = p.key_lo;
		*hi = p.key_hi;
	}
	true
}

/// True when no preset passed the audibility check, so none were hidden.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_all_silent(f: *const FontHandle) -> bool {
	!f.is_null() && unsafe { &*f }.0.all_silent
}

/// Number of presets hidden because they cannot make a sound.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_silent_presets(f: *const FontHandle) -> u32 {
	if f.is_null() {
		return 0
	}
	unsafe { &*f }.0.silent_presets as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_preset_count(f: *const FontHandle) -> u32 {
	if f.is_null() {
		return 0
	}
	unsafe { &*f }.0.presets.len() as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_font_preset(f: *const FontHandle, index: u32, bank: *mut u16, program: *mut u16, name: *mut c_char, name_len: usize) -> bool {
	if f.is_null() {
		return false
	}
	let Some(p) = unsafe { &*f }.0.presets.get(index as usize) else { return false };
	unsafe {
		if !bank.is_null() {
			*bank = p.bank;
		}
		if !program.is_null() {
			*program = p.program;
		}
	}
	write_str(&p.name, name, name_len);
	true
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_new(f: *const FontHandle, sample_rate: f32) -> *mut Synth {
	if f.is_null() {
		return std::ptr::null_mut()
	}
	let font = unsafe { &*f }.0.clone();
	Box::into_raw(Box::new(Synth::new(font, sample_rate)))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_free(s: *mut Synth) {
	if !s.is_null() {
		drop(unsafe { Box::from_raw(s) });
	}
}

/// Runs `f` on the synth, marking it dead (silent) if it panics.
fn guarded<R: Default>(s: *mut Synth, f: impl FnOnce(&mut Synth) -> R) -> R {
	if s.is_null() {
		return R::default()
	}
	let synth = unsafe { &mut *s };
	if synth.dead {
		return R::default()
	}
	match catch_unwind(AssertUnwindSafe(|| f(&mut *synth))) {
		Ok(r) => r,
		Err(_) => {
			synth.dead = true;
			R::default()
		}
	}
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_set_sample_rate(s: *mut Synth, sample_rate: f32) {
	guarded(s, |s| s.set_sample_rate(sample_rate))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_apply_settings(s: *mut Synth, settings: *const Sf2Settings) {
	if settings.is_null() {
		return
	}
	let settings = unsafe { *settings };
	guarded(s, |s| s.apply_settings(&settings))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_note_on(s: *mut Synth, channel: u8, key: u8, velocity: u8) {
	guarded(s, |s| s.note_on(channel, key, velocity))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_note_off(s: *mut Synth, channel: u8, key: u8) {
	guarded(s, |s| s.note_off(channel, key))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_choke(s: *mut Synth, channel: u8, key: u8) {
	// a CLAP choke stops the note immediately, without a release phase
	guarded(s, |s| s.choke_note(channel & 15, key & 127))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_control_change(s: *mut Synth, channel: u8, cc: u8, value: u8) {
	guarded(s, |s| s.control_change(channel, cc, value))
}

/// Returns true if the single-mode preset changed because of this program change.
#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_program_change(s: *mut Synth, channel: u8, program: u8) -> bool {
	guarded(s, |s| s.program_change(channel, program))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_pitch_bend(s: *mut Synth, channel: u8, value14: u16) {
	guarded(s, |s| s.pitch_bend(channel, value14))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_channel_pressure(s: *mut Synth, channel: u8, value: u8) {
	guarded(s, |s| s.channel_pressure(channel, value))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_poly_pressure(s: *mut Synth, channel: u8, key: u8, value: u8) {
	guarded(s, |s| s.poly_pressure(channel, key, value))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_all_sound_off(s: *mut Synth) {
	guarded(s, |s| s.all_sound_off())
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_reset(s: *mut Synth) {
	guarded(s, |s| s.reset())
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_render(s: *mut Synth, left: *mut f32, right: *mut f32, frames: u32) {
	if left.is_null() || right.is_null() || frames == 0 {
		return
	}
	let l = unsafe { std::slice::from_raw_parts_mut(left, frames as usize) };
	let r = unsafe { std::slice::from_raw_parts_mut(right, frames as usize) };
	if s.is_null() || unsafe { &*s }.dead {
		l.fill(0.0);
		r.fill(0.0);
		return
	}
	let ok = guarded(s, |s| {
		s.render(l, r);
		true
	});
	if !ok {
		let l = unsafe { std::slice::from_raw_parts_mut(left, frames as usize) };
		let r = unsafe { std::slice::from_raw_parts_mut(right, frames as usize) };
		l.fill(0.0);
		r.fill(0.0);
	}
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_active_voices(s: *mut Synth) -> u32 {
	guarded(s, |s| s.active_voices())
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_selected_preset(s: *mut Synth) -> i32 {
	if s.is_null() {
		return -1
	}
	guarded(s, |s| s.selected_preset())
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_channel_preset(s: *mut Synth, channel: u8) -> i32 {
	if s.is_null() {
		return -1
	}
	guarded(s, |s| s.channel_preset(channel))
}

#[unsafe(no_mangle)]
pub extern "C" fn sf2_synth_is_dead(s: *const Synth) -> bool {
	s.is_null() || unsafe { &*s }.dead
}
