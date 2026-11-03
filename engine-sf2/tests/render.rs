// Smoke tests against real SoundFonts. Point SF_TEST_FONT at an .sf2/.sf3 file to run them.

use std::sync::Arc;
use std::sync::atomic::AtomicU32;
use std::time::Instant;

use sf2engine::{Sf2Settings, Synth, load_font};

fn font_path() -> Option<String> {
	std::env::var("SF_TEST_FONT").ok()
}

fn rms(buf: &[f32]) -> f32 {
	(buf.iter().map(|s| s * s).sum::<f32>() / buf.len().max(1) as f32).sqrt()
}

fn render(synth: &mut Synth, frames: usize) -> (Vec<f32>, Vec<f32>) {
	let mut l = vec![0.0; frames];
	let mut r = vec![0.0; frames];
	for (cl, cr) in l.chunks_mut(512).zip(r.chunks_mut(512)) {
		synth.render(cl, cr);
	}
	(l, r)
}

#[test]
fn plays_notes_and_honours_sustain() {
	let Some(path) = font_path() else {
		eprintln!("SF_TEST_FONT not set, skipping");
		return
	};

	let t = Instant::now();
	let font = load_font(path.into(), Arc::new(AtomicU32::new(0))).expect("font loads");
	eprintln!("loaded '{}' in {:?}: {} presets", font.name, t.elapsed(), font.presets.len());
	for p in font.presets.iter().take(5) {
		eprintln!("  {:03}:{:03} {}", p.bank, p.program, p.name);
	}
	assert!(!font.presets.is_empty());

	let mut synth = Synth::new(Arc::new(font), 48000.0);
	synth.apply_settings(&Sf2Settings::default());

	// silence before any note
	let (l, _) = render(&mut synth, 4800);
	assert_eq!(rms(&l), 0.0);

	synth.note_on(0, 60, 100);
	let (l, r) = render(&mut synth, 24000);
	let level = rms(&l).max(rms(&r));
	eprintln!("note level {level}, voices {}", synth.active_voices());
	assert!(level > 1e-4, "note should be audible");
	assert!(l.iter().chain(r.iter()).all(|s| s.is_finite() && s.abs() < 4.0));

	// sustain pedal keeps the note alive after note-off
	synth.control_change(0, 64, 127);
	synth.note_off(0, 60);
	render(&mut synth, 48000);
	assert!(synth.active_voices() > 0, "pedal should hold the note");

	synth.control_change(0, 64, 0);
	render(&mut synth, 48000 * 6);
	assert_eq!(synth.active_voices(), 0, "note should die after pedal release");

	// multi-timbral GM: channel 10 drums plays something
	let s = Sf2Settings { multi_timbral: 1, ..Sf2Settings::default() };
	synth.apply_settings(&s);
	synth.note_on(9, 38, 110);
	let (l, _) = render(&mut synth, 9600);
	eprintln!("drum level {}", rms(&l));
	assert!(rms(&l) > 1e-4, "drum channel should be audible");

	// program change switches channel 1 to another preset
	synth.program_change(0, 40);
	synth.note_on(0, 72, 100);
	let (l, _) = render(&mut synth, 9600);
	assert!(rms(&l) > 1e-4);

	// mono / legato mode does not lose notes
	let s = Sf2Settings { mono: 1, multi_timbral: 0, ..Sf2Settings::default() };
	synth.all_sound_off();
	synth.apply_settings(&s);
	synth.note_on(0, 60, 100);
	synth.note_on(0, 64, 100);
	synth.note_off(0, 64);
	let (l, _) = render(&mut synth, 9600);
	assert!(rms(&l) > 1e-4, "legato falls back to held note");
	synth.note_off(0, 60);
	render(&mut synth, 48000 * 6);
	assert_eq!(synth.active_voices(), 0);
}

#[test]
fn performance_many_voices() {
	let Some(path) = font_path() else { return };
	let font = load_font(path.into(), Arc::new(AtomicU32::new(0))).unwrap();
	let mut synth = Synth::new(Arc::new(font), 48000.0);
	let s = Sf2Settings { interpolation: 3, polyphony: 256, ..Sf2Settings::default() };
	synth.apply_settings(&s);
	for k in 30..94 {
		synth.note_on(0, k, 100);
	}
	let t = Instant::now();
	render(&mut synth, 48000);
	let el = t.elapsed();
	eprintln!("1s of audio with {} voices (sinc64) rendered in {:?}", synth.active_voices(), el);
}
