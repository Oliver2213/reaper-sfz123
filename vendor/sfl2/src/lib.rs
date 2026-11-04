
// Modified for sfz123 by Claude Code (Claude Opus 5.5) for Blake Oliver, 2026-11-03 (from SFLT 0.9.0); the changes are listed in sfz123's README.
// Modified for sfz123 since 2026 (from SFLT 0.9.0); the changes are listed in sfz123's README.
//
// this is a port of the original C++ implementation,
// which is why you might find it to be more "C-like"
// than an idiomatic implementation would be.

// the `lo` module is an implementation of the types defined by the SF2 spec.
// the `hi` module builds higher-level abstractions on top of these types, and implements the actual synthesis.

// this implementation intentionally leaves certain parts of the SF2 synthesis model up to the front-end, such as:
//
// - reverb & chorus processing
// - panning
// - ExclusiveClass handling
//
// these features are omitted to avoid making the synthesis engine too opinionated in terms of buffer format,
// voice storage, etc. `sfl2` only renders single InstVoices into mono buffers, and any stereo processing is
// the responsibility of the front-end.
//
// for guidance on how to implement these components, refer to `sflt-fl` and `sflt-nih`.

mod riff;

pub use riff::{SM24_FLAG, OGG_FLAG};

mod util {
	pub(crate) trait TryPush<T> {
		fn try_push(&mut self, item: T) -> Result<(), std::collections::TryReserveError>;
	}

	impl<T> TryPush<T> for Vec<T> {
		fn try_push(&mut self, item: T) -> Result<(), std::collections::TryReserveError> {
			self.try_reserve(1)?;
			self.push(item);
			Ok(())
		}
	}
}


pub mod lo {
	use std::{fmt::Debug, hash::Hash, io::{self, Cursor, ErrorKind}, ops::{Index, IndexMut}, path::PathBuf, sync::{Arc, atomic::{AtomicU32, Ordering}}};

	use lewton::inside_ogg::OggStreamReader;

	use crate::{riff::{self, OGG_FLAG}, util::TryPush};

	pub struct Sf2Data {
		pub path: PathBuf,
		pub name: Option<String>,
		pub version_major: u16,
		pub version_minor: u16,

		pub smpl: Vec<i16>,
		pub sm24: Vec<u8>,
		pub phdr: Vec<PresetHeader>,
		pub pbag: Vec<Bag>,
		pub pmod: Vec<ModList>,
		pub pgen: Vec<GenList>,
		pub inst: Vec<Inst>,
		pub ibag: Vec<Bag>,
		pub imod: Vec<ModList>,
		pub igen: Vec<GenList>,
		pub shdr: Vec<Sample>,
	}

	// SAFETY:
	// This function returns a raw, mutable byte view into any object passed into it.
	// This can be used to construct objects with invalid bit patterns, which is UB.
	// The responsibility is on the caller to modify this
	unsafe fn as_u8_slice_mut<T: ?Sized>(from: &mut T) -> &mut [u8] {
		let ptr: *mut u8 = (from as *mut T).cast();
		unsafe { std::slice::from_raw_parts_mut(ptr, size_of_val(from)) }
	}

	// SAFETY:
	// This function reads data directly from a u8 buffer into a value of type T.
	// If the data read does not represent a valid value of T, the behaviour is undefined.
	unsafe fn read_from_chunk<T: ?Sized>(i: &mut usize, chunk_data: &[u8], t: &mut T) {
		let size = size_of_val(t);

		assert!(size <= chunk_data[*i..].len());

		unsafe { as_u8_slice_mut(t) }.copy_from_slice(&chunk_data[*i..(*i+size)]);

		*i += size;
	}

	fn chunk_read_gen(i: &mut usize, chunk_data: &[u8], t: &mut Gen) -> std::io::Result<()> {
		let mut data = 0u16;

		unsafe {
			read_from_chunk(i, chunk_data, &mut data);

			if data >= Gen::GeneratorMax as u16 {
				*t = Gen::Unused1;
				return Ok(())
			}

			as_u8_slice_mut(t).copy_from_slice(as_u8_slice_mut(&mut data));
		}

		Ok(())
	}

	fn chunk_read_transform(i: &mut usize, chunk_data: &[u8], t: &mut Transform) -> std::io::Result<()> {
		let mut data = 0u16;
		unsafe {
			read_from_chunk(i, chunk_data, &mut data);
		}

		*t = match data {
			2 => Transform::Absolute,
			_ => Transform::Linear,
		};

		Ok(())
	}

	fn chunk_read_u16(i: &mut usize, chunk_data: &[u8], t: &mut u16) {
		unsafe { read_from_chunk(i, chunk_data, t) }
	}

	fn chunk_read_i16(i: &mut usize, chunk_data: &[u8], t: &mut i16) {
		unsafe { read_from_chunk(i, chunk_data, t) }
	}

	fn chunk_read_u32(i: &mut usize, chunk_data: &[u8], t: &mut u32) {
		unsafe { read_from_chunk(i, chunk_data, t) }
	}

	fn chunk_read_bytes(i: &mut usize, chunk_data: &[u8], t: &mut [u8]) {
		unsafe { read_from_chunk(i, chunk_data, t) }
	}


	impl Sf2Data {
		pub fn load_from_file(path: PathBuf, load_sm24: bool, load_progress: Arc<AtomicU32>) -> std::io::Result<Self> {
			load_progress.store(0, Ordering::SeqCst);

			let mut smpl = vec![];
			let mut smpl_compressed = vec![];
			let mut smpl_opt_enabled = true;
			let mut riff = riff::RiffChunk::load_from_file(&path, &mut smpl, &mut smpl_opt_enabled, load_sm24, &load_progress)?;

			let mut result = Sf2Data {
				path,
				name: None,
				version_major: 2,
				version_minor: 1,
				smpl,
				sm24: vec![],
				phdr: vec![],
				pbag: vec![],
				pmod: vec![],
				pgen: vec![],
				inst: vec![],
				ibag: vec![],
				imod: vec![],
				igen: vec![],
				shdr: vec![],
			};

			for chunk in riff.sub_chunks.iter_mut() {

				match chunk.sub_id_str() {
					"INFO" => {
						let Some(ifil) = chunk.get_subchunk("ifil") else {
							return Err(io::Error::new(ErrorKind::InvalidData, "missing `ifil` chunk!"))
						};

						let mut i = 0;

						chunk_read_u16(&mut i, &ifil.chunk_data, &mut result.version_major);
						chunk_read_u16(&mut i, &ifil.chunk_data, &mut result.version_minor);

						if let Some(inam) = chunk.get_subchunk("INAM")
							&& let Some(term) = inam.chunk_data.iter().position(|c| *c == 0)
						{
							let Ok(conv) = String::from_utf8(inam.chunk_data[0..term].into()) else {
								return Err(io::Error::new(ErrorKind::InvalidData, "invalid `INAM` data!"))
							};

							result.name = Some(conv);
						}
					}

					"sdta" => {
						for sub in chunk.sub_chunks.iter_mut() {
							match sub.chunk_id_str() {
								// 16-bit sample data (RAM). no-op in most cases, we already got it earlier
								"smpl" => if !smpl_opt_enabled {
									smpl_compressed = std::mem::take(&mut sub.chunk_data);
								}

								// Least Significant Bytes for 24-bit sample data, combined with `smpl` when resampling
								"sm24" => if load_sm24 {
									result.sm24 = std::mem::take(&mut sub.chunk_data);
								}

								_ => {}
							}
						}
					}

					"pdta" => {
						for sub in chunk.sub_chunks.iter() {
							match sub.chunk_id_str() {
								"phdr" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut header = PresetHeader::default();

										chunk_read_bytes(&mut i, &sub.chunk_data, &mut header.name);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut header.preset);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut header.bank);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut header.preset_bag_idx);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut header.library);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut header.genre);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut header.morphology);

										result.phdr.try_push(header)?;
									}
								}

								"pbag" | "ibag" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut bag = Bag {
											gen_idx: 0,
											mod_idx: 0
										};

										chunk_read_u16(&mut i, &sub.chunk_data, &mut bag.gen_idx);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut bag.mod_idx);

										if sub.chunk_id_str() == "pbag" {
											result.pbag.try_push(bag)?;
										} else {
											result.ibag.try_push(bag)?;
										}
									}
								}

								"pmod" | "imod" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut modulator = ModList::default();

										chunk_read_i16(&mut i, &sub.chunk_data, &mut modulator.src_oper.data);
										chunk_read_gen(&mut i, &sub.chunk_data, &mut modulator.dest_oper)?;
										chunk_read_i16(&mut i, &sub.chunk_data, &mut modulator.mod_amt);
										chunk_read_i16(&mut i, &sub.chunk_data, &mut modulator.mod_amt_src_oper.data);
										chunk_read_transform(&mut i, &sub.chunk_data, &mut modulator.mod_amt_trans_oper)?;

										if sub.chunk_id_str() == "pmod" {
											result.pmod.try_push(modulator)?;
										} else {
											result.imod.try_push(modulator)?;
										}
									}
								}

								"pgen" | "igen" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut genlist = GenList {
											oper: Gen::default(),
											amount: GenAmountType::default(),
										};

										chunk_read_gen(&mut i, &sub.chunk_data, &mut genlist.oper)?;
										chunk_read_i16(&mut i, &sub.chunk_data, genlist.amount.sh_amount_mut());

										if sub.chunk_id_str() == "pgen" {
											result.pgen.try_push(genlist)?;
										} else {
											result.igen.try_push(genlist)?;
										}
									}
								}

								"inst" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut inst = Inst {
											name: [0; 20],
											inst_bag_idx: 0,
										};

										chunk_read_bytes(&mut i, &sub.chunk_data, &mut inst.name);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut inst.inst_bag_idx);

										result.inst.try_push(inst)?;
									}
								}

								"shdr" => {
									let mut i = 0;

									while i < sub.chunk_data.len() {
										let mut sample = Sample {
											name: [0; 20],
											start: 0,
											end: 0,
											start_loop: 0,
											end_loop: 0,
											sample_rate: 0,
											original_pitch: 0,
											pitch_correction: 0,
											sample_link: 0,
											sample_type: SampleType::MonoSample as u16,
										};

										chunk_read_bytes(&mut i, &sub.chunk_data, &mut sample.name);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut sample.start);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut sample.end);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut sample.start_loop);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut sample.end_loop);
										chunk_read_u32(&mut i, &sub.chunk_data, &mut sample.sample_rate);

										// can't be fucked to make separate functions for these.
										// SAFETY: i8 and u8 don't have any invalid bit patterns
										unsafe {
											read_from_chunk(&mut i, &sub.chunk_data, &mut sample.original_pitch);
											read_from_chunk(&mut i, &sub.chunk_data, &mut sample.pitch_correction);
										}

										chunk_read_u16(&mut i, &sub.chunk_data, &mut sample.sample_link);
										chunk_read_u16(&mut i, &sub.chunk_data, &mut sample.sample_type);

										result.shdr.try_push(sample)?;
									}
								}

								_ => {}
							}
						}
					}

					_ => {}
				}
			}

			if result.version_major == 3 {
				result.decompressed(smpl_compressed, load_progress)
			} else {
				Ok(result)
			}
		}

		fn decompressed(mut self, smpl: Vec<u8>, load_progress: Arc<AtomicU32>) -> std::io::Result<Self> {
			let mut pcm_range_end = 0;

			// find non-compressed samples. these must be at the beginning of the SMPL subchunk
			for header in self.shdr.iter() {
				if (header.sample_type & SAMPLE_TYPE_COMPRESSED) == 0 {
					pcm_range_end = pcm_range_end.max(header.end);
				}
		 	}

			// just in case
			self.smpl.clear();

			if pcm_range_end != 0 {	
				self.smpl.try_reserve_exact(pcm_range_end as usize)?;

				pcm_range_end *= 2; // convert from short index to byte index
				
				let slice = unsafe { &mut as_u8_slice_mut(self.smpl.spare_capacity_mut())[0..pcm_range_end as usize] };

				slice.copy_from_slice(&smpl[0..pcm_range_end as usize]);
			}

			let shdr_len = self.shdr.len();

			// now go through samples and decompress any ogg ones
			for (n, header) in self.shdr.iter_mut().enumerate() {
				if (header.sample_type & SAMPLE_TYPE_COMPRESSED) == 0 {
					continue
				}

				load_progress.store((n as u64 * 1000 / shdr_len as u64) as u32 | OGG_FLAG, Ordering::Relaxed);

				let slice = Cursor::new(&smpl[header.start as usize..header.end as usize]);

				let Ok(mut reader) = OggStreamReader::new(slice) else {
					return Err(std::io::Error::new(ErrorKind::InvalidData, "failed to create OGG stream reader!"));
				};
				
				if reader.ident_hdr.audio_channels != 1 {
					return Err(std::io::Error::new(ErrorKind::InvalidData, "multiple channels not supported!"));
				}
				
				let new_start = self.smpl.len();
				
				while let Ok(Some(mut packet)) = reader.read_dec_packet() {
					self.smpl.append(&mut packet[0]);
				}

				header.start = new_start as u32;
				header.end = self.smpl.len() as u32;
				header.start_loop += header.start;
				header.end_loop += header.start;
			}

			self.smpl.shrink_to_fit();

			Ok(self)
		}
	}

	#[derive(Default)]
	pub struct PresetHeader {
		pub name: [u8; 20],
		pub preset: u16,
		pub bank: u16,
		pub preset_bag_idx: u16,
		pub library: u32,
		pub genre: u32,
		pub morphology: u32,
	}

	pub struct Bag {
		pub gen_idx: u16,
		pub mod_idx: u16,
	}

	#[repr(C)]
	#[derive(Clone, Copy, PartialEq, Eq)]
	pub struct RangesType {
		pub lo: u8,
		pub hi: u8,
	}

	#[repr(C)]
	#[derive(Clone, Copy)]
	pub union GenAmountType {
		pub ranges: RangesType,
		pub sh_amount: i16,
		pub w_amount: u16,
	}

	impl Debug for GenAmountType {
		fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
			write!(f, "{}", self.w_amount())
		}
	}

	impl GenAmountType {
		// SAFETY:
		//
		// this entire union is repr(C), and all bit
		// patterns are valid for every union field

		pub fn ranges(&self) -> RangesType {
			unsafe { self.ranges }
		}

		pub fn sh_amount(&self) -> i16 {
			unsafe { self.sh_amount }
		}

		pub fn w_amount(&self) -> u16 {
			unsafe { self.w_amount }
		}

		pub fn ranges_mut(&mut self) -> &mut RangesType {
			unsafe { &mut self.ranges }
		}

		pub fn sh_amount_mut(&mut self) -> &mut i16 {
			unsafe { &mut self.sh_amount }
		}

		pub fn w_amount_mut(&mut self) -> &mut u16 {
			unsafe { &mut self.w_amount }
		}
	}

	#[derive(PartialEq, Eq, Clone, Copy, Debug)]
	pub enum GenType {
		Range,
		Short,
		Word
	}

	impl Default for GenAmountType {
		fn default() -> Self {
			GenAmountType {
				w_amount: 0,
			}
		}
	}

	#[repr(u16)]
	#[derive(Clone, Copy, PartialEq, Eq, Hash, Default, Debug)]
	pub enum Gen {
		StartAddrsOffset = 0,
		EndAddrsOffset = 1,
		StartLoopAddrsOffset = 2,
		EndLoopAddrsOffset = 3,
		StartAddrsCoarseOffset = 4,
		ModLfoToPitch = 5,
		VibLfoToPitch = 6,
		ModEnvToPitch = 7,
		InitialFilterFc = 8,
		InitialFilterQ = 9,
		ModLfoToFilterFc = 10,
		ModEnvToFilterFc = 11,
		EndAddrsCoarseOffset = 12,
		ModLfoToVolume = 13,
		Unused1 = 14,
		ChorusEffectsSend = 15,
		ReverbEffectsSend = 16,
		Pan = 17,
		Unused2 = 18,
		Unused3 = 19,
		Unused4 = 20,
		DelayModLFO = 21,
		FreqModLFO = 22,
		DelayVibLFO = 23,
		FreqVibLFO = 24,
		DelayModEnv = 25,
		AttackModEnv = 26,
		HoldModEnv = 27,
		DecayModEnv = 28,
		SustainModEnv = 29,
		ReleaseModEnv = 30,
		KeynumToModEnvHold = 31,
		KeynumToModEnvDecay = 32,
		DelayVolEnv = 33,
		AttackVolEnv = 34,
		HoldVolEnv = 35,
		DecayVolEnv = 36,
		SustainVolEnv = 37,
		ReleaseVolEnv = 38,
		KeynumToVolEnvHold = 39,
		KeynumToVolEnvDecay = 40,
		Instrument = 41,
		Unused5 = 42,
		KeyRange = 43,
		VelRange = 44,
		StartLoopAddrsCoarseOffset = 45,
		KeyNum = 46,
		Velocity = 47,
		InitialAttenuation = 48,
		EndLoopAddrsCoarseOffset = 50,
		CoarseTune = 51,
		FineTune = 52,
		SampleID = 53,
		SampleModes = 54,
		Unused6 = 55,
		ScaleTuning = 56,
		ExclusiveClass = 57,
		OverridingRootKey = 58,
		#[default]
		GeneratorMax = 59,
	}

	#[repr(u8)]
	pub enum GeneralControlPalette {
		NoController = 0,
		NoteOnVelocity = 2,
		NoteOnKeyNumber = 3,
		PolyPressure = 10,
		ChannelPressure = 13,
		PitchWheel = 14,
		PitchWheelSensitivity = 16,
	}


	impl TryFrom<u8> for GeneralControlPalette {
		type Error = ();

		fn try_from(i: u8) -> Result<GeneralControlPalette, ()> {
			match i {
				0 => Ok(GeneralControlPalette::NoController),
				2 => Ok(GeneralControlPalette::NoteOnVelocity),
				3 => Ok(GeneralControlPalette::NoteOnKeyNumber),
				10 => Ok(GeneralControlPalette::PolyPressure),
				13 => Ok(GeneralControlPalette::ChannelPressure),
				14 => Ok(GeneralControlPalette::PitchWheel),
				16 => Ok(GeneralControlPalette::PitchWheelSensitivity),
				_ => Err(())
			}
		}
	}


	#[repr(u8)]
	pub enum ControllerType {
		Linear = 0,
		Concave = 1,
		Convex = 2,
		Switch = 3,
	}


	impl TryFrom<u8> for ControllerType {
		type Error = ();

		fn try_from(i: u8) -> Result<ControllerType, ()> {
			match i {
				0 => Ok(ControllerType::Linear),
				1 => Ok(ControllerType::Concave),
				2 => Ok(ControllerType::Convex),
				3 => Ok(ControllerType::Switch),
				_ => Err(())
			}
		}
	}


	#[derive(Clone, Copy, PartialEq, Eq, Hash, Default, Debug)]
	pub struct Mod {
		data: i16,
	}

	impl Mod {
		pub fn is_midi_palette_enabled(&self) -> bool {
			(self.data & 0b10000000) != 0
		}

		pub fn is_general_palette_enabled(&self) -> bool {
			!self.is_midi_palette_enabled()
		}

		pub fn direction_bit(&self) -> bool {
			(self.data & 0b100000000) != 0
		}

		pub fn polarity_bit(&self) -> bool {
			(self.data & 0b1000000000) != 0
		}

		pub fn controller_source(&self) -> u8 {
			(self.data & 0b1111111) as u8
		}

		pub fn general_palette_value(&self) -> GeneralControlPalette {
			GeneralControlPalette::try_from(self.controller_source()).unwrap_or(GeneralControlPalette::NoController)
		}

		pub fn controller_type(&self) -> ControllerType {
			ControllerType::try_from((self.data >> 10) as u8).unwrap_or(ControllerType::Linear)
		}
	}

	#[repr(u16)]
	#[derive(Clone, Copy, PartialEq, Eq, Hash, Default, Debug)]
	pub enum Transform {
		#[default]
		Linear = 0,
		Absolute = 2,
	}

	#[repr(u16)]
	#[derive(Debug)]
	pub enum SampleType {
		MonoSample = 0x1,
		RightSample = 0x2,
		LeftSample = 0x4,
		LinkedSample = 0x8,
		CompressedSample = 0x10, // sf3
		RomSample = 0x8000, // unsupported
	}

	pub const SAMPLE_TYPE_MONO: u16         = 0x1;
	pub const SAMPLE_TYPE_RIGHT: u16        = 0x2;
	pub const SAMPLE_TYPE_LEFT: u16         = 0x4;
	pub const SAMPLE_TYPE_LINKED: u16       = 0x8;
	pub const SAMPLE_TYPE_COMPRESSED: u16   = 0x10;
	pub const SAMPLE_TYPE_ROM: u16          = 0x8000;

	/*
	#[repr(u16)]
	#[derive(Debug)]
	pub enum SampleLink {
		MonoSample = 1,
		RightSample = 2,
		LeftSample = 4,
		LinkedSample = 8,
		RomMonoSample = 0x8001,
		RomRightSample = 0x8002,
		RomLeftSample = 0x8004,
		RomLinkedSample = 0x8008,
	}
	*/

	pub struct Inst {
		pub name: [u8; 20],
		pub inst_bag_idx: u16,
	}

	#[derive(Clone, Copy, Debug)]
	pub struct GenList {
		pub oper: Gen,
		pub amount: GenAmountType,
	}

	impl PartialEq for GenList {
		fn eq(&self, other: &Self) -> bool {
			self.oper == other.oper
		}
	}

	#[derive(Clone, Copy, Default, Debug)]
	pub struct ModList {
		pub src_oper: Mod,
		pub dest_oper: Gen,
		pub mod_amt: i16,
		pub mod_amt_src_oper: Mod,
		pub mod_amt_trans_oper: Transform,
	}

	impl ModList {
		pub const fn new(src_oper: i16, dest_oper: Gen, mod_amt: i16, mod_amt_src_oper: i16, mod_amt_trans_oper: Transform) -> Self {
			Self {
				src_oper: Mod { data: src_oper },
				dest_oper,
				mod_amt,
				mod_amt_src_oper: Mod { data: mod_amt_src_oper },
				mod_amt_trans_oper
			}
		}
	}

	impl PartialEq for ModList {
		fn eq(&self, other: &Self) -> bool {
			self.src_oper == other.src_oper &&
			self.dest_oper == other.dest_oper &&
			self.mod_amt_src_oper == other.mod_amt_src_oper
		}
	}

	impl Eq for ModList {}

	impl Hash for ModList {
		fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
			self.src_oper.hash(state);
			self.dest_oper.hash(state);
			self.mod_amt_src_oper.hash(state);
		}
	}

	#[derive(Debug)]
	pub struct GenValues {
		// using the raw value here for better rust-analyzer lints: will error in GenValues::new() if incorrect
		values: [GenAmountType; 59],
	}

	impl Index<Gen> for GenValues {
		type Output = GenAmountType;

		fn index(&self, index: Gen) -> &Self::Output {
			&self.values[index as usize]
		}
	}

	impl IndexMut<Gen> for GenValues {
		fn index_mut(&mut self, index: Gen) -> &mut Self::Output {
			&mut self.values[index as usize]
		}
	}

	impl GenValues {
		pub fn new() -> Self {
			let mut result = GenValues { values: [Default::default(); Gen::GeneratorMax as usize] };

			// set default values for generators

			result[Gen::InitialFilterFc].sh_amount = 13500;

			result[Gen::DelayModLFO].sh_amount = -12000;
			result[Gen::DelayVibLFO].sh_amount = -12000;

			result[Gen::DelayModEnv].sh_amount = -12000;
			result[Gen::AttackModEnv].sh_amount = -12000;
			result[Gen::HoldModEnv].sh_amount = -12000;
			result[Gen::DecayModEnv].sh_amount = -12000;
			result[Gen::SustainModEnv].sh_amount = 0;
			result[Gen::ReleaseModEnv].sh_amount = -12000;

			result[Gen::DelayVolEnv].sh_amount = -12000;
			result[Gen::AttackVolEnv].sh_amount = -12000;
			result[Gen::HoldVolEnv].sh_amount = -12000;
			result[Gen::DecayVolEnv].sh_amount = -12000;
			result[Gen::SustainVolEnv].sh_amount = 0;
			result[Gen::ReleaseVolEnv].sh_amount = -12000;

			result[Gen::KeyRange].ranges.lo = 0;
			result[Gen::KeyRange].ranges.hi = 127;
			result[Gen::VelRange].ranges.lo = 0;
			result[Gen::VelRange].ranges.hi = 127;

			result[Gen::KeyNum].sh_amount = -1;
			result[Gen::Velocity].sh_amount = -1;

			result[Gen::ScaleTuning].sh_amount = 100;

			result[Gen::OverridingRootKey].sh_amount = -1;

			result
		}

		// reset generator values, preserving keynum and velocity
		pub fn reset(&mut self) {
			let keynum = self[Gen::KeyNum];
			let vel = self[Gen::Velocity];
			
			*self = GenValues::new();

			self[Gen::KeyNum] = keynum;
			self[Gen::Velocity] = vel;
		}

		pub fn is_realtime_gen(generator: Gen) -> bool {
			match generator {
				Gen::KeyRange |
				Gen::VelRange |
				Gen::KeyNum |
				Gen::Velocity |
				Gen::SampleID |
				Gen::SampleModes |
				Gen::ScaleTuning |
				Gen::ExclusiveClass |
				Gen::OverridingRootKey |

					// below are technically defined as realtime by the spec, but
					// have no clear meaning when changed midway through a voice
					// or don't support it internally yet
				Gen::DelayModLFO |
				Gen::DelayVibLFO |
				Gen::DelayVolEnv |
				Gen::DelayModEnv |
				Gen::AttackVolEnv |
				Gen::AttackModEnv |
				Gen::HoldVolEnv |
				Gen::HoldModEnv |
				Gen::SustainVolEnv |
				Gen::SustainModEnv |
				Gen::ReleaseVolEnv |
				Gen::ReleaseModEnv
					=> false,

				_
					=> true,
			}
		}

		pub fn get_gen_type(generator: Gen) -> GenType {
			match generator {
				Gen::KeyRange | Gen::VelRange => GenType::Range,

				Gen::StartAddrsOffset |
				Gen::StartAddrsCoarseOffset |
				Gen::InitialFilterQ |
				Gen::ChorusEffectsSend |
				Gen::ReverbEffectsSend |
				Gen::SustainModEnv |
				Gen::SustainVolEnv |
				Gen::InitialAttenuation |
				Gen::ScaleTuning |
				Gen::ExclusiveClass => GenType::Word,

				_ => GenType::Short,
			}

		}

		pub(crate) fn apply_mod_list(&mut self, modlist: &ModList, amt_val: f64, mod_amt_val: f64) {
			let dest = modlist.dest_oper;
			let amt = modlist.mod_amt;
			let gen_type = GenValues::get_gen_type(dest);

			if gen_type == GenType::Range {
				return
			}

			let mut value = amt as f64 * amt_val * mod_amt_val;

			if modlist.mod_amt_trans_oper == Transform::Absolute {
				value = value.abs();
			}

			let value = value.clamp(i16::MIN as f64, i16::MAX as f64) as i16;

			match gen_type {
				GenType::Word => {
					*self[dest].w_amount_mut() = self[dest].w_amount().wrapping_add_signed(value);
				}

				GenType::Short => {
					*self[dest].sh_amount_mut() = self[dest].sh_amount().saturating_add(value);
				}

				_ => unreachable!(),
			}
		}

		pub fn apply_gen_list(&mut self, list: &GenList, abs: bool) {
			if abs {
				self[list.oper] = list.amount;
			} else {
				match Self::get_gen_type(list.oper) {
					GenType::Range => {
						let current = self[list.oper].ranges_mut();
						let update = list.amount.ranges();

						current.lo = current.lo.max(update.lo);
						current.hi = current.hi.min(update.hi);
					}

					GenType::Word => {
						self[list.oper].w_amount = self[list.oper].w_amount().wrapping_add(list.amount.w_amount());
					}

					GenType::Short => {
						let mut result = self[list.oper].sh_amount().saturating_add(list.amount.sh_amount());

						match list.oper {
							Gen::ModLfoToPitch |
							Gen::VibLfoToPitch |
							Gen::ModEnvToPitch |
							Gen::ModLfoToFilterFc |
							Gen::ModEnvToFilterFc => {
								result = result.clamp(-12000, 12000);
							}

							Gen::InitialFilterFc => {
								result = result.clamp(1500, 13500);
							}

							_ => {}
						}

						self[list.oper].sh_amount = result;
					}
				}
			}
		}
	}

	// NOTE: Sinc interpolation is unimplemented
	#[repr(C)]
	#[derive(Clone, Copy, Debug, PartialEq)]
	pub enum ResampleMethod {
		Linear = 0,
		Hermite = 1,
		Sinc16 = 2,
		Sinc32 = 3,
		Sinc64 = 4,
		Nearest = 5,
		Gaussian = 6,
	}

	impl TryFrom<i32> for ResampleMethod {
		type Error = ();

		fn try_from(value: i32) -> Result<ResampleMethod, ()> {
			match value {
				0 => Ok(Self::Linear),
				1 => Ok(Self::Hermite),
				2 => Ok(Self::Sinc16),
				3 => Ok(Self::Sinc32),
				4 => Ok(Self::Sinc64),
				5 => Ok(Self::Nearest),
				6 => Ok(Self::Gaussian),
				_ => Err(()),
			}
		}
	}

	impl std::fmt::Display for ResampleMethod {
		fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
			match self {
				ResampleMethod::Linear => write!(f, "linear"),
				ResampleMethod::Hermite => write!(f, "hermite"),
				ResampleMethod::Sinc16 => write!(f, "sinc16"),
				ResampleMethod::Sinc32 => write!(f, "sinc32"),
				ResampleMethod::Sinc64 => write!(f, "sinc64"),
				ResampleMethod::Nearest => write!(f, "nearest"),
				ResampleMethod::Gaussian => write!(f, "gaussian"),
			}
		}
	}

	// courtesy of https://problemkaputt.de/fullsnes.htm#snesapudspbrrpitch
	const GAUSSIAN_TABLE: &[i32] = &[
		0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,
		0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x002,0x002,0x002,0x002,0x002,
		0x002,0x002,0x003,0x003,0x003,0x003,0x003,0x004,0x004,0x004,0x004,0x004,0x005,0x005,0x005,0x005,
		0x006,0x006,0x006,0x006,0x007,0x007,0x007,0x008,0x008,0x008,0x009,0x009,0x009,0x00A,0x00A,0x00A,
		0x00B,0x00B,0x00B,0x00C,0x00C,0x00D,0x00D,0x00E,0x00E,0x00F,0x00F,0x00F,0x010,0x010,0x011,0x011,
		0x012,0x013,0x013,0x014,0x014,0x015,0x015,0x016,0x017,0x017,0x018,0x018,0x019,0x01A,0x01B,0x01B,
		0x01C,0x01D,0x01D,0x01E,0x01F,0x020,0x020,0x021,0x022,0x023,0x024,0x024,0x025,0x026,0x027,0x028,
		0x029,0x02A,0x02B,0x02C,0x02D,0x02E,0x02F,0x030,0x031,0x032,0x033,0x034,0x035,0x036,0x037,0x038,
		0x03A,0x03B,0x03C,0x03D,0x03E,0x040,0x041,0x042,0x043,0x045,0x046,0x047,0x049,0x04A,0x04C,0x04D,
		0x04E,0x050,0x051,0x053,0x054,0x056,0x057,0x059,0x05A,0x05C,0x05E,0x05F,0x061,0x063,0x064,0x066,
		0x068,0x06A,0x06B,0x06D,0x06F,0x071,0x073,0x075,0x076,0x078,0x07A,0x07C,0x07E,0x080,0x082,0x084,
		0x086,0x089,0x08B,0x08D,0x08F,0x091,0x093,0x096,0x098,0x09A,0x09C,0x09F,0x0A1,0x0A3,0x0A6,0x0A8,
		0x0AB,0x0AD,0x0AF,0x0B2,0x0B4,0x0B7,0x0BA,0x0BC,0x0BF,0x0C1,0x0C4,0x0C7,0x0C9,0x0CC,0x0CF,0x0D2,
		0x0D4,0x0D7,0x0DA,0x0DD,0x0E0,0x0E3,0x0E6,0x0E9,0x0EC,0x0EF,0x0F2,0x0F5,0x0F8,0x0FB,0x0FE,0x101,
		0x104,0x107,0x10B,0x10E,0x111,0x114,0x118,0x11B,0x11E,0x122,0x125,0x129,0x12C,0x130,0x133,0x137,
		0x13A,0x13E,0x141,0x145,0x148,0x14C,0x150,0x153,0x157,0x15B,0x15F,0x162,0x166,0x16A,0x16E,0x172,
		0x176,0x17A,0x17D,0x181,0x185,0x189,0x18D,0x191,0x195,0x19A,0x19E,0x1A2,0x1A6,0x1AA,0x1AE,0x1B2,
		0x1B7,0x1BB,0x1BF,0x1C3,0x1C8,0x1CC,0x1D0,0x1D5,0x1D9,0x1DD,0x1E2,0x1E6,0x1EB,0x1EF,0x1F3,0x1F8,
		0x1FC,0x201,0x205,0x20A,0x20F,0x213,0x218,0x21C,0x221,0x226,0x22A,0x22F,0x233,0x238,0x23D,0x241,
		0x246,0x24B,0x250,0x254,0x259,0x25E,0x263,0x267,0x26C,0x271,0x276,0x27B,0x280,0x284,0x289,0x28E,
		0x293,0x298,0x29D,0x2A2,0x2A6,0x2AB,0x2B0,0x2B5,0x2BA,0x2BF,0x2C4,0x2C9,0x2CE,0x2D3,0x2D8,0x2DC,
		0x2E1,0x2E6,0x2EB,0x2F0,0x2F5,0x2FA,0x2FF,0x304,0x309,0x30E,0x313,0x318,0x31D,0x322,0x326,0x32B,
		0x330,0x335,0x33A,0x33F,0x344,0x349,0x34E,0x353,0x357,0x35C,0x361,0x366,0x36B,0x370,0x374,0x379,
		0x37E,0x383,0x388,0x38C,0x391,0x396,0x39B,0x39F,0x3A4,0x3A9,0x3AD,0x3B2,0x3B7,0x3BB,0x3C0,0x3C5,
		0x3C9,0x3CE,0x3D2,0x3D7,0x3DC,0x3E0,0x3E5,0x3E9,0x3ED,0x3F2,0x3F6,0x3FB,0x3FF,0x403,0x408,0x40C,
		0x410,0x415,0x419,0x41D,0x421,0x425,0x42A,0x42E,0x432,0x436,0x43A,0x43E,0x442,0x446,0x44A,0x44E,
		0x452,0x455,0x459,0x45D,0x461,0x465,0x468,0x46C,0x470,0x473,0x477,0x47A,0x47E,0x481,0x485,0x488,
		0x48C,0x48F,0x492,0x496,0x499,0x49C,0x49F,0x4A2,0x4A6,0x4A9,0x4AC,0x4AF,0x4B2,0x4B5,0x4B7,0x4BA,
		0x4BD,0x4C0,0x4C3,0x4C5,0x4C8,0x4CB,0x4CD,0x4D0,0x4D2,0x4D5,0x4D7,0x4D9,0x4DC,0x4DE,0x4E0,0x4E3,
		0x4E5,0x4E7,0x4E9,0x4EB,0x4ED,0x4EF,0x4F1,0x4F3,0x4F5,0x4F6,0x4F8,0x4FA,0x4FB,0x4FD,0x4FF,0x500,
		0x502,0x503,0x504,0x506,0x507,0x508,0x50A,0x50B,0x50C,0x50D,0x50E,0x50F,0x510,0x511,0x511,0x512,
		0x513,0x514,0x514,0x515,0x516,0x516,0x517,0x517,0x517,0x518,0x518,0x518,0x518,0x518,0x519,0x519,
	];

	#[derive(Debug)]
	pub struct Sample {
		pub name: [u8; 20],
		pub start: u32,
		pub end: u32,
		pub start_loop: u32,
		pub end_loop: u32,
		pub sample_rate: u32,
		pub original_pitch: u8,
		pub pitch_correction: i8,
		pub sample_link: u16,
		pub sample_type: u16,
	}

	impl Sample {
		pub fn is_rom(&self) -> bool {
			(self.sample_type & SAMPLE_TYPE_ROM) != 0
		}

		pub fn should_loop(sample_mode: i16, released: bool) -> bool {
			// NOTE: the spec technically says `sample_mode == 2` should not loop,
			// but in practice implementations seem to treat it like `sample_mode == 1`
			sample_mode == 1 || sample_mode == 2 || (sample_mode == 3 && !released)
		}

		fn offset_calc(origin: u32, gen_values: &GenValues, coarse: Gen, fine: Gen) -> u32 {
			let coarse_amt = gen_values[coarse].sh_amount() as i64;
			let fine_amt = gen_values[fine].sh_amount() as i64;

			(origin as i64 + 32768 * coarse_amt + fine_amt) as u32
		}

		pub fn get_start(&self, gen_values: &GenValues) -> u32 {
			Self::offset_calc(self.start, gen_values, Gen::StartAddrsCoarseOffset, Gen::StartAddrsOffset)
		}

		pub fn get_end(&self, gen_values: &GenValues) -> u32 {
			Self::offset_calc(self.end, gen_values, Gen::EndAddrsCoarseOffset, Gen::EndAddrsOffset)
		}

		pub fn get_start_loop(&self, gen_values: &GenValues) -> u32 {
			Self::offset_calc(self.start_loop, gen_values, Gen::StartLoopAddrsCoarseOffset, Gen::StartLoopAddrsOffset)
		}

		pub fn get_end_loop(&self, gen_values: &GenValues) -> u32 {
			Self::offset_calc(self.end_loop, gen_values, Gen::EndLoopAddrsCoarseOffset, Gen::EndLoopAddrsOffset)
		}

		pub fn get_remapped_offset(
			mut current_pos: f64,
			should_loop: bool,
			end: f64,
			start_loop: f64,
			end_loop: f64,
		) -> f64 {
			if should_loop && end_loop != start_loop {
				while current_pos >= end_loop {
					current_pos -= end_loop - start_loop;
				}

				current_pos
			} else {
				current_pos.min(end - 1.0)
			}
		}

		pub fn resample(
			&self,
			offset: &mut f64,
			phase_inc: f64,
			method: ResampleMethod,
			fetch_sample: impl Fn(usize) -> f32,
			out: &mut [f32],
			released: bool,
			gen_values: &GenValues,
			env: &[f32],
		) {
			debug_assert!(out.len() == env.len());

			let mut start_offset = *offset;
			let start = self.get_start(gen_values);
			let end = self.get_end(gen_values).max(start);

			let start_loop = (self.get_start_loop(gen_values).max(start) - start) as f64;
			let mut end_loop = (self.get_end_loop(gen_values).min(end) - start) as f64;
			let end = (end - start) as f64;

			if end_loop < start_loop {
				end_loop = end;
			}

			let should_loop = Sample::should_loop(gen_values[Gen::SampleModes].sh_amount(), released);

			match method {
				ResampleMethod::Nearest => {
					for (sample, env) in out.iter_mut().zip(env.iter().copied()) {
						let s = fetch_sample(start_offset as usize);

						*sample = s * env;

						start_offset = Self::get_remapped_offset(
							start_offset + phase_inc,
							should_loop,
							end,
							start_loop,
							end_loop,
						)
					}
				}

				ResampleMethod::Linear => {
					for (sample, env) in out.iter_mut().zip(env.iter().copied()) {
						let next = Self::get_remapped_offset(
							start_offset + 1.0,
							should_loop,
							end,
							start_loop,
							end_loop,
						);

						let a = start_offset as usize;
						let b = next as usize;
						let t = start_offset - a as f64;

						let sa = fetch_sample(a);
						let sb = fetch_sample(b);

						let t = t as f32;
						let s = sa * (1.0 - t) + sb * t;

						*sample = s * env;

						start_offset = Self::get_remapped_offset(
							start_offset + phase_inc,
							should_loop,
							end,
							start_loop,
							end_loop,
						)
					}
				}

				ResampleMethod::Hermite => {
					for (sample, env) in out.iter_mut().zip(env.iter().copied()) {
						let y = |i| fetch_sample(Self::get_remapped_offset(
								start_offset + i,
								should_loop,
								end,
								start_loop,
								end_loop,
							) as usize);

						// compensate for phase difference
						let ofs = -(start_offset as isize).min(2) as f64;

						let x = start_offset.fract() as f32;
						let ym2 = y(ofs+0.0);
						let ym1 = y(ofs+1.0);
						let y0 = y(ofs+2.0);
						let y1 = y(ofs+3.0);
						let y2 = y(ofs+4.0);
						let y3 = y(ofs+5.0);

						let c0 = y0;
						let c1 = 1.0/12.0*(ym2-y2) + 2.0/3.0*(y1-ym1);
						let c2 = 5.0/4.0*ym1 - 7.0/3.0*y0 + 5.0/3.0*y1 - 0.5*y2 + 1.0/12.0*y3 - 1.0/6.0*ym2;
						let c3 = 1.0/12.0*(ym2-y3) + 7.0/12.0*(y2-ym1) + 4.0/3.0*(y0-y1);

						*sample = (((c3*x+c2)*x+c1)*x+c0) * env;

						start_offset = Self::get_remapped_offset(
							start_offset + phase_inc,
							should_loop,
							end,
							start_loop,
							end_loop,
						)
					}
				}

				ResampleMethod::Gaussian => {
					let maprange = i16::MAX as f32;
					let inv_maprange = 1.0 / maprange;

					for (sample, env) in out.iter_mut().zip(env.iter().copied()) {
						let y = |i| (fetch_sample(Self::get_remapped_offset(
								start_offset + i as f64,
								should_loop,
								end,
								start_loop,
								end_loop,
							) as usize) * maprange) as i32;


						let x0 = y(0);
						let x1 = y(1);
						let x2 = y(2);
						let x3 = y(3);
						
						let f = ((start_offset.fract() * 4096.0) as usize >> 4) & 0xFF;

						let mut s = 0;

						// SAFETY: `f` is always <= 0xFF, so none of these indices
						// can leave the range 0x0..0x200
						unsafe {
							s += (GAUSSIAN_TABLE.get_unchecked(0x0FF - f) * x0) >> 10;
							s += (GAUSSIAN_TABLE.get_unchecked(0x1FF - f) * x1) >> 10;
							s += (GAUSSIAN_TABLE.get_unchecked(0x100 + f) * x2) >> 10;
							s += (GAUSSIAN_TABLE.get_unchecked(0x000 + f) * x3) >> 10;
						}
						s >>= 1;

						*sample = s as f32 * inv_maprange * env;

						start_offset = Self::get_remapped_offset(
							start_offset + phase_inc,
							should_loop,
							end,
							start_loop,
							end_loop,
						)
					}
				}

				ResampleMethod::Sinc16 | ResampleMethod::Sinc32 | ResampleMethod::Sinc64 => {}
			}

			*offset = start_offset;
		}
	}
}

pub mod hi {
	use std::{collections::{HashMap, HashSet}, f64::consts::PI, path::PathBuf, sync::Arc};

	use super::lo::{ControllerType, Gen, GenList, GenValues, GeneralControlPalette, Mod, ModList, ResampleMethod, Sample, Sf2Data, Transform};

	const SEMITONE_RATIO: f64 = 1.05946309436;
	const TIMECENT_BASE: f64 = 1.00057778951;

	fn cents_to_hz_mult(cents: f64) -> f64 {
		SEMITONE_RATIO.powf(cents / 100.0)
	}

	fn centibels_to_gain(cb: f32) -> f32 {
		10.0f32.powf(cb / 200.0)
	}

	pub fn timecents_to_secs(timecents: f64) -> f64 {
		TIMECENT_BASE.powf(timecents)
	}

	pub fn millis_to_timecents(millis: f64) -> f64 {
		1200.0 * (millis / 1000.0).log2()
	}

	fn concave_curve(t: f64) -> f64 {
		if t <= 0.0 {
			return 0.0
		}
		if t >= 1.0 {
			return 1.0
		}

		let t = 1.0 - t;

		(-20.0 / 96.0) * (t * t).ln()
	}

	fn sf_curve(f: f64, t: ControllerType, clamp: bool) -> f64 {
		let result = match t {
			ControllerType::Linear => f,
			ControllerType::Concave => concave_curve(f),
			ControllerType::Convex => 1.0 - concave_curve(1.0 - f),
			ControllerType::Switch => if f >= 0.5 { 1.0 } else { 0.0 }
		};

		if clamp {
			result.clamp(0.0, 1.0)
		} else {
			result
		}
	}

	#[derive(Debug)]
	pub struct Zone {
		pub mods: Vec<ModList>,
		pub gens: Vec<GenList>,
	}

	impl Zone {
		pub fn get_key_range_gen(&self) -> Option<&GenList> {
			if let key @ Some(GenList { oper: Gen::KeyRange, .. }) = self.gens.get(0) {
				key
			} else {
				None
			}
		}

		pub fn get_vel_range_gen(&self) -> Option<&GenList> {
			if let vel @ Some(GenList { oper: Gen::VelRange, .. }) = self.gens.get(0) {
				vel
			} else if let vel @ Some(GenList { oper: Gen::VelRange, .. }) = self.gens.get(1) {
				vel
			} else {
				None
			}
		}

		pub fn get_instrument_gen(&self) -> Option<&GenList> {
			if let inst @ Some(GenList { oper: Gen::Instrument, .. }) = self.gens.last() {
				inst
			} else {
				None
			}
		}

		pub fn get_sample_id_gen(&self) -> Option<&GenList> {
			if let smpl @ Some(GenList { oper: Gen::SampleID, .. }) = self.gens.last() {
				smpl
			} else {
				None
			}
		}

		pub fn is_active(&self, keynum: u8, vel: u8) -> bool {
			if let Some(key_range) = self.get_key_range_gen() {
				let range = key_range.amount.ranges();

				if keynum < range.lo || keynum > range.hi {
					return false
				}
			}

			if let Some(vel_range) = self.get_vel_range_gen() {
				let range = vel_range.amount.ranges();

				if vel < range.lo || vel > range.hi {
					return false
				}
			}

			true
		}
	}

	#[derive(Debug)]
	pub struct Preset {
		pub name: String,
		pub preset: u16,
		pub bank: u16,

		pub zones: Vec<Arc<Zone>>,

		pub library: u32,
		pub genre: u32,
		pub morphology: u32,
	}

	#[derive(Debug)]
	pub struct Instrument {
		pub name: String,

		pub zones: Vec<Arc<Zone>>,
	}

	#[derive(Default, Debug)]
	pub struct LfoState {
		pub phase: f64,
		pub delay_progress: f64,
	}

	impl LfoState {
		pub fn value(&self) -> f64 {
			let state = self.phase % 1.0;

			if state >= 0.75 {
				(state - 0.75) * 4.0 - 1.0
			} else if state >= 0.25 {
				1.0 - (state - 0.25) * 4.0
			} else {
				state * 4.0
			}
		}

		fn update(&mut self, delay: f64, freq: f64, sample_rate: u32, samples: usize) {
			let elapsed = samples as f64 / sample_rate as f64;

			if self.delay_progress < 1.0 {
				self.delay_progress += elapsed / delay;
			} else {
				self.phase = (self.phase + elapsed * freq) % 1.0;
			}
		}
	}

	#[derive(Debug, Default)]
	pub struct SynthOverrides {
		pub attack: i32,
		pub decay: i32,
		pub sustain: i32,
		pub release: i32,

		pub lfo_2_del: i32,
		pub lfo_2_amt: i32,
		pub lfo_2_spd: i32,

		pub filter_fc: i32,
		pub modulation: i32,

		pub filter_bypass: bool,
	}

	#[repr(u8)]
	#[derive(Debug, Clone, Copy, PartialEq, Eq)]
	pub enum EngineTweak {
		MidiMappingFix = 0,
		SkipShortEnvPhases = 1,
		OverrideHoldWithDecay = 2,
		FilterFix = 3,
		EngineTweakMax = 4,
	}

	impl TryFrom<u8> for EngineTweak {
		type Error = ();

		fn try_from(value: u8) -> Result<Self, Self::Error> {
			if value <= EngineTweak::EngineTweakMax as u8 {
				Ok(unsafe { std::mem::transmute(value) })
			} else {
				Err(())
			}
		}
	}

	impl EngineTweak {
		pub fn newest_default(tweak: usize) -> bool {
			match tweak {
				_ => true,
			}
		}

		pub fn compat_default(tweak: usize) -> bool {
			match tweak {
				0 => true,
				_ => false,
			}
		}
	}

	#[derive(Debug)]
	pub struct EngineSettings {
		pub tweaks: u64,
		pub env_phase_skip_threshold: i16,
	}

	impl EngineSettings {
		fn tweak(&self, tweak: EngineTweak) -> bool {
			(self.tweaks & (1 << tweak as usize)) != 0
		}
	}

	impl Default for EngineSettings {
		fn default() -> Self {
			Self {
				tweaks: 0,
				env_phase_skip_threshold: -12000,
			}
		}
	}

	#[derive(Debug)]
	pub struct RenderContext {
		pub overrides: SynthOverrides,
		pub settings: EngineSettings,
		mods_filter: HashSet<ModList>,
		active_mods_pool: Vec<Vec<ModList>>,
		inst_voice_vec_pool: Vec<Vec<InstVoice>>,
	}

	impl RenderContext {
		pub fn new() -> Self {
			Self::with_poly(64)
		}

		pub fn with_poly(voices: usize) -> Self {
			Self::with_capacity(voices, 4, 32, 128)
		}

		pub fn with_capacity(
			voices: usize,
			inst_voices_per_voice: usize,
			mods_per_inst_voice: usize,
			mods_filter: usize,
		) -> Self {
			let inst_voices = inst_voices_per_voice * voices;

			Self {
				overrides: SynthOverrides::default(),
				settings: EngineSettings::default(),

				// pre-allocate room for `mods_filter` modulators to be filtered at once
				//
				// this number is fairly arbitrary, but should be enough to
				// take care of most use-cases without using an excessive
				// amount of memory
				mods_filter: HashSet::with_capacity(mods_filter),

				// pre-allocate room for InstVoice vectors
				// every Voice can contain an arbitrary number of InstVoices,
				// but they generally contain 1 or 2 (mono/stereo).
				//
				// we allocate room for `voices` number of Voices, with
				// max `inst_voices_per_voice` InstVoices each, which in turn
				// have capacity for `mods_per_inst_voice` active modulators
				inst_voice_vec_pool: (0..voices).into_iter().map(|_| Vec::with_capacity(inst_voices_per_voice)).collect(),
				active_mods_pool: (0..inst_voices).into_iter().map(|_| Vec::with_capacity(mods_per_inst_voice)).collect(),
			}
		}

		fn get_mod_src_value_raw(&self, v: &Voice<impl HostData>, src: Mod) -> f64 {
			if src.is_general_palette_enabled() {
				match src.general_palette_value() {
					GeneralControlPalette::NoController => 1.0,

					GeneralControlPalette::NoteOnVelocity => v.note_on_velocity as f64 + v.vel_offset as f64,

					GeneralControlPalette::NoteOnKeyNumber => v.note_on_key_number as f64,

					GeneralControlPalette::ChannelPressure => v.host_data.channel_pressure() as f64 / 127.0,

					GeneralControlPalette::PolyPressure => v.host_data.poly_pressure() as f64 / 127.0,

					_ => 1.0,
				}
			} else {
				let ctrl_src = src.controller_source();

				if self.overrides.modulation != -1 && let Ok(MidiCC::ModWheel) = ctrl_src.try_into() {
					return self.overrides.modulation as f64 / 127.0
				}

				let host_val = v.host_data.midi_cc_value(ctrl_src);

				if host_val != HOST_CC_NONE {
					if self.settings.tweak(EngineTweak::MidiMappingFix) {
						// HACK: the SF spec specifies the CC range mapping as [0.0, ~0.992],
						// since it's an even number of values and mapping it to [0.0, 1.0] would
						// cause there to be no value that lands directly on 0.5
						//
						// that said, having no value that lands on 1 means that i.e. hard-panning
						// right is impossible, so we use this compromise - stretching the values
						// above 0.5 to reach all the way up to 1.0
						//
						// you can see the graph of this mapping here:
						// https://www.desmos.com/calculator/gyjmp5ezxl
						if host_val > 64 {
							0.5 + (host_val - 64) as f64 / 126.0
						} else {
							host_val as f64 / 128.0
						}
					} else {
						host_val as f64 / 128.0
					}
				} else {
					match ctrl_src.try_into() {
						Ok(MidiCC::Balance | MidiCC::Pan) => 0.5,

						Ok(MidiCC::Volume | MidiCC::Expression) => 1.0,

						_ => 0.0
					}
				}
			}
		}

		pub(crate) fn get_mod_src_value(&self, v: &Voice<impl HostData>, src: Mod) -> f64 {
			let mut val = self.get_mod_src_value_raw(v, src);

			if src.direction_bit() {
				val = 1.0 - val;
			}

			if src.polarity_bit() {
				val -= 0.5;
				val *= 2.0;
			}

			sf_curve(val.abs(), src.controller_type(), true) * val.signum()
		}
	}

	#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
	pub enum EnvPhase {
		#[default]
		NotStarted,
		Delay,
		Attack,
		Hold,
		Decay,
		Sustain,
		Release,
		Stop
	}

	impl EnvPhase {
		fn next(self) -> EnvPhase {
			match self {
				EnvPhase::NotStarted => EnvPhase::Delay,
				EnvPhase::Delay => EnvPhase::Attack,
				EnvPhase::Attack => EnvPhase::Hold,
				EnvPhase::Hold => EnvPhase::Decay,
				EnvPhase::Decay => EnvPhase::Sustain,
				EnvPhase::Sustain => EnvPhase::Release,
				EnvPhase::Release => EnvPhase::Stop,
				EnvPhase::Stop => EnvPhase::Stop,
			}
		}
	}

	#[derive(Debug)]
	pub struct EnvState {
		pub phase: EnvPhase,
		pub prev_phase: EnvPhase,

		pub amt_at_release: f64,
		pub state_progress: f64,

		pub delay_smpls: usize,

		pub time_cache: f64,
		pub env_prev_time: i16,

		pub is_mod: bool,
	}

	impl EnvState {
		pub fn volume() -> Self {
			EnvState {
				phase: EnvPhase::NotStarted,
				prev_phase: EnvPhase::NotStarted,

				amt_at_release: 0.0,
				state_progress: 0.0,
				delay_smpls: 0,

				time_cache: 0.0,
				env_prev_time: 0,

				is_mod: false,
			}
		}

		pub fn modulation() -> Self {
			EnvState {
				is_mod: true,
				..Self::volume()
			}
		}

		pub fn process_fine(
			&mut self,
			del: i16,
			atk: i16,
			hold: i16,
			dec: i16,
			sus: f64,
			rel: i16,
			sample_rate: u32,
			out: &mut [f32],
			ctx: &mut RenderContext,
		) {
			self.process(del, atk, hold, dec, sus, rel, sample_rate, 0, Some(out), ctx);
		}

		pub fn process_coarse(
			&mut self,
			del: i16,
			atk: i16,
			hold: i16,
			dec: i16,
			sus: f64,
			rel: i16,
			sample_rate: u32,
			samples: usize,
			ctx: &mut RenderContext,
		) {
			self.process(del, atk, hold, dec, sus, rel, sample_rate, samples, None, ctx);
		}

		fn process(
			&mut self,
			del: i16,
			atk: i16,
			hold: i16,
			dec: i16,
			sus: f64,
			rel: i16,
			sample_rate: u32,
			mut len: usize,
			mut out: Option<&mut [f32]>,
			ctx: &mut RenderContext,
		) {
			if let Some(out) = &out {
				len = out.len();
			}

			let mut i = 0;
			let delta = 1.0 / sample_rate as f64;

			while i < len {
				if self.state_progress >= 1.0 || self.phase == EnvPhase::NotStarted {
					self.state_progress = 0.0;
					self.phase = self.phase.next();
				}

				let time_sh = match self.phase {
					EnvPhase::Delay => del,
					EnvPhase::Attack => atk,
					EnvPhase::Hold => hold,
					EnvPhase::Decay => dec,
					EnvPhase::Release => rel.max(-9000),
					_ => 0,
				};

				if self.phase != self.prev_phase || time_sh != self.env_prev_time {
					// skip phases shorter than env_phase_skip_threshold (default: ~1ms)
					if ctx.settings.tweak(EngineTweak::SkipShortEnvPhases) && time_sh <= ctx.settings.env_phase_skip_threshold {
						self.state_progress = 1.0;
						continue
					}

					self.time_cache = timecents_to_secs(time_sh as f64);

					if self.phase == EnvPhase::Release {
						self.time_cache *= self.amt_at_release;
					}
					else if self.phase == EnvPhase::Decay {
						self.time_cache *= 1.0 - sus;
					}

					if self.time_cache == 0.0 {
						self.state_progress = 1.0;
						continue
					}

					self.env_prev_time = time_sh;
					self.prev_phase = self.phase;
				}

				if self.phase == EnvPhase::Sustain {
					// a voice in the Sustain phase can only be released manually and inbetween buffers,
					// so we can just keep filling the buffer with the sustain value until the voice gets released
					if let Some(out) = &mut out {
						out[i..].fill(sus as f32);
					}

					break
				} else if self.phase == EnvPhase::Stop {
					break
				}

				while i < len && self.state_progress < 1.0 {
					self.state_progress += delta / self.time_cache;

					if let Some(out) = &mut out {
						out[i] = self.value(sus) as f32;
					}

					if self.phase == EnvPhase::Delay {
						self.delay_smpls += 1;
					}

					i += 1;
				}
			}
		}

		fn release(&mut self, sustain: f64) {
			if matches!(self.phase, EnvPhase::Release | EnvPhase::Stop) {
				return
			}

			self.amt_at_release = self.value(sustain);
			self.phase = EnvPhase::Release;
			self.state_progress = 0.0;
		}

		fn value(&self, sustain: f64) -> f64 {
			match self.phase {
				EnvPhase::NotStarted | EnvPhase::Delay | EnvPhase::Stop => 0.0,

				EnvPhase::Attack => {
					if self.is_mod {
						self.state_progress.clamp(0.0, 1.0)
					} else {
						if self.state_progress <= 0.0 {
							return 0.0
						}

						let logprog = self.state_progress.min(1.0).ln();

						(1.0 + (20.0 / 96.0) * logprog).clamp(0.0, 1.0)
					}
				}

				EnvPhase::Hold => 1.0,

				EnvPhase::Decay => 1.0 * (1.0 - self.state_progress) + sustain * self.state_progress,

				EnvPhase::Sustain => sustain,

				EnvPhase::Release => self.amt_at_release * (1.0 - self.state_progress),
			}
		}
	}

	#[derive(Default, Debug)]
	pub struct FilterState {
		last_res_freq: f64,
		last_q: f64,
		last_sample_rate: u32,

		pub ic1eq: f32,
		pub ic2eq: f32,

		pub a1: f32,
		pub a2: f32,
		pub a3: f32,

		pub g: f32,
		pub k: f32,
		pub v0: f32,
		pub v1: f32,
		pub v2: f32,
		pub v3: f32
	}

	impl FilterState {
		pub fn calculate_coefficients(&mut self) {
			self.g = (PI * self.last_res_freq / (self.last_sample_rate as f64)).tan() as f32;
			self.k = 1.0 / self.last_q as f32;
			self.a1 = 1.0 / (1.0 + self.g * (self.g + self.k));
			self.a2 = self.g * self.a1;
			self.a3 = self.g * self.a2;
		}

		pub fn set_params(&mut self, mut res_freq: f64, q: f64, sample_rate: u32) {
			let mut stale = false;

			res_freq = res_freq.min(0.45 * sample_rate as f64).max(5.0);

			if res_freq != self.last_res_freq {
				self.last_res_freq = res_freq;
				stale = true;
			}

			if q != self.last_q {
				self.last_q = q;
				stale = true;
			}

			if sample_rate != self.last_sample_rate {
				self.last_sample_rate = sample_rate;
				stale = true;
			}

			if stale {
				self.calculate_coefficients();
			}
		}

		pub fn reset(&mut self) {
			self.ic1eq = 0.0;
			self.ic2eq = 0.0;
		}
	}

	#[derive(Debug)]
	pub struct InstVoice {
		pub sf: Arc<SoundFont>,

		pub sample: Arc<Sample>,

		pub p_global_zone: Option<Arc<Zone>>,
		pub i_global_zone: Option<Arc<Zone>>,

		pub p_zone: Arc<Zone>,
		pub i_zone: Arc<Zone>,

		pub gen_values: GenValues,

		pub active_mods: Vec<ModList>,

		pub mod_env: EnvState,
		pub vol_env: EnvState,
		pub filter: FilterState,

		pub mod_lfo: LfoState,
		pub vib_lfo: LfoState,

		pub source_progress: f64,
		pub last_vol: (f32, f32)
	}

	// MIDI CC values only range from 0-127, so this is cheaper than Option<u8>
	pub const HOST_CC_NONE: u8 = 0xFF;

	pub trait HostData {
		fn midi_cc_value(&self, _cc: u8) -> u8 { HOST_CC_NONE }

		// channel / polyphonic aftertouch, 0-127
		fn channel_pressure(&self) -> u8 { 0 }
		fn poly_pressure(&self) -> u8 { 0 }
	}

	#[derive(Debug)]
	pub struct Voice<H: HostData> {
		pub inst_voices: Option<Vec<InstVoice>>,

		pub init_pitch: f32,
		pub pitch_offset: f32,
		pub first_rendering: bool,
		pub released: bool,
		pub has_active_voices: bool,

		pub note_on_key_number: f32,
		pub note_on_velocity: f32,
		pub vel_offset: f32,

		pub host_data: H,
	}

	impl<H: HostData> Voice<H> {
		// call this before dropping to return resources to the RenderContext.
		// not *necessary*, but helps prevent allocating on the audio thread
		pub fn release_resources(&mut self, ctx: &mut RenderContext) {
			let Some(mut inst_voices) = self.inst_voices.take() else {
				return
			};

			// clear out and return vectors so we get to reuse the allocated memory
			// is this worth it? shrug. but i'm doin' it

			for inst_voice in inst_voices.iter_mut() {
				 let mut mods = std::mem::take(&mut inst_voice.active_mods);
				
				 mods.clear();
				 ctx.active_mods_pool.push(mods);
			}

			inst_voices.clear();
			ctx.inst_voice_vec_pool.push(inst_voices);
		}

		pub fn choke(&mut self) {
			let Some(inst_voices) = self.inst_voices.as_mut() else {
				return
			};

			for iv in inst_voices.iter_mut() {
				iv.vol_env.phase = EnvPhase::Stop;
				iv.mod_env.phase = EnvPhase::Stop;
			}
		}
	}

	// see SoundFont 2.01 spec for definitions
	pub static DEFAULT_MODS: &[ModList] = &[
		// 8.4.1 MIDI Note-On Velocity to Initial Attenuation
		ModList::new(
			0x0502,
			Gen::InitialAttenuation,
			960,
			0x0,
			Transform::Linear,
		),

		// 8.4.2 MIDI Note-On Velocity to Filter Cutoff
		ModList::new(
			0x0102,
			Gen::InitialFilterFc,
			-2400,
			0x0,
			Transform::Linear,
		),

		// 8.4.3 MIDI Channel Pressure to Vibrato LFO Pitch Depth
		ModList::new(
			0x000D,
			Gen::VibLfoToPitch,
			50,
			0x0,
			Transform::Linear,
		),

		// 8.4.4 MIDI Continuous Controller 1 (Modulation Wheel) to Vibrato LFO Pitch Depth
		ModList::new(
			0x0081,
			Gen::VibLfoToPitch,
			50,
			0x0,
			Transform::Linear,
		),

		// 8.4.5 MIDI Continuous Controller 7 (Volume) to Initial Attenuation
		ModList::new(
			0x0587,
			Gen::InitialAttenuation,
			960,
			0x0,
			Transform::Linear,
		),

		// 8.4.6 MIDI Continuous Controller 10 (Pan) to Pan Position
		ModList::new(
			0x028A,
			Gen::Pan,
			500, // says 1000 in spec, but pan ranges from -500 to 500
			0x0,
			Transform::Linear,
		),

		// 8.4.7 MIDI Continuous Controller 11 (Expression) to Initial Attenuation
		ModList::new(
			0x058B,
			Gen::InitialAttenuation,
			960,
			0x0,
			Transform::Linear,
		),

		// 8.4.8 MIDI Continuous Controller 91 to Reverb Effects Send
		ModList::new(
			0x00DB,
			Gen::ReverbEffectsSend,
			200,
			0x0,
			Transform::Linear,
		),

		// 8.4.9 MIDI Continuous Controller 93 to Chorus Effects Send
		ModList::new(
			0x00DD,
			Gen::ChorusEffectsSend,
			200,
			0x0,
			Transform::Linear,
		),
	];

	#[derive(Debug)]
	pub struct SoundFont {
		pub path: PathBuf,
		pub name: Option<String>,
		pub presets: Vec<Preset>,
		pub instruments: Vec<Instrument>,
		pub sample_data: Vec<i16>,
		pub sample_data_24: Vec<u8>,
		pub sample_headers: Vec<Arc<Sample>>,

		pub preset_map: HashMap<(u8, u8), usize>,
	}

	#[repr(u8)]
	#[derive(Copy, Clone, Debug)]
	enum MidiCC {
		BankSelect = 0,
		ModWheel = 1,
		BreathControl = 2,
		FootPedal = 4,
		PortamentoTime = 5,
		DataEntry = 6,
		Volume = 7,
		Balance = 8,
		Pan = 10,
		Expression = 11,
		// todo: implement the rest
	}

	impl TryFrom<u8> for MidiCC {
		type Error = ();

		fn try_from(value: u8) -> Result<Self, Self::Error> {
			match value {
				0 => Ok(MidiCC::BankSelect),
				1 => Ok(MidiCC::ModWheel),
				2 => Ok(MidiCC::BreathControl),
				4 => Ok(MidiCC::FootPedal),
				5 => Ok(MidiCC::PortamentoTime),
				6 => Ok(MidiCC::DataEntry),
				7 => Ok(MidiCC::Volume),
				8 => Ok(MidiCC::Balance),
				10 => Ok(MidiCC::Pan),
				11 => Ok(MidiCC::Expression),
				_ => Err(())
			}
		}
	}

	impl InstVoice {
		pub fn release(&mut self, ctx: &mut RenderContext) {
			self.vol_env.release(self.vol_env_sustain(ctx));
			self.mod_env.release(self.mod_env_sustain());
		}

		fn vol_env_sustain(&self, ctx: &mut RenderContext) -> f64 {
			if ctx.overrides.sustain != -1 {
				if ctx.overrides.sustain == 0 {
					return 0.0
				} else {
					return 1.0 + (20.0 / 96.0) * (ctx.overrides.sustain as f64 / 127.0).ln()
				}
			}

			let sustain = self.gen_values[Gen::SustainVolEnv].sh_amount();

			1.0 - sustain.clamp(0, 960) as f64 / 960.0
		}

		fn mod_env_sustain(&self) -> f64 {
			let sustain = self.gen_values[Gen::SustainModEnv].sh_amount();

			1.0 - sustain.clamp(0, 1000) as f64 / 1000.0
		}

		pub fn update(&mut self, v: &Voice<impl HostData>, ctx: &mut RenderContext) {
			self.gen_values.reset();

			if let Some(i_global_zone) = &self.i_global_zone {
				for generator in &i_global_zone.gens {
					self.gen_values.apply_gen_list(generator, true);
				}
			}

			for generator in &self.i_zone.gens {
				self.gen_values.apply_gen_list(generator, true);
			}

			if let Some(p_global_zone) = &self.p_global_zone {
				for generator in &p_global_zone.gens {
					// this is a yucky `O(nm)` operation, but `n` and `m` should generally
					// be small enough to get away with it. the alternatives involve
					// allocating which feels worse
					if !self.p_zone.gens.contains(generator) {
						self.gen_values.apply_gen_list(generator, false);
					}
				}
			}

			for generator in &self.p_zone.gens {
				self.gen_values.apply_gen_list(generator, false);
			}

			// modulators
			// TODO: linked modulator support

			for modulator in &self.active_mods {
				let amt_val = ctx.get_mod_src_value(v, modulator.src_oper);
				let mod_amt_val = ctx.get_mod_src_value(v, modulator.mod_amt_src_oper);
				self.gen_values.apply_mod_list(&modulator, amt_val, mod_amt_val);
			}
		}

		pub fn render_mod_envelope(&mut self, samples: usize, sample_rate: u32, ctx: &mut RenderContext) {
			let del = self.gen_values[Gen::DelayModEnv].sh_amount();
			let atk = self.gen_values[Gen::AttackModEnv].sh_amount();
			let mut hold = self.gen_values[Gen::HoldModEnv].sh_amount();
			let mut dec = self.gen_values[Gen::DecayModEnv].sh_amount();
			let rel = self.gen_values[Gen::ReleaseModEnv].sh_amount();

			hold -= (self.gen_values[Gen::KeyNum].sh_amount() - 60) * self.gen_values[Gen::KeynumToModEnvHold].sh_amount();
			dec -= (self.gen_values[Gen::KeyNum].sh_amount() - 60) * self.gen_values[Gen::KeynumToModEnvDecay].sh_amount();

			let sus = self.mod_env_sustain();

			self.mod_env.process_coarse(
				del,
				atk,
				hold,
				dec,
				sus,
				rel,
				sample_rate,
				samples,
				ctx
			);
		}

		pub fn render_vol_envelope(&mut self, out: &mut [f32], sample_rate: u32, ctx: &mut RenderContext) {
			let del = self.gen_values[Gen::DelayVolEnv].sh_amount();
			let mut atk = self.gen_values[Gen::AttackVolEnv].sh_amount();
			let mut hold = self.gen_values[Gen::HoldVolEnv].sh_amount();
			let mut dec = self.gen_values[Gen::DecayVolEnv].sh_amount();
			let mut rel = self.gen_values[Gen::ReleaseVolEnv].sh_amount();

			hold -= (self.gen_values[Gen::KeyNum].sh_amount() - 60) * self.gen_values[Gen::KeynumToVolEnvHold].sh_amount();
			dec -= (self.gen_values[Gen::KeyNum].sh_amount() - 60) * self.gen_values[Gen::KeynumToVolEnvDecay].sh_amount();

			if ctx.overrides.attack != -1 {
				atk = millis_to_timecents(ctx.overrides.attack as f64).min(i16::MAX as f64) as i16;
			}

			if ctx.overrides.decay != -1 {
				dec = millis_to_timecents(ctx.overrides.decay as f64 * 4.0).min(i16::MAX as f64) as i16;

				if ctx.settings.tweak(EngineTweak::OverrideHoldWithDecay) {
					hold = -12000;
				}
			}

			if ctx.overrides.release != -1 {
				rel = millis_to_timecents(ctx.overrides.release as f64 * 4.0).min(i16::MAX as f64) as i16;
			}

			let sus = self.vol_env_sustain(ctx);
			let init_atten = (self.gen_values[Gen::InitialAttenuation].sh_amount() as f32).max(0.0);

			self.vol_env.process_fine(del, atk, hold, dec, sus, rel, sample_rate, out, ctx);

			// convert raw envelope value to dB gain
			for smp in out {
				if *smp == 0.0 {
					continue
				}

				let mut cb = -960.0 * (1.0 - *smp);

				cb -= init_atten * 0.4;

				*smp = centibels_to_gain(cb);
			}
		}

		pub fn render(
			&mut self,
			v: &Voice<impl HostData>,
			out: &mut [f32],
			env: &[f32],
			sample_rate: u32,
			method: ResampleMethod,
			ctx: &mut RenderContext
		) -> bool {
			let smpl = &*self.sample;

			// it aint the 90s anymore chum
			if smpl.is_rom() {
				return false
			}

			let sample_mode = self.gen_values[Gen::SampleModes].sh_amount();
			let not_looping = !Sample::should_loop(sample_mode, v.released);

			if not_looping && self.source_progress >= (smpl.get_end(&self.gen_values) - smpl.get_start(&self.gen_values) - 1) as f64 {
				self.vol_env.phase = EnvPhase::Stop;
				return false
			}

			let mod_env_val = self.mod_env.value(self.mod_env_sustain());
			let mod_lfo_val = self.mod_lfo.value();
			let vib_lfo_val = self.vib_lfo.value();

			// process LFOs

			let mut vib_amt = 0.0;

			let mut vib_lfo_to_pitch = self.gen_values[Gen::VibLfoToPitch].sh_amount() as f64 / 100.0;
			let mut vib_lfo_delay = timecents_to_secs(self.gen_values[Gen::DelayVibLFO].sh_amount() as f64);
			let mut vib_lfo_freq = 8.176 * cents_to_hz_mult(self.gen_values[Gen::FreqVibLFO].sh_amount() as f64);

			let mod_lfo_to_pitch = self.gen_values[Gen::ModLfoToPitch].sh_amount() as f64 / 100.0;
			let mod_lfo_delay = timecents_to_secs(self.gen_values[Gen::DelayModLFO].sh_amount() as f64);
			let mod_lfo_freq = 8.176 * cents_to_hz_mult(self.gen_values[Gen::FreqModLFO].sh_amount() as f64);

			let mod_env_to_pitch = self.gen_values[Gen::ModEnvToPitch].sh_amount() as f64 / 100.0;

			if ctx.overrides.lfo_2_del != -1 {
				vib_lfo_delay = ctx.overrides.lfo_2_del as f64 * 0.004;
			}

			if ctx.overrides.lfo_2_amt != -1 {
				vib_lfo_to_pitch = (ctx.overrides.lfo_2_amt - 129) as f64 / 12.7;
			}

			if ctx.overrides.lfo_2_spd != -1 {
				vib_lfo_freq = ctx.overrides.lfo_2_spd as f64 / 12.0;
			}

			vib_amt += vib_lfo_val * vib_lfo_to_pitch.abs();
			vib_amt += mod_lfo_val * mod_lfo_to_pitch.abs();
			vib_amt += mod_env_val * mod_env_to_pitch;

			// calculate pitch
			let mut root_key = smpl.original_pitch as f64;
			let scale_tuning = self.gen_values[Gen::ScaleTuning].sh_amount() as f64 / 100.0;

			if self.gen_values[Gen::OverridingRootKey].sh_amount() != -1 {
				root_key = self.gen_values[Gen::OverridingRootKey].sh_amount() as f64;
			}

			let mut tuning = self.gen_values[Gen::CoarseTune].sh_amount() as f64;

			tuning += self.gen_values[Gen::FineTune].sh_amount() as f64 / 100.0;
			tuning += smpl.pitch_correction as f64 / 100.0;

			let sample_ratio = smpl.sample_rate as f64 / sample_rate as f64;
			let key_offset = (v.init_pitch as f64 - root_key) * scale_tuning + v.pitch_offset as f64 + tuning + vib_amt;
			let progress_ratio = SEMITONE_RATIO.powf(key_offset) * sample_ratio;

			let delay = self.vol_env.delay_smpls as usize;

			self.vol_env.delay_smpls = (delay as i64 - out.len() as i64).max(0) as usize;

			if delay < out.len() {
				// bounds-checked access: malformed files must never read out of bounds
				let smpl_start = self.sf.sample_data.get(smpl.start as usize..).unwrap_or(&[]);

				let out = &mut out[delay..];
				let env = &env[delay..];

				// separate resample calls for 16-bit and 24-bit, so they Hopefully(tm)
				// get monomorphized and inlined into separate functions that don't
				// branch inside the extremely hot resampling loop

				if self.sf.sample_data_24.is_empty() {
					// regular 16-bit resampling

					smpl.resample(
						&mut self.source_progress,
						progress_ratio,
						method,
						|i| smpl_start.get(i).copied().unwrap_or(0) as f32 / 32767.0,
						out,
						v.released,
						&self.gen_values,
						env,
					);
				} else {
					// fancy 24-bit resampling
					let sm24_start = self.sf.sample_data_24.get(smpl.start as usize..).unwrap_or(&[]);

					smpl.resample(
						&mut self.source_progress,
						progress_ratio,
						method,
						|i| (((smpl_start.get(i).copied().unwrap_or(0) as i32) << 8) | sm24_start.get(i).copied().unwrap_or(0) as i32) as f32 / 8388607.0,
						out,
						v.released,
						&self.gen_values,
						env,
					);
				}

				if !ctx.overrides.filter_bypass {
					// apply lowpass filter

					let lp = &mut self.filter;
					let mut filter_q = self.gen_values[Gen::InitialFilterQ].sh_amount() as f64;
					let mut filter_fc = self.gen_values[Gen::InitialFilterFc].sh_amount() as f64;

					if ctx.settings.tweak(EngineTweak::FilterFix) {
						// fixed filter behaviour

						filter_fc += mod_env_val * self.gen_values[Gen::ModEnvToFilterFc].sh_amount() as f64;
						filter_fc += mod_lfo_val * self.gen_values[Gen::ModLfoToFilterFc].sh_amount() as f64;

						if ctx.overrides.filter_fc != -1 {
							let t = sf_curve(ctx.overrides.filter_fc as f64 / 127.0, ControllerType::Convex, true);
							filter_fc = t * 13500.0 + (1.0 - t) * 1500.0;
						}
					} else {
						// old filter behaviour (pre-0.9)
						
						filter_fc += sf_curve(mod_env_val, ControllerType::Concave, true) * self.gen_values[Gen::ModEnvToFilterFc].sh_amount() as f64;
						filter_fc += mod_lfo_val * self.gen_values[Gen::ModLfoToFilterFc].sh_amount() as f64;

						if ctx.overrides.filter_fc != -1 {
							filter_fc = ctx.overrides.filter_fc as f64 / 127.0 * 13500.0;
						}
					}

					let filter_fc_hz = 8.176 * cents_to_hz_mult(filter_fc);

					filter_q /= 10.0; // cB to dB
					filter_q -= 3.01; // adjust for flat freq response at Q == 0.0

					let linear_q = 10.0f64.powf(filter_q / 20.0);

					lp.set_params(filter_fc_hz, linear_q, sample_rate);
					
					if v.first_rendering {
						lp.reset();
					}

					for s in out.iter_mut() {
						lp.v0 = *s;

						lp.v3 = lp.v0 - lp.ic2eq;
						lp.v1 = lp.a1 * lp.ic1eq + lp.a2 * lp.v3;
						lp.v2 = lp.ic2eq + lp.a2 * lp.ic1eq + lp.a3 * lp.v3;

						lp.ic1eq = 2.0 * lp.v1 - lp.ic1eq;
						lp.ic2eq = 2.0 * lp.v2 - lp.ic2eq;

						if filter_fc < 13500.0 {
							*s = lp.v2;
						}
					}
				}

				// final gain
				let mut final_gain_db = 2.0;

				final_gain_db += mod_lfo_val * self.gen_values[Gen::ModLfoToVolume].sh_amount() as f64 / 10.0;

				let final_gain_linear = 10.0f64.powf(final_gain_db / 20.0) as f32;

				for smp in out.iter_mut() {
					*smp *= final_gain_linear;
				}

				self.vib_lfo.update(vib_lfo_delay, vib_lfo_freq, sample_rate, out.len());
				self.mod_lfo.update(mod_lfo_delay, mod_lfo_freq, sample_rate, out.len());
			}

			true
		}
	}

	impl SoundFont {
		pub fn from_sf2_data(sf: Sf2Data) -> Self {
			let mut result = Self {
				path: sf.path,
				name: sf.name,
				presets: vec![],
				instruments: vec![],
				sample_data: vec![],
				sample_data_24: sf.sm24,
				sample_headers: vec![],

				preset_map: HashMap::new(),
			};

			for i in 0..(sf.phdr.len() - 1) {
				let phdr = &sf.phdr[i];

				let mut zones = vec![];

				// fetch zones
				for j in phdr.preset_bag_idx..sf.phdr[i+1].preset_bag_idx {
					let j = j as usize;
					let pbag = &sf.pbag[j];

					let mut zone = Zone { gens: vec![], mods: vec![] };

					for k in pbag.gen_idx..sf.pbag[j+1].gen_idx {
						zone.gens.push(sf.pgen[k as usize]);
					}

					for k in pbag.mod_idx..sf.pbag[j+1].mod_idx {
						zone.mods.push(sf.pmod[k as usize]);
					}

					zones.push(Arc::new(zone));
				}

				if !result.preset_map.contains_key(&(phdr.bank as u8, phdr.preset as u8)) {
					result.preset_map.insert((phdr.bank as u8, phdr.preset as u8), i);
				}

				let end = phdr.name.iter().position(|i| *i == 0).unwrap_or(20);

				result.presets.push(Preset {
					name: String::from_utf8_lossy(&phdr.name[0..end]).into_owned(),

					preset: phdr.preset,
					bank: phdr.bank,
					zones,

					library: phdr.library,
					genre: phdr.genre,
					morphology: phdr.morphology
				});
			}

			for i in 0..(sf.inst.len() - 1) {
				let inst = &sf.inst[i];

				let mut zones = vec![];

				for j in inst.inst_bag_idx..sf.inst[i+1].inst_bag_idx {
					let j = j as usize;
					let ibag = &sf.ibag[j];

					let mut zone = Zone { gens: vec![], mods: vec![] };

					for k in ibag.gen_idx..sf.ibag[j+1].gen_idx {
						zone.gens.push(sf.igen[k as usize]);
					}

					for k in ibag.mod_idx..sf.ibag[j+1].mod_idx {
						zone.mods.push(sf.imod[k as usize]);
					}

					zones.push(Arc::new(zone));
				}

				let end = inst.name.iter().position(|i| *i == 0).unwrap_or(20);

				result.instruments.push(Instrument {
					name: String::from_utf8_lossy(&inst.name[0..end]).into_owned(),
					zones
				});
			}

			result.sample_data = sf.smpl;
			result.sample_headers = sf.shdr.into_iter().map(Arc::new).collect();

			result
		}


		pub fn get_preset_idx(&self, bank: u8, patch: u8) -> Option<usize> {
			self.preset_map.get(&(bank, patch)).copied()
		}

		pub fn spawn_dummy_voice<H: HostData>(host_data: H) -> Voice<H> {
			Voice {
				inst_voices: Some(vec![]),
				init_pitch: 0.0,
				pitch_offset: 0.0,
				first_rendering: true,
				released: false,
				has_active_voices: false,
				note_on_key_number: 60.0 / 127.0,
				note_on_velocity: 0.5,
				vel_offset: 0.0,
				host_data
			}
		}


		pub fn spawn_voice<H: HostData>(
			self: &Arc<Self>,
			host_data: H,
			preset_idx: usize,
			pitch: f32,
			vel: u8,
			ctx: &mut RenderContext
		)
			-> Voice<H>
		{
			let preset = &self.presets[preset_idx];

			let mut p_global_zone = None;

			let keynum = pitch as u8;

			let mut voice = Voice {
				init_pitch: pitch,
				pitch_offset: 0.0,
				first_rendering: true,
				host_data,
				has_active_voices: false,
				inst_voices: None,
				released: false,
				note_on_key_number: pitch / 127.0,
				note_on_velocity: vel as f32 / 127.0,
				vel_offset: 0.0,
			};

			let mut inst_voices = ctx.inst_voice_vec_pool.pop().unwrap_or(vec![]);

			// cap velocity for zone detection
			let vel = vel.min(127);

			for zone in &preset.zones {
				let Some(inst_gen) = zone.get_instrument_gen() else {
					if zone.is_active(keynum, vel) {
						p_global_zone = Some(zone.clone());
						continue
					} else {
						// if global zone is inactive, skip all zones
						break
					}
				};

				if !zone.is_active(keynum, vel) {
					continue
				}

				let mut i_global_zone = None;

				// damaged files can reference instruments or samples that don't exist
				let Some(inst) = self.instruments.get(inst_gen.amount.w_amount() as usize) else {
					continue
				};

				for izone in &inst.zones {
					let Some(sample_id_gen) = izone.get_sample_id_gen() else {
						if izone.is_active(keynum, vel) {
							i_global_zone = Some(izone.clone());
							continue
						} else {
							// if global zone is inactive, skip all zones
							break
						}
					};

					if !izone.is_active(keynum, vel) {
						continue
					}

					let Some(sample) = self.sample_headers.get(sample_id_gen.amount.w_amount() as usize) else {
						continue
					};

					let mut gen_values = GenValues::new();

					gen_values[Gen::KeyNum].w_amount = keynum as u16;
					gen_values[Gen::Velocity].w_amount = vel as u16;

					// determine active mods
					let mut active_mods = ctx.active_mods_pool.pop().unwrap_or(vec![]);
					let mods_filter = &mut ctx.mods_filter;

					mods_filter.clear();

					// add default modulators
					for modulator in DEFAULT_MODS {
						mods_filter.insert(*modulator);
					}

					if let Some(i_global_zone) = &i_global_zone {
						// override default mods with global instrument zone mods
						for modulator in &i_global_zone.mods {
							mods_filter.insert(*modulator);
						}
					}

					// override again with local instrument zone mods
					for modulator in &izone.mods {
						mods_filter.insert(*modulator);
					}

					// add instrument zone mods to active_mods list
					active_mods.extend(mods_filter.iter());
					mods_filter.clear();

					// now do the same for preset zone modulators
					if let Some(p_global_zone) = &p_global_zone {
						for modulator in &p_global_zone.mods {
							mods_filter.insert(*modulator);
						}
					}

					for modulator in &zone.mods {
						mods_filter.insert(*modulator);
					}

					active_mods.extend(mods_filter.iter());

					let mut iv = InstVoice {
						sf: self.clone(),
						sample: sample.clone(),
						p_global_zone: p_global_zone.clone(),
						i_global_zone: i_global_zone.clone(),
						p_zone: zone.clone(),
						i_zone: izone.clone(),
						gen_values,
						active_mods,
						filter: FilterState::default(),
						mod_env: EnvState::modulation(),
						vol_env: EnvState::volume(),
						mod_lfo: LfoState::default(),
						vib_lfo: LfoState::default(),
						source_progress: 0.0,
						last_vol: (0.0, 0.0)
					};

					iv.update(&voice, ctx);

					inst_voices.push(iv);
				}
			}

			voice.inst_voices = Some(inst_voices);

			voice
		}
	}
}