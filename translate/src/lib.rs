// SPDX-License-Identifier: MIT

mod a840_profile;

use std::borrow::Cow;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{c_char, CStr, CString};
use std::fmt::Display;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::sync::{Arc, OnceLock};

use metal2vulkan::device::{Device, FormatUsage};
use metal2vulkan::error::{
    ContainerError, DescriptorError, Error, InputErrorKind, Location, MetalAnswer,
};
use metal2vulkan::library::{Function, FunctionStage, Library};
use metal2vulkan::metal::resource_word::TextureWord;
use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::metal::sampler::SamplerWord;
use metal2vulkan::metal::{
    self, CompareFunction, DataType, PixelFormat, SamplerAddressMode, SamplerBorderColor,
    SamplerMinMagFilter, SamplerMipFilter, TextureSwizzleChannels, VertexFormat,
    VertexStepFunction,
};
use metal2vulkan::pipeline::{
    threadgroup_constants, BindingSource, ColorAttachmentDescriptor, ComputePipelineDescriptor,
    FunctionDescriptor, PipelineInterface, PipelineLayout, PushField, PushLoc, RenderPipeline,
    RenderPipelineDescriptor, SamplerRef, SlotClass, Stage, StageEntries, StageModule, StageSource,
    VertexAttributeDescriptor, VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan::{vk, Budget};

const FAT_MAGIC: [u8; 4] = [0xca, 0xfe, 0xba, 0xbe];
const FAT64_MAGIC: [u8; 4] = [0xca, 0xfe, 0xba, 0xbf];
const METAL_FAT_MAGIC: [u8; 4] = [0xcb, 0xfe, 0xba, 0xbe];
const MACH64_MAGIC: [u8; 4] = [0xcf, 0xfa, 0xed, 0xfe];
const AIR64: u32 = 0x0100_0017;
const APPLE_GPU: u32 = 0x0100_0013;
const FAT_HEADER_SIZE: usize = 8;
const FAT_ARCH_SIZE: usize = 20;
const MAX_FAT_ARCHES: u32 = 16;
const NMB_DEVICE_FLAG_ROBUST_OFF: u32 = 1 << 0;
const NMB_STAGE_VERTEX_BIT: u32 = 1 << 0;
const NMB_STAGE_FRAGMENT_BIT: u32 = 1 << 1;
const NMB_STAGE_COMPUTE_BIT: u32 = 1 << 2;
const NO_SLOT: u32 = u32::MAX;
const MAX_COLORS: usize = 8;
const MAX_VERTEX_ATTRIBUTES: usize = 31;
const MAX_VERTEX_LAYOUTS: usize = 31;
const MAX_VERTEX_INDEX: u32 = 31;

type FfiError = Cow<'static, CStr>;
type FfiResult<T> = Result<T, FfiError>;

thread_local! {
    static LAST_ERROR: RefCell<FfiError> = const { RefCell::new(Cow::Borrowed(c"")) };
}

fn set_error(error: FfiError) {
    let _ = LAST_ERROR.try_with(|slot| *slot.borrow_mut() = error);
}

fn message(error: impl Display) -> FfiError {
    match CString::new(error.to_string()) {
        Ok(string) => Cow::Owned(string),
        Err(_) => Cow::Borrowed(c"translator error message contains NUL"),
    }
}

fn catch_ffi<T: Copy>(failure: T, run: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(run)) {
        Ok(value) => value,
        Err(_) => {
            let _ = LAST_ERROR.try_with(|slot| {
                if let Ok(mut last) = slot.try_borrow_mut() {
                    *last = Cow::Borrowed(c"panic inside native Metal translator");
                }
            });
            failure
        }
    }
}

fn ffi<T: Copy>(failure: T, run: impl FnOnce() -> FfiResult<T>) -> T {
    catch_ffi(failure, || match run() {
        Ok(value) => {
            set_error(Cow::Borrowed(c""));
            value
        }
        Err(error) => {
            set_error(error);
            failure
        }
    })
}

fn be_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(bytes[at..at + 4].try_into().expect("validated fat record"))
}

fn le_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(
        bytes[at..at + 4]
            .try_into()
            .expect("validated archive record"),
    )
}

fn le_u64(bytes: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(
        bytes[at..at + 8]
            .try_into()
            .expect("validated archive record"),
    )
}

fn air_slice(bytes: &[u8]) -> Result<(usize, usize), &'static CStr> {
    if bytes.starts_with(b"MTLB") {
        return Ok((0, bytes.len()));
    }
    let record_size = if bytes.starts_with(&FAT64_MAGIC) {
        32
    } else if bytes.starts_with(&FAT_MAGIC) || bytes.starts_with(&METAL_FAT_MAGIC) {
        FAT_ARCH_SIZE
    } else {
        return Err(c"expected MTLB or Mach-O fat magic");
    };
    let header = bytes
        .get(..FAT_HEADER_SIZE)
        .ok_or(c"truncated Mach-O fat header")?;
    let n_arch = be_u32(header, 4);
    if !(1..=MAX_FAT_ARCHES).contains(&n_arch) {
        return Err(c"Mach-O fat architecture count must be in 1..=16");
    }

    let table_end = FAT_HEADER_SIZE + n_arch as usize * record_size;
    let table = bytes
        .get(FAT_HEADER_SIZE..table_end)
        .ok_or(c"truncated Mach-O fat architecture table")?;
    let mut air = None;
    for arch in table.chunks_exact(record_size) {
        let range_error = c"Mach-O fat slice range is out of bounds or empty";
        let (offset, size) = if record_size == FAT_ARCH_SIZE {
            (be_u32(arch, 8) as u64, be_u32(arch, 12) as u64)
        } else {
            let offset =
                u64::from_be_bytes(arch[8..16].try_into().expect("validated fat64 record"));
            let size = u64::from_be_bytes(arch[16..24].try_into().expect("validated fat64 record"));
            (offset, size)
        };
        let offset = usize::try_from(offset).map_err(|_| range_error)?;
        let size = usize::try_from(size).map_err(|_| range_error)?;
        if offset < table_end
            || size == 0
            || offset.checked_add(size).is_none_or(|end| end > bytes.len())
        {
            return Err(range_error);
        }
        if be_u32(arch, 0) == AIR64 && air.is_none() {
            air = Some((offset, size));
        }
    }
    air.ok_or(c"Mach-O fat container contains no AIR64 slice")
}

/// The native Apple GPU archive's AIR lives in its __TEXT,__metallib section.
fn archive_air_section(bytes: &[u8]) -> Result<&[u8], &'static CStr> {
    const MH_HEADER_SIZE: usize = 32;
    const LC_SEGMENT_64: u32 = 0x19;
    const SEGMENT_HEADER_SIZE: usize = 72;
    const SECTION_SIZE: usize = 80;
    const TRUNCATED: &CStr = c"truncated Apple GPU Mach-O archive";

    let header = bytes.get(..MH_HEADER_SIZE).ok_or(TRUNCATED)?;
    if le_u32(header, 4) != APPLE_GPU {
        return Err(c"Mach-O archive does not target Apple GPU");
    }
    let mut commands = bytes[MH_HEADER_SIZE..]
        .get(..le_u32(header, 20) as usize)
        .ok_or(TRUNCATED)?;
    for _ in 0..le_u32(header, 16) {
        let command_header = commands.get(..8).ok_or(TRUNCATED)?;
        let command_size = le_u32(command_header, 4) as usize;
        if command_size < 8 {
            return Err(c"invalid Apple GPU Mach-O load command size");
        }
        let (command, remaining) = commands.split_at_checked(command_size).ok_or(TRUNCATED)?;
        commands = remaining;
        if le_u32(command, 0) != LC_SEGMENT_64 {
            continue;
        }
        let segment = command.get(..SEGMENT_HEADER_SIZE).ok_or(TRUNCATED)?;
        let section_count = le_u32(segment, 64) as usize;
        let sections_size = section_count.checked_mul(SECTION_SIZE).ok_or(TRUNCATED)?;
        let sections = command[SEGMENT_HEADER_SIZE..]
            .get(..sections_size)
            .ok_or(TRUNCATED)?;
        for section in sections.as_chunks::<SECTION_SIZE>().0 {
            if &section[..16] == b"__metallib\0\0\0\0\0\0"
                && &section[16..32] == b"__TEXT\0\0\0\0\0\0\0\0\0\0"
            {
                let offset = le_u32(section, 48) as usize;
                let size = usize::try_from(le_u64(section, 40)).map_err(|_| TRUNCATED)?;
                return bytes
                    .get(offset..)
                    .and_then(|tail| tail.get(..size))
                    .ok_or(TRUNCATED);
            }
        }
    }
    Err(c"Apple GPU Mach-O archive has no __TEXT,__metallib section")
}

// Binary archives can include companion MTLBs with pre-specialized aliases
// (ALIS + SPCI in place of NAME). These are not selectable by the name-only
// ABI; only the translator's precise MissingTag at such an entry is skipped.
fn is_specialized_alias(record: &[u8], location: &Location) -> Option<bool> {
    let header = record.get(..88)?;
    let offset = usize::try_from(le_u64(header, 24)).ok()?;
    let size = usize::try_from(le_u64(header, 32)).ok()?;
    let count = le_u32(record.get(offset..)?.get(..4)?, 0);
    let mut entry = offset.checked_add(4)?;
    let end = entry.checked_add(size)?;
    for _ in 0..count {
        let bytes = record.get(entry..end)?;
        let size = le_u32(bytes.get(..4)?, 0) as usize;
        if size < 8 {
            return None;
        }
        let mut tags = bytes.get(4..size)?;
        if *location == Location::container(entry as u64) {
            let mut alias = false;
            let mut specialization = false;
            let mut name = false;
            while let Some(tag) = tags.get(..4) {
                match tag {
                    b"ENDT" => return Some(tags.len() == 4 && alias && specialization && !name),
                    b"ALIS" => alias = true,
                    b"SPCI" => specialization = true,
                    b"NAME" => name = true,
                    _ => {}
                }
                let length = u16::from_le_bytes(tags.get(4..6)?.try_into().ok()?) as usize;
                tags = tags.get(6 + length..)?;
            }
            return None;
        }
        entry = entry.checked_add(size)?;
    }
    Some(false)
}

fn archive_libraries(mut section: &[u8]) -> FfiResult<Vec<Library>> {
    let mut libraries = Vec::new();
    // Each embedded MTLB declares its own length and is padded to 16 bytes.
    while !section.is_empty() {
        let header = section.get(..24).ok_or(c"truncated embedded MTLB header")?;
        if !header.starts_with(b"MTLB") {
            return Err(c"expected embedded MTLB record".into());
        }
        let invalid_size = c"embedded MTLB size is invalid or truncated";
        let size = usize::try_from(le_u64(header, 16)).map_err(|_| invalid_size)?;
        if size < header.len() {
            return Err(invalid_size.into());
        }
        let (record, tail) = section.split_at_checked(size).ok_or(invalid_size)?;
        match Library::parse(record) {
            Ok(library) => libraries.push(library),
            Err(Error::InvalidInput(error))
                if error.kind == InputErrorKind::Container(ContainerError::MissingTag)
                    && is_specialized_alias(record, &error.at) == Some(true) => {}
            Err(error) => return Err(message(error)),
        }
        let padding = (16 - size % 16) % 16;
        let (padding, remaining) = tail
            .split_at_checked(padding.min(tail.len()))
            .ok_or(invalid_size)?;
        if padding.iter().any(|&byte| byte != 0) {
            return Err(c"nonzero embedded MTLB alignment padding".into());
        }
        section = remaining;
    }
    if libraries.is_empty() {
        return Err(c"Apple GPU Mach-O archive has no named AIR library members".into());
    }
    Ok(libraries)
}

pub struct NmbDevice {
    translator: Device,
    extended: OnceLock<Result<Device, Error>>,
    flags: u32,
}

impl NmbDevice {
    fn extended(&self) -> FfiResult<&Device> {
        self.extended
            .get_or_init(|| a840_profile::create(true))
            .as_ref()
            .map_err(message)
    }
}

pub struct NmbLibrary {
    libraries: Vec<Library>,
}

impl NmbLibrary {
    fn parse(bytes: &[u8]) -> FfiResult<Self> {
        let libraries = if bytes.starts_with(&MACH64_MAGIC) {
            archive_libraries(archive_air_section(bytes)?)?
        } else {
            let (offset, len) = air_slice(bytes)?;
            vec![Library::parse(&bytes[offset..offset + len]).map_err(message)?]
        };
        Ok(Self { libraries })
    }

    fn function_info(&self, index: usize) -> FfiResult<metal2vulkan::library::FunctionInfo<'_>> {
        self.libraries
            .iter()
            .flat_map(Library::functions)
            .nth(index)
            .ok_or_else(|| c"function index is out of range".into())
    }

    fn function(&self, name: &str, stage: FunctionStage) -> FfiResult<Function> {
        let library = self
            .libraries
            .iter()
            .find(|lib| has_function(lib, name, stage))
            .ok_or_else(|| message(format_args!("no {stage:?} function named {name:?}")))?;
        library.function(name).map_err(message)
    }

    fn render_functions(&self, vertex: &str, fragment: &str) -> FfiResult<(Function, Function)> {
        // Prefer a common archive member over duplicate specialized functions
        // from separate members.
        if let Some(library) = self.libraries.iter().find(|lib| {
            has_function(lib, vertex, FunctionStage::Vertex)
                && has_function(lib, fragment, FunctionStage::Fragment)
        }) {
            return Ok((
                library.function(vertex).map_err(message)?,
                library.function(fragment).map_err(message)?,
            ));
        }
        Ok((
            self.function(vertex, FunctionStage::Vertex)?,
            self.function(fragment, FunctionStage::Fragment)?,
        ))
    }
}

fn has_function(lib: &Library, name: &str, stage: FunctionStage) -> bool {
    lib.functions()
        .any(|info| info.name == name && info.stage == stage)
}

struct SpirvStage {
    words: Arc<[u32]>,
    entry_point: CString,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbSpirvBinding {
    descriptor_set: u32,
    binding: u32,
    descriptor_type: u32,
    descriptor_count: u32,
    stage_mask: u32,
    buffer_index: u32,
    texture_index: u32,
    sampler_index: u32,
}

const NMB_PUSH_BUFFER_ADDRESS: u32 = 1;
const NMB_PUSH_BUFFER_BOUNDS: u32 = 2;
const NMB_PUSH_ZERO_ADDRESS: u32 = 3;
const NMB_PUSH_TEXTURE_ID: u32 = 4;
const NMB_PUSH_SAMPLER_ID: u32 = 5;
const NMB_PUSH_DISPATCH_REGION: u32 = 6;
const NMB_PUSH_STAGE_IN_REGION: u32 = 7;
const NMB_PUSH_TEXTURE_LEVELS: u32 = 8;
const NMB_PUSH_TEXTURE_LEVEL_BASE: u32 = 9;
const NMB_PUSH_INTEGER_CHANNELS: u32 = 10;
const NMB_PUSH_ARGUMENT_TABLE_ADDRESS: u32 = 11;
const NMB_PUSH_RENDER_TARGET_ARRAY_LENGTH: u32 = 12;
const NMB_PUSH_VIEWPORT_COUNT: u32 = 13;
const NMB_PUSH_VERTICES_PER_PRIMITIVE: u32 = 14;
const NMB_PUSH_TAIL_WORD: u32 = 15;
const NMB_PUSH_ZERO_U32: u32 = 16;
const NMB_PUSH_ZERO_U64: u32 = 17;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbSpirvPushConstantRange {
    offset_bytes: u32,
    size_bytes: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbSpirvPushField {
    kind: u32,
    in_arg_table: u32,
    offset_bytes: u32,
    size_bytes: u32,
    stage_mask: u32,
    metal_slot: u32,
    extra: u32,
}

pub struct NmbSpirvResult {
    stages: Vec<SpirvStage>,
    bindings: Vec<NmbSpirvBinding>,
    push_constants: [NmbSpirvPushConstantRange; 2],
    push_fields: [Vec<NmbSpirvPushField>; 2],
    arg_table_bytes: [u32; 2],
}

impl NmbSpirvResult {
    fn new(
        stages: impl IntoIterator<Item = StageModule>,
        layout: PipelineLayout,
        interface: &PipelineInterface,
    ) -> FfiResult<Self> {
        let stages = stages
            .into_iter()
            .map(|stage| {
                Ok(SpirvStage {
                    words: stage.spirv,
                    entry_point: CString::new(stage.entry_point).map_err(message)?,
                })
            })
            .collect::<FfiResult<Vec<_>>>()?;
        let active_stages = if stages.len() == 1 {
            NMB_STAGE_COMPUTE_BIT
        } else {
            NMB_STAGE_VERTEX_BIT | NMB_STAGE_FRAGMENT_BIT
        };
        let mut bindings = Vec::new();
        for set in layout.sets {
            for binding in set.bindings {
                let mut reflected = NmbSpirvBinding {
                    descriptor_set: set.set,
                    binding: binding.binding,
                    descriptor_type: binding.ty.as_raw() as u32,
                    descriptor_count: binding.count,
                    stage_mask: active_stages
                        & (u32::from(binding.stages.contains(vk::ShaderStageFlags::VERTEX))
                            | (u32::from(binding.stages.contains(vk::ShaderStageFlags::FRAGMENT))
                                << 1)
                            | (u32::from(binding.stages.contains(vk::ShaderStageFlags::COMPUTE))
                                << 2)),
                    buffer_index: NO_SLOT,
                    texture_index: NO_SLOT,
                    sampler_index: NO_SLOT,
                };
                match binding.source {
                    BindingSource::Buffer(slot) => reflected.buffer_index = slot.index,
                    BindingSource::Texture(slot) | BindingSource::YCbCrPlanes(slot) => {
                        reflected.texture_index = slot.index;
                    }
                    BindingSource::Sampler(slot) => reflected.sampler_index = slot.index,
                    BindingSource::CombinedTextureSampler { texture, sampler } => {
                        reflected.texture_index = texture.index;
                        if let SamplerRef::Slot(index) = sampler.sampler {
                            reflected.sampler_index = index;
                        }
                    }
                    BindingSource::SeparateTexture { texture, .. } => {
                        reflected.texture_index = texture.index;
                    }
                    BindingSource::SeparateSampler { sampler, .. }
                    | BindingSource::HeapSampler { sampler, .. } => {
                        if let SamplerRef::Slot(index) = sampler.sampler {
                            reflected.sampler_index = index;
                        }
                    }
                    _ => {}
                }
                bindings.push(reflected);
            }
        }
        let stage_bits = if stages.len() == 1 {
            [vk::ShaderStageFlags::COMPUTE, vk::ShaderStageFlags::empty()]
        } else {
            [vk::ShaderStageFlags::VERTEX, vk::ShaderStageFlags::FRAGMENT]
        };
        let push_constants = stage_bits.map(|bit| {
            let mut ranges = layout
                .push_constants
                .iter()
                .filter(|r| r.stages.intersects(bit));
            ranges
                .next()
                .map_or_else(NmbSpirvPushConstantRange::default, |first| {
                    let (lo, hi) = ranges
                        .fold((first.offset, first.offset + first.size), |(lo, hi), r| {
                            (lo.min(r.offset), hi.max(r.offset + r.size))
                        });
                    NmbSpirvPushConstantRange {
                        offset_bytes: lo,
                        size_bytes: hi - lo,
                    }
                })
        });
        let is_compute = stages.len() == 1;
        let mut push_fields: [Vec<NmbSpirvPushField>; 2] = [Vec::new(), Vec::new()];
        let mut arg_table_bytes = [0u32; 2];
        for placement in &interface.push {
            let Some((stage_idx, stage_mask)) = (match (is_compute, placement.stage) {
                (true, Stage::Kernel) => Some((0usize, NMB_STAGE_COMPUTE_BIT)),
                (false, Stage::Vertex) => Some((0usize, NMB_STAGE_VERTEX_BIT)),
                (false, Stage::Fragment) => Some((1usize, NMB_STAGE_FRAGMENT_BIT)),
                _ => None,
            }) else {
                continue;
            };
            arg_table_bytes[stage_idx] = placement.argument_table_size;
            for placed in &placement.fields {
                let (in_arg_table, offset_bytes) = match placed.loc {
                    PushLoc::Push { offset } => (0u32, offset),
                    PushLoc::ArgumentTable { offset } => (1u32, offset),
                    _ => continue,
                };
                let (kind, metal_slot, extra) = match placed.field {
                    PushField::SlotGuestBase(slot) => match slot.class {
                        SlotClass::Buffer => (NMB_PUSH_BUFFER_ADDRESS, slot.index, 0u32),
                        SlotClass::Texture => (NMB_PUSH_TEXTURE_ID, slot.index, 0u32),
                        SlotClass::Sampler => (NMB_PUSH_SAMPLER_ID, slot.index, 0u32),
                    },
                    PushField::SlotBounds(slot) => (NMB_PUSH_BUFFER_BOUNDS, slot.index, 0u32),
                    PushField::ZeroAddress => (NMB_PUSH_ZERO_ADDRESS, NO_SLOT, 0u32),
                    PushField::DispatchRegion => (NMB_PUSH_DISPATCH_REGION, NO_SLOT, 0u32),
                    PushField::StageInRegion => (NMB_PUSH_STAGE_IN_REGION, NO_SLOT, 0u32),
                    PushField::TextureLevels(slot) => (NMB_PUSH_TEXTURE_LEVELS, slot.index, 0u32),
                    PushField::TextureLevelBase(slot) => {
                        (NMB_PUSH_TEXTURE_LEVEL_BASE, slot.index, 0u32)
                    }
                    PushField::TextureIntegerChannels(slot) => {
                        (NMB_PUSH_INTEGER_CHANNELS, slot.index, 0u32)
                    }
                    PushField::ArgumentTableAddress => {
                        (NMB_PUSH_ARGUMENT_TABLE_ADDRESS, NO_SLOT, 0u32)
                    }
                    PushField::RenderTargetArrayLength => {
                        (NMB_PUSH_RENDER_TARGET_ARRAY_LENGTH, NO_SLOT, 0u32)
                    }
                    PushField::ViewportCount => (NMB_PUSH_VIEWPORT_COUNT, NO_SLOT, 0u32),
                    PushField::VerticesPerPrimitive => {
                        (NMB_PUSH_VERTICES_PER_PRIMITIVE, NO_SLOT, 0u32)
                    }
                    PushField::TailWord => (NMB_PUSH_TAIL_WORD, NO_SLOT, 0u32),
                    _ => {
                        let fallback = if placed.size == 8 {
                            NMB_PUSH_ZERO_U64
                        } else {
                            NMB_PUSH_ZERO_U32
                        };
                        (fallback, NO_SLOT, 0u32)
                    }
                };
                push_fields[stage_idx].push(NmbSpirvPushField {
                    kind,
                    in_arg_table,
                    offset_bytes,
                    size_bytes: placed.size,
                    stage_mask,
                    metal_slot,
                    extra,
                });
            }
            if let Some(tail_offset) = placement.tail_word {
                push_fields[stage_idx].push(NmbSpirvPushField {
                    kind: NMB_PUSH_TAIL_WORD,
                    in_arg_table: 0,
                    offset_bytes: tail_offset,
                    size_bytes: 4,
                    stage_mask,
                    metal_slot: NO_SLOT,
                    extra: 0,
                });
            }
        }
        Ok(Self {
            stages,
            bindings,
            push_constants,
            push_fields,
            arg_table_bytes,
        })
    }

    fn stage(&self, index: u32) -> FfiResult<&SpirvStage> {
        self.stages
            .get(index as usize)
            .ok_or_else(|| c"stage index is out of range".into())
    }
}

unsafe fn read_bytes<'a>(bytes: *const u8, len: usize) -> FfiResult<&'a [u8]> {
    if bytes.is_null() || len == 0 {
        return Err(c"bytes must be non-null and length must be nonzero".into());
    }
    if len > isize::MAX as usize || bytes.addr().checked_add(len).is_none() {
        return Err(c"input length exceeds addressable slice size".into());
    }
    // The caller supplies the readable allocation for this call.
    Ok(unsafe { slice::from_raw_parts(bytes, len) })
}

unsafe fn name_arg<'a>(name: *const c_char) -> FfiResult<&'a str> {
    if name.is_null() {
        return Err(c"function name is required".into());
    }
    let name = unsafe { CStr::from_ptr(name) }.to_str().map_err(message)?;
    if name.is_empty() {
        return Err(c"function name is empty".into());
    }
    Ok(name)
}

unsafe fn borrowed<'a, T>(handle: *const T, error: &'static CStr) -> FfiResult<&'a T> {
    unsafe { handle.as_ref() }.ok_or_else(|| error.into())
}

unsafe fn destroy<T>(handle: *mut T) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

#[no_mangle]
pub extern "C" fn nmb_translate_abi_version() -> u32 {
    catch_ffi(0, || 2)
}

#[no_mangle]
pub extern "C" fn nmb_last_error() -> *const c_char {
    catch_ffi(c"translator error unavailable".as_ptr(), || {
        LAST_ERROR
            .try_with(|slot| slot.borrow().as_ptr())
            .unwrap_or(c"".as_ptr())
    })
}

/// # Safety
/// Input bytes and output slots must be valid for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn nmb_extract_air_slice(
    bytes: *const u8,
    len: usize,
    out_offset: *mut usize,
    out_len: *mut usize,
) -> bool {
    ffi(false, || {
        let bytes = unsafe { read_bytes(bytes, len) }?;
        if out_offset.is_null() || out_len.is_null() || out_offset == out_len {
            return Err(c"output pointers must be non-null and distinct".into());
        }
        let (offset, size) = air_slice(bytes)?;
        unsafe {
            out_offset.write(offset);
            out_len.write(size);
        }
        Ok(true)
    })
}

#[no_mangle]
pub extern "C" fn nmb_device_create_a840(flags: u32) -> *mut NmbDevice {
    ffi(ptr::null_mut(), || {
        if flags & !NMB_DEVICE_FLAG_ROBUST_OFF != 0 {
            return Err(c"unknown A840 device flags".into());
        }
        let translator = a840_profile::create(false).map_err(message)?;
        Ok(Box::into_raw(Box::new(NmbDevice {
            translator,
            extended: OnceLock::new(),
            flags,
        })))
    })
}

/// # Safety
/// A non-null device handle must be live and owned by the caller.
#[no_mangle]
pub unsafe extern "C" fn nmb_device_destroy(dev: *mut NmbDevice) {
    ffi((), || {
        unsafe { destroy(dev) };
        Ok(())
    });
}

/// # Safety
/// A non-null device handle must stay live during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_device_flags(dev: *const NmbDevice) -> u32 {
    ffi(0, || {
        Ok(unsafe { borrowed(dev, c"device is required") }?.flags)
    })
}

/// # Safety
/// The device handle and input byte range must stay valid during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_parse(
    dev: *const NmbDevice,
    bytes: *const u8,
    len: usize,
) -> *mut NmbLibrary {
    ffi(ptr::null_mut(), || {
        unsafe { borrowed(dev, c"device is required") }?;
        let bytes = unsafe { read_bytes(bytes, len) }?;
        Ok(Box::into_raw(Box::new(NmbLibrary::parse(bytes)?)))
    })
}

/// # Safety
/// A non-null library handle must be live and owned by the caller.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_destroy(lib: *mut NmbLibrary) {
    ffi((), || {
        unsafe { destroy(lib) };
        Ok(())
    });
}

/// # Safety
/// A non-null library handle must stay live during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_function_count(lib: *const NmbLibrary) -> usize {
    ffi(0, || {
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        Ok(lib.libraries.iter().map(|m| m.functions().count()).sum())
    })
}

/// # Safety
/// The library handle must stay live and out_buf must be writable for out_cap bytes.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_function_name(
    lib: *const NmbLibrary,
    index: usize,
    out_buf: *mut c_char,
    out_cap: usize,
) -> bool {
    ffi(false, || {
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        let info = lib.function_info(index)?;
        let name = CString::new(info.name).map_err(message)?;
        let name = name.as_bytes_with_nul();
        if out_buf.is_null() || out_cap < name.len() {
            return Err(c"function name output buffer is too small or null".into());
        }
        unsafe { ptr::copy_nonoverlapping(name.as_ptr(), out_buf.cast(), name.len()) };
        Ok(true)
    })
}

/// # Safety
/// The handles and NUL-terminated function name must stay valid during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_compile_compute_spirv(
    dev: *const NmbDevice,
    lib: *const NmbLibrary,
    fn_name: *const c_char,
) -> *mut NmbSpirvResult {
    ffi(ptr::null_mut(), || {
        let dev = unsafe { borrowed(dev, c"device is required") }?;
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        let name = unsafe { name_arg(fn_name) }?;
        let func = lib.function(name, FunctionStage::Kernel)?;
        let desc =
            ComputePipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(func)));
        let pipeline = dev
            .translator
            .compile_compute_pipeline(&desc, &Budget::default())
            .map_err(message)?;
        let result = NmbSpirvResult::new([pipeline.stage], pipeline.layout, &pipeline.interface)?;
        Ok(Box::into_raw(Box::new(result)))
    })
}

/// # Safety
/// The handles and NUL-terminated function names must stay valid during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_compile_render_spirv(
    dev: *const NmbDevice,
    lib: *const NmbLibrary,
    vertex_fn: *const c_char,
    fragment_fn: *const c_char,
    color0_mtl_format: u32,
) -> *mut NmbSpirvResult {
    ffi(ptr::null_mut(), || {
        let dev = unsafe { borrowed(dev, c"device is required") }?;
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        let vertex_name = unsafe { name_arg(vertex_fn) }?;
        let fragment_name = unsafe { name_arg(fragment_fn) }?;
        let format = PixelFormat::try_from(color0_mtl_format).map_err(message)?;
        if format == PixelFormat::Invalid {
            return Err(c"color attachment 0 requires a Metal pixel format".into());
        }
        let (vertex, fragment) = lib.render_functions(vertex_name, fragment_name)?;
        let mut desc =
            RenderPipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(vertex)));
        desc.fragment = Some(StageSource::Function(FunctionDescriptor::new(fragment)));
        desc.color_attachments[0] = Some(ColorAttachmentDescriptor::new(format));
        let pipeline = dev
            .translator
            .compile_render_pipeline(&desc, &Budget::default())
            .map_err(message)?;
        Ok(Box::into_raw(Box::new(render_result(pipeline)?)))
    })
}

/// # Safety
/// The library and writable output must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_function_stage_kind(
    lib: *const NmbLibrary,
    index: usize,
    out_kind: *mut u32,
) -> bool {
    ffi(false, || {
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        if out_kind.is_null() {
            return Err(c"stage kind output is required".into());
        }
        let kind = match lib.function_info(index)?.stage {
            FunctionStage::Vertex => 0,
            FunctionStage::Fragment => 1,
            FunctionStage::Kernel => 2,
            FunctionStage::Exported => 3,
            FunctionStage::Visible => 4,
            FunctionStage::Extern => 5,
            FunctionStage::Intersection => 6,
            FunctionStage::Mesh => 7,
            FunctionStage::Object => 8,
            FunctionStage::Other(raw) => u32::from(raw),
            _ => return Err(c"unknown function stage kind".into()),
        };
        unsafe { out_kind.write(kind) };
        Ok(true)
    })
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbVertexAttribute {
    attribute_index: u32,
    buffer_index: u32,
    offset: u32,
    mtl_vertex_format: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbVertexLayout {
    buffer_index: u32,
    stride: u32,
    step_function: u32,
    step_rate: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct NmbRenderPipelineDesc {
    color_attachment_count: u32,
    color_mtl_formats: [u32; MAX_COLORS],
    depth_mtl_format: u32,
    stencil_mtl_format: u32,
    sample_count: u32,
    vertex_attribute_count: u32,
    vertex_attributes: [NmbVertexAttribute; MAX_VERTEX_ATTRIBUTES],
    vertex_layout_count: u32,
    vertex_layouts: [NmbVertexLayout; MAX_VERTEX_LAYOUTS],
}

fn render_descriptor(vertex: Function, fragment: Function) -> RenderPipelineDescriptor {
    let mut desc =
        RenderPipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(vertex)));
    desc.fragment = Some(StageSource::Function(FunctionDescriptor::new(fragment)));
    desc
}

fn render_result(pipeline: RenderPipeline) -> FfiResult<NmbSpirvResult> {
    if !matches!(&pipeline.stages[..], [vertex, fragment]
        if vertex.stage == vk::ShaderStage::VERTEX && fragment.stage == vk::ShaderStage::FRAGMENT)
    {
        return Err(c"render pipeline must emit one vertex and one fragment stage".into());
    }
    NmbSpirvResult::new(pipeline.stages, pipeline.layout, &pipeline.interface)
}

fn attachment_format(raw: u32) -> FfiResult<Option<PixelFormat>> {
    let pixel = PixelFormat::try_from(raw).map_err(message)?;
    Ok((pixel != PixelFormat::Invalid).then_some(pixel))
}

fn set_render_desc(
    translator: &Device,
    desc: &mut RenderPipelineDescriptor,
    input: &NmbRenderPipelineDesc,
) -> FfiResult<()> {
    let colors = input
        .color_mtl_formats
        .get(..input.color_attachment_count as usize)
        .ok_or(c"color_attachment_count exceeds 8")?;
    if !matches!(input.sample_count, 1 | 2 | 4) {
        return Err(c"sample_count must be 1, 2, or 4".into());
    }
    desc.raster_sample_count = input.sample_count;
    for (attachment, &raw) in desc.color_attachments.iter_mut().zip(colors) {
        *attachment = attachment_format(raw)?.map(ColorAttachmentDescriptor::new);
    }
    desc.depth_format = attachment_format(input.depth_mtl_format)?;
    desc.stencil_format = attachment_format(input.stencil_mtl_format)?;
    for (format, aspect) in [
        (desc.depth_format, vk::ImageAspectFlags::DEPTH),
        (desc.stencil_format, vk::ImageAspectFlags::STENCIL),
    ] {
        if let Some(format) = format {
            let mapping = metal::format::vk_format(
                format,
                &[FormatUsage::DepthStencilAttachment].into(),
                translator.profile(),
            )
            .map_err(message)?;
            if !mapping.aspects.contains(aspect) {
                return Err(c"pixel format lacks the requested depth/stencil aspect".into());
            }
        }
    }

    let attrs = input
        .vertex_attributes
        .get(..input.vertex_attribute_count as usize)
        .ok_or(c"vertex_attribute_count exceeds 31")?;
    let layouts = input
        .vertex_layouts
        .get(..input.vertex_layout_count as usize)
        .ok_or(c"vertex_layout_count exceeds 31")?;
    if attrs.is_empty() && layouts.is_empty() {
        return Ok(());
    }
    let mut vertex = VertexDescriptor::new();
    for &a in attrs {
        if a.attribute_index > MAX_VERTEX_INDEX || a.buffer_index > MAX_VERTEX_INDEX {
            return Err(c"vertex attribute or buffer index exceeds the A840 limit".into());
        }
        let format = VertexFormat::try_from(a.mtl_vertex_format).map_err(message)?;
        if vertex
            .attributes
            .insert(
                a.attribute_index,
                VertexAttributeDescriptor::new(format, a.offset, a.buffer_index),
            )
            .is_some()
        {
            return Err(c"duplicate vertex attribute index".into());
        }
    }
    for &l in layouts {
        if l.buffer_index > MAX_VERTEX_INDEX {
            return Err(c"vertex layout buffer index exceeds the A840 limit".into());
        }
        let mut layout =
            VertexBufferLayoutDescriptor::new((l.stride != u32::MAX).then_some(l.stride));
        layout.step_function = VertexStepFunction::try_from(l.step_function).map_err(message)?;
        layout.step_rate = l.step_rate;
        if vertex.layouts.insert(l.buffer_index, layout).is_some() {
            return Err(c"duplicate vertex layout buffer index".into());
        }
    }
    desc.vertex_descriptor = Some(vertex);
    Ok(())
}

fn default_vertex_format(ty: DataType) -> FfiResult<VertexFormat> {
    use DataType as D;
    use VertexFormat as V;

    Ok(match ty {
        D::Float => V::Float,
        D::Float2 => V::Float2,
        D::Float3 => V::Float3,
        D::Float4 => V::Float4,
        D::Half => V::Half,
        D::Half2 => V::Half2,
        D::Half3 => V::Half3,
        D::Half4 => V::Half4,
        D::Int => V::Int,
        D::Int2 => V::Int2,
        D::Int3 => V::Int3,
        D::Int4 => V::Int4,
        D::UInt => V::UInt,
        D::UInt2 => V::UInt2,
        D::UInt3 => V::UInt3,
        D::UInt4 => V::UInt4,
        D::Short => V::Short,
        D::Short2 => V::Short2,
        D::Short3 => V::Short3,
        D::Short4 => V::Short4,
        D::UShort => V::UShort,
        D::UShort2 => V::UShort2,
        D::UShort3 => V::UShort3,
        D::UShort4 => V::UShort4,
        D::Char => V::Char,
        D::Char2 => V::Char2,
        D::Char3 => V::Char3,
        D::Char4 => V::Char4,
        D::UChar => V::UChar,
        D::UChar2 => V::UChar2,
        D::UChar3 => V::UChar3,
        D::UChar4 => V::UChar4,
        _ => {
            return Err(message(format_args!(
                "unsupported reflected vertex attribute type {ty:?}"
            )))
        }
    })
}

fn default_vertex_desc(
    vertex: &Function,
    input: &mut NmbRenderPipelineDesc,
    budget: &Budget,
) -> FfiResult<()> {
    let props = vertex.properties(budget).map_err(message)?;
    let mut attrs = props
        .vertex_attributes()
        .unwrap_or_default()
        .iter()
        .collect::<Vec<_>>();
    attrs.sort_by_key(|a| a.attribute_index());
    if attrs.len() > MAX_VERTEX_ATTRIBUTES {
        return Err(c"too many reflected vertex attributes".into());
    }
    let mut stride = 0_u32;
    let mut alignment = 1;
    for (attr, slot) in attrs.iter().zip(&mut input.vertex_attributes) {
        let format = default_vertex_format(attr.attribute_type())?;
        let align = metal::vertex::vertex_alignment(format).map_err(message)?;
        let block = metal::vertex::vk_vertex_format(format)
            .map_err(message)?
            .block()
            .ok_or(c"reflected vertex format has no size")?;
        alignment = alignment.max(align);
        stride = stride.next_multiple_of(align);
        *slot = NmbVertexAttribute {
            attribute_index: attr.attribute_index(),
            buffer_index: 0,
            offset: stride,
            mtl_vertex_format: format.as_raw(),
        };
        stride += block.size;
    }
    if !attrs.is_empty() {
        input.vertex_attribute_count = attrs.len() as u32;
        input.vertex_layout_count = 1;
        input.vertex_layouts[0] = NmbVertexLayout {
            buffer_index: 0,
            stride: stride.next_multiple_of(alignment),
            step_function: VertexStepFunction::PerVertex.as_raw(),
            step_rate: 1,
        };
    }
    Ok(())
}

fn default_render_desc(
    translator: &Device,
    vertex: Function,
    fragment: Function,
) -> FfiResult<NmbRenderPipelineDesc> {
    use PixelFormat as P;
    // Metal's format is a pipeline property. Try valid float, uint and sint
    // formats in descending channel counts, refining only the reported slot.
    const PROBE_FORMATS: [PixelFormat; 9] = [
        P::RGBA8Unorm,
        P::RG16Float,
        P::R16Float,
        P::RGBA32Uint,
        P::RG32Uint,
        P::R32Uint,
        P::RGBA32Sint,
        P::RG32Sint,
        P::R32Sint,
    ];
    let budget = Budget::default();
    let mut input = NmbRenderPipelineDesc {
        sample_count: 1,
        ..Default::default()
    };
    default_vertex_desc(&vertex, &mut input, &budget)?;
    let mut desc = render_descriptor(vertex, fragment);
    set_render_desc(translator, &mut desc, &input)?;
    if desc
        .fragment_writes_depth_unattached(&budget, &StageEntries::new())
        .map_err(message)?
    {
        desc.depth_format = Some(P::Depth32Float);
        input.depth_mtl_format = P::Depth32Float.as_raw();
    }
    let mut choice = [0; MAX_COLORS];
    desc.color_attachments
        .fill(Some(ColorAttachmentDescriptor::new(PROBE_FORMATS[0])));
    let graphics = loop {
        match translator.compile_render_pipeline(&desc, &budget) {
            Ok(pipeline) => break pipeline.graphics,
            Err(
                error @ Error::InvalidDescriptor(DescriptorError::ColorAttachment {
                    index,
                    metal: MetalAnswer::NilWithError,
                }),
            ) => {
                let slot = index as usize;
                let current = choice.get_mut(slot).ok_or_else(|| message(&error))?;
                *current += 1;
                let format = *PROBE_FORMATS.get(*current).ok_or_else(|| message(&error))?;
                desc.color_attachments[slot] = Some(ColorAttachmentDescriptor::new(format));
            }
            Err(error) => return Err(message(error)),
        }
    };

    let full = desc.color_attachments;
    for (slot, attachment) in desc.color_attachments.iter_mut().enumerate() {
        let written = graphics
            .color_attachments
            .get(slot)
            .and_then(Option::as_ref)
            .is_some_and(|color| !color.write_mask.is_empty());
        let read = graphics
            .input_attachments
            .iter()
            .any(|a| a.color_index == slot as u32);
        if !written && !read {
            *attachment = None;
        }
    }
    // Even a dead [[color(n)]] parameter requires a bound attachment in Metal.
    loop {
        match translator.render_pipeline_key(&desc, &budget) {
            Ok(_) => break,
            Err(Error::InvalidDescriptor(DescriptorError::ColorAttachment {
                index,
                metal: MetalAnswer::NilWithError,
            })) if desc
                .color_attachments
                .get(index as usize)
                .is_some_and(Option::is_none) =>
            {
                desc.color_attachments[index as usize] = full[index as usize];
            }
            Err(error) => return Err(message(error)),
        }
    }
    for (index, attachment) in desc.color_attachments.iter().enumerate() {
        if let Some(attachment) = attachment {
            input.color_attachment_count = index as u32 + 1;
            input.color_mtl_formats[index] = attachment.pixel_format.as_raw();
        }
    }
    Ok(input)
}

/// # Safety
/// Handles, names and writable output must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_library_default_render_desc(
    dev: *const NmbDevice,
    lib: *const NmbLibrary,
    vertex_fn: *const c_char,
    fragment_fn: *const c_char,
    out: *mut NmbRenderPipelineDesc,
) -> bool {
    ffi(false, || {
        let dev = unsafe { borrowed(dev, c"device is required") }?;
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        if out.is_null() {
            return Err(c"render descriptor output is required".into());
        }
        let (vertex, fragment) = lib.render_functions(unsafe { name_arg(vertex_fn) }?, unsafe {
            name_arg(fragment_fn)
        }?)?;
        let input = default_render_desc(dev.extended()?, vertex, fragment)?;
        unsafe { out.write(input) };
        Ok(true)
    })
}

/// # Safety
/// Handles, names and descriptor must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_compile_render_spirv_ex(
    dev: *const NmbDevice,
    lib: *const NmbLibrary,
    vertex_fn: *const c_char,
    fragment_fn: *const c_char,
    input: *const NmbRenderPipelineDesc,
) -> *mut NmbSpirvResult {
    ffi(ptr::null_mut(), || {
        let dev = unsafe { borrowed(dev, c"device is required") }?;
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        let input = unsafe { borrowed(input, c"render descriptor is required") }?;
        let (vertex, fragment) = lib.render_functions(unsafe { name_arg(vertex_fn) }?, unsafe {
            name_arg(fragment_fn)
        }?)?;
        let translator = dev.extended()?;
        let mut desc = render_descriptor(vertex, fragment);
        set_render_desc(translator, &mut desc, input)?;
        let budget = Budget::default();
        if desc
            .fragment_writes_depth_unattached(&budget, &StageEntries::new())
            .map_err(message)?
        {
            return Err(c"fragment writes depth but the depth attachment is unbound".into());
        }
        let pipeline = translator
            .compile_render_pipeline(&desc, &budget)
            .map_err(message)?;
        let mut result = render_result(pipeline)?;
        result.pack_bindless()?;
        Ok(Box::into_raw(Box::new(result)))
    })
}

/// # Safety
/// The result handle and writable output must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_push_constant_range(
    res: *const NmbSpirvResult,
    index: u32,
    out: *mut NmbSpirvPushConstantRange,
) -> bool {
    ffi(false, || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        res.stage(index)?;
        if out.is_null() {
            return Err(c"push constant range output is required".into());
        }
        unsafe { out.write(res.push_constants[index as usize]) };
        Ok(true)
    })
}

/// # Safety
/// The result handle must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_push_field_count(
    res: *const NmbSpirvResult,
    stage_index: u32,
) -> usize {
    ffi(0, || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        res.stage(stage_index)?;
        Ok(res.push_fields[stage_index as usize].len())
    })
}

/// # Safety
/// The result handle and writable output must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_push_field(
    res: *const NmbSpirvResult,
    stage_index: u32,
    index: usize,
    out: *mut NmbSpirvPushField,
) -> bool {
    ffi(false, || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        res.stage(stage_index)?;
        if out.is_null() {
            return Err(c"push field output is required".into());
        }
        let field = res.push_fields[stage_index as usize]
            .get(index)
            .copied()
            .ok_or_else(|| c"push field index is out of range")?;
        unsafe { out.write(field) };
        Ok(true)
    })
}

/// # Safety
/// The result handle must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_arg_table_bytes(
    res: *const NmbSpirvResult,
    stage_index: u32,
) -> u32 {
    ffi(0, || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        res.stage(stage_index)?;
        Ok(res.arg_table_bytes[stage_index as usize])
    })
}

#[no_mangle]
pub extern "C" fn nmb_encode_sampler_word(
    normalized_coordinates: bool,
    min_filter: u32,
    mag_filter: u32,
    mip_filter: u32,
    s_address_mode: u32,
    t_address_mode: u32,
    r_address_mode: u32,
    border_color: u32,
    compare_function: u32,
) -> u32 {
    catch_ffi(0, || {
        let mut desc = SamplerDescriptor::default();
        desc.normalized_coordinates = normalized_coordinates;
        if let Ok(v) = SamplerMinMagFilter::try_from(min_filter) {
            desc.min_filter = v;
        }
        if let Ok(v) = SamplerMinMagFilter::try_from(mag_filter) {
            desc.mag_filter = v;
        }
        if let Ok(v) = SamplerMipFilter::try_from(mip_filter) {
            desc.mip_filter = v;
        }
        if let Ok(v) = SamplerAddressMode::try_from(s_address_mode) {
            desc.s_address_mode = v;
        }
        if let Ok(v) = SamplerAddressMode::try_from(t_address_mode) {
            desc.t_address_mode = v;
        }
        if let Ok(v) = SamplerAddressMode::try_from(r_address_mode) {
            desc.r_address_mode = v;
        }
        if let Ok(v) = SamplerBorderColor::try_from(border_color) {
            desc.border_color = v;
        }
        if let Ok(v) = CompareFunction::try_from(compare_function) {
            desc.compare_function = v;
        }
        SamplerWord::of(&desc).0
    })
}

#[no_mangle]
pub extern "C" fn nmb_encode_texture_word(mtl_pixel_format: u32) -> u32 {
    catch_ffi(0, || {
        let Ok(pf) = PixelFormat::try_from(mtl_pixel_format) else {
            return 0;
        };
        TextureWord::of(pf, TextureSwizzleChannels::DEFAULT)
            .map(|w| w.0)
            .unwrap_or(0)
    })
}

// SPIR-V instructions used by the extended AOT projection. Values are from
// the SPIR-V grammar in the pinned Mesa src/compiler/spirv/spirv.h.
const SPV_OP_ENTRY_POINT: u32 = 15;
const SPV_OP_TYPE_VOID: u32 = 19;
const SPV_OP_TYPE_BOOL: u32 = 20;
const SPV_OP_TYPE_INT: u32 = 21;
const SPV_OP_TYPE_POINTER: u32 = 32;
const SPV_OP_TYPE_FORWARD_POINTER: u32 = 39;
const SPV_OP_CONSTANT: u32 = 43;
const SPV_OP_CONSTANT_NULL: u32 = 46;
const SPV_OP_SPEC_CONSTANT_TRUE: u32 = 48;
const SPV_OP_SPEC_CONSTANT_FALSE: u32 = 49;
const SPV_OP_SPEC_CONSTANT: u32 = 50;
const SPV_OP_FUNCTION: u32 = 54;
const SPV_OP_VARIABLE: u32 = 59;
const SPV_OP_LOAD: u32 = 61;
const SPV_OP_STORE: u32 = 62;
const SPV_OP_DECORATE: u32 = 71;
const SPV_OP_I_EQUAL: u32 = 170;
const SPV_OP_CONTROL_BARRIER: u32 = 224;
const SPV_OP_PHI: u32 = 245;
const SPV_OP_SELECTION_MERGE: u32 = 247;
const SPV_OP_LABEL: u32 = 248;
const SPV_OP_BRANCH: u32 = 249;
const SPV_OP_BRANCH_CONDITIONAL: u32 = 250;
const SPV_DEC_SPEC_ID: u32 = 1;
const SPV_DEC_BUILT_IN: u32 = 11;
const SPV_DEC_BINDING: u32 = 33;
const SPV_DEC_DESCRIPTOR_SET: u32 = 34;
const SPV_BUILT_IN_LOCAL_INVOCATION_INDEX: u32 = 29;
const SPV_STORAGE_INPUT: u32 = 1;
const SPV_STORAGE_WORKGROUP: u32 = 4;
const SPV_SCOPE_WORKGROUP: u32 = 2;
const SPV_ACQUIRE_RELEASE_WORKGROUP_MEMORY: u32 = 0x108;
const SPV_INVALID: &CStr = c"invalid SPIR-V from the translator";
const DESCRIPTOR_SETS: usize = 8;
const MAX_DESCRIPTORS_PER_SET: u32 = 65_536;
// The frozen compiler reports its bindful driver UBOs as set 0, blocks 1..=4,
// without a bindful flag. Keep those numbers disjoint from the AOT reflection.
const FIRST_USER_SLOT_SET_ZERO: u32 = 5;

fn spirv_instructions(words: &[u32]) -> FfiResult<Vec<std::ops::Range<usize>>> {
    if words.len() < 5 || words[0] != 0x0723_0203 {
        return Err(SPV_INVALID.into());
    }
    let mut instructions = Vec::new();
    let mut start = 5;
    while start < words.len() {
        let len = (words[start] >> 16) as usize;
        let end = start + len;
        if len == 0 || end > words.len() {
            return Err(SPV_INVALID.into());
        }
        instructions.push(start..end);
        start = end;
    }
    Ok(instructions)
}

fn write_instruction(words: &mut Vec<u32>, opcode: u32, args: &[u32]) {
    words.push(((args.len() as u32 + 1) << 16) | opcode);
    words.extend_from_slice(args);
}

type DescriptorLocations = BTreeMap<(u32, u32), (u32, u32)>;

fn rewrite_descriptor_slots(words: &mut [u32], locations: &DescriptorLocations) -> FfiResult<()> {
    let mut decorations = BTreeMap::<u32, (Option<usize>, Option<usize>)>::new();
    for range in spirv_instructions(words)? {
        if let [op, id, decoration, _] = words[range.clone()] {
            if op & 0xffff == SPV_OP_DECORATE {
                match decoration {
                    SPV_DEC_DESCRIPTOR_SET => {
                        decorations.entry(id).or_default().0 = Some(range.start + 3)
                    }
                    SPV_DEC_BINDING => decorations.entry(id).or_default().1 = Some(range.start + 3),
                    _ => {}
                }
            }
        }
    }
    for (set, binding) in decorations.into_values() {
        let (set, binding) = set.zip(binding).ok_or(SPV_INVALID)?;
        let &(new_set, new_binding) = locations
            .get(&(words[set], words[binding]))
            .ok_or(c"SPIR-V descriptor has no reflected binding")?;
        words[set] = new_set;
        words[binding] = new_binding;
    }
    Ok(())
}

impl NmbSpirvResult {
    fn pack_bindless(&mut self) -> FfiResult<()> {
        // Frozen IR3 indexes the hardware table at binding+i (texture/sampler
        // pairs at binding+2*i and binding+2*i+1). Reserve the entire span
        // before rewriting both decorations and reflection to that numbering.
        self.bindings.sort_by_key(|b| (b.descriptor_set, b.binding));
        let mut used_sets = [false; DESCRIPTOR_SETS];
        used_sets[0] = true;
        for binding in &self.bindings {
            *used_sets
                .get_mut(binding.descriptor_set as usize)
                .ok_or(c"unsupported: too many A840 descriptor sets")? = true;
        }
        let mut cursors = [0; DESCRIPTOR_SETS];
        cursors[0] = FIRST_USER_SLOT_SET_ZERO;
        let mut locations = DescriptorLocations::new();
        let mut spilling = None;
        let mut original_set = None;
        for binding in &mut self.bindings {
            let original = (binding.descriptor_set, binding.binding);
            let next_set = original.0 as usize;
            if original_set != Some(next_set) {
                spilling = None;
                original_set = Some(next_set);
            }
            let stride = if binding.descriptor_type
                == vk::DescriptorType::COMBINED_IMAGE_SAMPLER.as_raw() as u32
            {
                2
            } else {
                1
            };
            let count = binding
                .descriptor_count
                .checked_mul(stride)
                .filter(|n| (1..=MAX_DESCRIPTORS_PER_SET).contains(n))
                .ok_or(c"unsupported: A840 descriptor array exceeds one hardware table")?;
            let mut set = spilling.unwrap_or(next_set);
            if cursors[set] + count > MAX_DESCRIPTORS_PER_SET {
                set = used_sets
                    .iter()
                    .position(|used| !used)
                    .ok_or(c"unsupported: A840 descriptor arrays exhaust the hardware tables")?;
                used_sets[set] = true;
                spilling = Some(set);
            }
            binding.descriptor_set = set as u32;
            binding.binding = cursors[set];
            cursors[set] += count;
            if locations
                .insert(original, (binding.descriptor_set, binding.binding))
                .is_some()
            {
                return Err(c"duplicate reflected descriptor binding".into());
            }
        }
        if locations.iter().any(|(old, new)| old != new) {
            for stage in &mut self.stages {
                rewrite_descriptor_slots(Arc::make_mut(&mut stage.words), &locations)?;
            }
        }
        self.bindings.sort_by_key(|b| (b.descriptor_set, b.binding));
        Ok(())
    }
}

fn set_workgroup_defaults(stage: &mut StageModule, local_size: [u32; 3]) -> FfiResult<()> {
    let mut key_values = BTreeMap::new();
    for (key, value) in threadgroup_constants(local_size) {
        if let Some(spec) = stage
            .specialization
            .constants
            .iter()
            .find(|spec| spec.key == key)
        {
            key_values.insert(spec.id, value as u32);
        }
    }
    let words = Arc::make_mut(&mut stage.spirv);
    let instructions = spirv_instructions(words)?;
    let mut ids = BTreeMap::new();
    for range in &instructions {
        if let [op, result, SPV_DEC_SPEC_ID, spec] = words[range.clone()] {
            if op & 0xffff == SPV_OP_DECORATE {
                if let Some(&value) = key_values.get(&spec) {
                    ids.insert(result, value);
                }
            }
        }
    }
    for range in instructions {
        let inst = &mut words[range];
        match inst[0] & 0xffff {
            SPV_OP_SPEC_CONSTANT if inst.len() == 4 => {
                if let Some(&value) = ids.get(&inst[2]) {
                    inst[3] = value;
                }
            }
            SPV_OP_SPEC_CONSTANT_TRUE | SPV_OP_SPEC_CONSTANT_FALSE if inst.len() == 3 => {
                if let Some(&value) = ids.get(&inst[2]) {
                    inst[0] = (3 << 16)
                        | if value == 0 {
                            SPV_OP_SPEC_CONSTANT_FALSE
                        } else {
                            SPV_OP_SPEC_CONSTANT_TRUE
                        };
                }
            }
            _ => {}
        }
    }
    Ok(())
}

// vtn_variables.c records a null Workgroup initializer as a shader flag.
// The frozen A840 lowering never runs Mesa's nir_zero_initialize_shared_memory.
// Emit stores to the declared objects by a single invocation, followed by a
// barrier executed by every invocation before the original entry body.
fn lower_zeroed_workgroup(words: &[u32], enabled: bool) -> FfiResult<Option<Vec<u32>>> {
    let instructions = spirv_instructions(words)?;
    let mut initializers = Vec::new();
    let mut nulls = BTreeMap::new();
    let mut uint = None;
    let mut bool_type = None;
    let mut local_index = None;
    let mut entry = None;
    let mut functions_at = None;
    let mut types_at = None;
    for range in &instructions {
        let inst = &words[range.clone()];
        let opcode = inst[0] & 0xffff;
        if (SPV_OP_TYPE_VOID..=SPV_OP_TYPE_FORWARD_POINTER).contains(&opcode) {
            types_at.get_or_insert(range.start);
        }
        match (opcode, &inst[1..]) {
            (SPV_OP_ENTRY_POINT, [5, id, ..]) => {
                if entry.replace(*id).is_some() {
                    return Err(SPV_INVALID.into());
                }
            }
            (SPV_OP_TYPE_INT, [id, 32, 0]) => uint = Some(*id),
            (SPV_OP_TYPE_BOOL, [id]) => bool_type = Some(*id),
            (SPV_OP_CONSTANT_NULL, [_, id]) => {
                nulls.insert(*id, ());
            }
            (SPV_OP_DECORATE, [id, SPV_DEC_BUILT_IN, SPV_BUILT_IN_LOCAL_INVOCATION_INDEX]) => {
                local_index = Some(*id);
            }
            (SPV_OP_VARIABLE, [_, id, SPV_STORAGE_WORKGROUP, init]) => {
                if !nulls.contains_key(init) {
                    return Err(SPV_INVALID.into());
                }
                initializers.push((*id, *init));
            }
            (SPV_OP_FUNCTION, _) => {
                functions_at = Some(range.start);
                break;
            }
            _ => {}
        }
    }
    if initializers.is_empty() {
        return Ok(None);
    }
    if !enabled {
        return Err(
            c"unsupported: ZeroedThreadgroupMemory requires the extended compute option".into(),
        );
    }
    let entry = entry.ok_or(SPV_INVALID)?;
    let types_at = types_at.ok_or(SPV_INVALID)?;
    let functions_at = functions_at.ok_or(SPV_INVALID)?;
    let mut output = words[..5].to_vec();
    let mut declarations = Vec::new();
    let add_index = local_index.is_none();
    let mut next_id = words[3];
    let mut id = || {
        let value = next_id;
        next_id += 1;
        value
    };
    let uint = uint.unwrap_or_else(|| {
        let ty = id();
        write_instruction(&mut declarations, SPV_OP_TYPE_INT, &[ty, 32, 0]);
        ty
    });
    let bool_type = bool_type.unwrap_or_else(|| {
        let ty = id();
        write_instruction(&mut declarations, SPV_OP_TYPE_BOOL, &[ty]);
        ty
    });
    let local_index = local_index.unwrap_or_else(|| {
        let existing_pointer = instructions.iter().find_map(|range| {
            if let [op, ptr, SPV_STORAGE_INPUT, ty] = words[range.clone()] {
                (op & 0xffff == SPV_OP_TYPE_POINTER && ty == uint).then_some(ptr)
            } else {
                None
            }
        });
        let ptr = existing_pointer.unwrap_or_else(|| {
            let ptr = id();
            write_instruction(
                &mut declarations,
                SPV_OP_TYPE_POINTER,
                &[ptr, SPV_STORAGE_INPUT, uint],
            );
            ptr
        });
        let var = id();
        write_instruction(
            &mut declarations,
            SPV_OP_VARIABLE,
            &[ptr, var, SPV_STORAGE_INPUT],
        );
        var
    });
    let (zero, scope, semantics) = (id(), id(), id());
    for (constant, value) in [
        (zero, 0),
        (scope, SPV_SCOPE_WORKGROUP),
        (semantics, SPV_ACQUIRE_RELEASE_WORKGROUP_MEMORY),
    ] {
        write_instruction(&mut declarations, SPV_OP_CONSTANT, &[uint, constant, value]);
    }
    let (lane, is_zero, clear, merge) = (id(), id(), id(), id());
    output[3] = next_id;

    let mut in_entry = false;
    let mut entry_label = None;
    let mut inserted = false;
    for range in instructions {
        let inst = &words[range.clone()];
        let opcode = inst[0] & 0xffff;
        if add_index && range.start == types_at {
            write_instruction(
                &mut output,
                SPV_OP_DECORATE,
                &[
                    local_index,
                    SPV_DEC_BUILT_IN,
                    SPV_BUILT_IN_LOCAL_INVOCATION_INDEX,
                ],
            );
        }
        if range.start == functions_at {
            output.extend_from_slice(&declarations);
        }
        if opcode == SPV_OP_FUNCTION {
            in_entry = inst.get(2) == Some(&entry);
        }
        if in_entry && opcode == SPV_OP_LABEL && entry_label.is_none() {
            entry_label = Some(inst[1]);
            output.extend_from_slice(inst);
            continue;
        }
        if in_entry && entry_label.is_some() && !inserted && opcode != SPV_OP_VARIABLE {
            write_instruction(&mut output, SPV_OP_LOAD, &[uint, lane, local_index]);
            write_instruction(
                &mut output,
                SPV_OP_I_EQUAL,
                &[bool_type, is_zero, lane, zero],
            );
            write_instruction(&mut output, SPV_OP_SELECTION_MERGE, &[merge, 0]);
            write_instruction(
                &mut output,
                SPV_OP_BRANCH_CONDITIONAL,
                &[is_zero, clear, merge],
            );
            write_instruction(&mut output, SPV_OP_LABEL, &[clear]);
            for &(var, init) in &initializers {
                write_instruction(&mut output, SPV_OP_STORE, &[var, init]);
            }
            write_instruction(&mut output, SPV_OP_BRANCH, &[merge]);
            write_instruction(&mut output, SPV_OP_LABEL, &[merge]);
            write_instruction(
                &mut output,
                SPV_OP_CONTROL_BARRIER,
                &[scope, scope, semantics],
            );
            inserted = true;
        }
        match (opcode, &inst[1..]) {
            (SPV_OP_ENTRY_POINT, [5, _, ..]) if add_index => {
                output.push(inst[0] + (1 << 16));
                output.extend_from_slice(&inst[1..]);
                output.push(local_index);
            }
            (SPV_OP_VARIABLE, [ptr, var, SPV_STORAGE_WORKGROUP, _]) => {
                write_instruction(
                    &mut output,
                    SPV_OP_VARIABLE,
                    &[*ptr, *var, SPV_STORAGE_WORKGROUP],
                );
            }
            (SPV_OP_PHI, [ty, result, args @ ..]) if inserted && in_entry => {
                output.extend_from_slice(&[inst[0], *ty, *result]);
                let (pairs, trailing) = args.as_chunks::<2>();
                if !trailing.is_empty() {
                    return Err(SPV_INVALID.into());
                }
                for &[value, pred] in pairs {
                    let label = if Some(pred) == entry_label {
                        merge
                    } else {
                        pred
                    };
                    output.extend_from_slice(&[value, label]);
                }
            }
            _ => output.extend_from_slice(inst),
        }
    }
    if !inserted {
        return Err(SPV_INVALID.into());
    }
    Ok(Some(output))
}

#[repr(C)]
pub struct NmbComputePipelineDesc {
    local_size: [u32; 3],
    zeroed_threadgroup_memory: u32,
}

/// # Safety
/// Handles, name and descriptor must stay valid for this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_compile_compute_spirv_ex(
    dev: *const NmbDevice,
    lib: *const NmbLibrary,
    fn_name: *const c_char,
    input: *const NmbComputePipelineDesc,
) -> *mut NmbSpirvResult {
    ffi(ptr::null_mut(), || {
        let dev = unsafe { borrowed(dev, c"device is required") }?;
        let lib = unsafe { borrowed(lib, c"library is required") }?;
        let input = unsafe { borrowed(input, c"compute descriptor is required") }?;
        let name = unsafe { name_arg(fn_name) }?;
        if input.zeroed_threadgroup_memory > 1 {
            return Err(c"zeroed_threadgroup_memory must be 0 or 1".into());
        }
        let local_size = (input.local_size != [0; 3]).then_some(input.local_size);
        let threads = input
            .local_size
            .into_iter()
            .try_fold(1_u32, u32::checked_mul);
        if local_size.is_some() && !matches!(threads, Some(1..=a840_profile::MAX_WORKGROUP_THREADS))
        {
            return Err(c"local_size must be all zero or a positive A840 workgroup size".into());
        }
        let func = lib.function(name, FunctionStage::Kernel)?;
        let mut desc =
            ComputePipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(func)));
        desc.required_threads_per_threadgroup = local_size;
        let mut pipeline = dev
            .extended()?
            .compile_compute_pipeline(&desc, &Budget::default())
            .map_err(message)?;
        if let Some(size) = local_size {
            if threads.unwrap_or_default() > pipeline.metal_info.max_total_threads_per_threadgroup {
                return Err(c"local_size exceeds the kernel's threadgroup limit".into());
            }
            set_workgroup_defaults(&mut pipeline.stage, size)?;
        }
        if let Some(words) =
            lower_zeroed_workgroup(&pipeline.stage.spirv, input.zeroed_threadgroup_memory != 0)?
        {
            pipeline.stage.spirv = words.into();
        }
        let mut result = NmbSpirvResult::new([pipeline.stage], pipeline.layout, &pipeline.interface)?;
        result.pack_bindless()?;
        Ok(Box::into_raw(Box::new(result)))
    })
}

/// # Safety
/// The result handle must stay live and a non-null out_bytes must be writable.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_words(
    res: *const NmbSpirvResult,
    stage_index: u32,
    out_bytes: *mut usize,
) -> *const u32 {
    ffi(ptr::null(), || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        if out_bytes.is_null() {
            return Err(c"out_bytes is required".into());
        }
        let stage = res.stage(stage_index)?;
        unsafe { out_bytes.write(std::mem::size_of_val(&*stage.words)) };
        Ok(stage.words.as_ptr())
    })
}

/// # Safety
/// A non-null result handle must stay live while the returned pointer is used.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_entry_point(
    res: *const NmbSpirvResult,
    stage_index: u32,
) -> *const c_char {
    ffi(ptr::null(), || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        Ok(res.stage(stage_index)?.entry_point.as_ptr())
    })
}

/// # Safety
/// A non-null result handle must stay live during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_stage_count(res: *const NmbSpirvResult) -> u32 {
    ffi(0, || {
        Ok(unsafe { borrowed(res, c"SPIR-V result is required") }?
            .stages
            .len() as u32)
    })
}

/// # Safety
/// A non-null result handle must stay live during this call.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_binding_count(res: *const NmbSpirvResult) -> usize {
    ffi(0, || {
        Ok(unsafe { borrowed(res, c"SPIR-V result is required") }?
            .bindings
            .len())
    })
}

/// # Safety
/// The result handle must stay live and a non-null out must be writable.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_binding(
    res: *const NmbSpirvResult,
    index: usize,
    out: *mut NmbSpirvBinding,
) -> bool {
    ffi(false, || {
        let res = unsafe { borrowed(res, c"SPIR-V result is required") }?;
        if out.is_null() {
            return Err(c"binding output is required".into());
        }
        let desc = res
            .bindings
            .get(index)
            .ok_or(c"binding index is out of range")?;
        unsafe { out.write(*desc) };
        Ok(true)
    })
}

/// # Safety
/// A non-null result handle must be live and owned by the caller.
#[no_mangle]
pub unsafe extern "C" fn nmb_spirv_result_free(res: *mut NmbSpirvResult) {
    ffi((), || {
        unsafe { destroy(res) };
        Ok(())
    });
}
#[cfg(test)]
mod tests {
    use super::*;

    fn fat(cpu: u32, offset: u32, size: u32) -> [u8; 32] {
        let mut bytes = [0; 32];
        bytes[..4].copy_from_slice(&FAT_MAGIC);
        bytes[4..8].copy_from_slice(&1u32.to_be_bytes());
        bytes[8..12].copy_from_slice(&cpu.to_be_bytes());
        bytes[16..20].copy_from_slice(&offset.to_be_bytes());
        bytes[20..24].copy_from_slice(&size.to_be_bytes());
        bytes
    }

    #[test]
    fn thin_metallib() {
        assert_eq!(air_slice(b"MTLB\x00\x01"), Ok((0, 6)));
    }

    #[test]
    fn fat_air64_slice() {
        assert_eq!(air_slice(&fat(AIR64, 28, 4)), Ok((28, 4)));
    }

    #[test]
    fn fat_without_air64() {
        assert_eq!(
            air_slice(&fat(0x0100_000c, 28, 4)),
            Err(c"Mach-O fat container contains no AIR64 slice")
        );
    }

    #[test]
    fn truncated_fat_header_and_table() {
        let bytes = fat(AIR64, 28, 4);
        assert_eq!(air_slice(&bytes[..7]), Err(c"truncated Mach-O fat header"));
        assert_eq!(
            air_slice(&bytes[..27]),
            Err(c"truncated Mach-O fat architecture table")
        );
    }

    #[test]
    fn rejects_invalid_ranges() {
        for (offset, size) in [(27, 4), (28, 0), (30, 4), (u32::MAX, u32::MAX)] {
            assert_eq!(
                air_slice(&fat(AIR64, offset, size)),
                Err(c"Mach-O fat slice range is out of bounds or empty")
            );
        }
    }

    #[test]
    fn fat64_and_metal_universal() {
        let mut wide = [0; 44];
        wide[..4].copy_from_slice(&FAT64_MAGIC);
        wide[4..8].copy_from_slice(&1u32.to_be_bytes());
        wide[8..12].copy_from_slice(&AIR64.to_be_bytes());
        wide[16..24].copy_from_slice(&40u64.to_be_bytes());
        wide[24..32].copy_from_slice(&4u64.to_be_bytes());
        assert_eq!(air_slice(&wide), Ok((40, 4)));
        wide[24..32].copy_from_slice(&u64::MAX.to_be_bytes());
        assert!(air_slice(&wide).is_err());

        let mut metal = fat(AIR64, 28, 4);
        metal[..4].copy_from_slice(&METAL_FAT_MAGIC);
        assert_eq!(air_slice(&metal), Ok((28, 4)));
    }

    #[test]
    fn archive_section_range_is_checked() {
        const SECTION: usize = 32 + 72;
        const PAYLOAD: usize = SECTION + 80;
        let mut bytes = [0u8; PAYLOAD + 4];
        bytes[..4].copy_from_slice(&MACH64_MAGIC);
        bytes[4..8].copy_from_slice(&APPLE_GPU.to_le_bytes());
        bytes[16..20].copy_from_slice(&1u32.to_le_bytes());
        bytes[20..24].copy_from_slice(&152u32.to_le_bytes());
        bytes[32..36].copy_from_slice(&0x19u32.to_le_bytes());
        bytes[36..40].copy_from_slice(&152u32.to_le_bytes());
        bytes[96..100].copy_from_slice(&1u32.to_le_bytes());
        bytes[SECTION..SECTION + 10].copy_from_slice(b"__metallib");
        bytes[SECTION + 16..SECTION + 22].copy_from_slice(b"__TEXT");
        bytes[SECTION + 40..SECTION + 48].copy_from_slice(&4u64.to_le_bytes());
        bytes[SECTION + 48..SECTION + 52].copy_from_slice(&(PAYLOAD as u32).to_le_bytes());
        bytes[PAYLOAD..].copy_from_slice(b"MTLB");
        assert_eq!(archive_air_section(&bytes), Ok(&b"MTLB"[..]));
        assert!(archive_air_section(&bytes[..PAYLOAD]).is_err());
        assert!(archive_air_section(&bytes[..SECTION]).is_err());
    }

    #[test]
    fn ffi_contains_panic_and_clears_error_on_success() {
        assert_eq!(ffi(0, || panic!("test panic")), 0);
        assert_eq!(
            unsafe { CStr::from_ptr(nmb_last_error()) },
            c"panic inside native Metal translator"
        );
        assert_eq!(ffi(0, || Ok(1)), 1);
        assert_eq!(unsafe { CStr::from_ptr(nmb_last_error()) }, c"");
    }

    #[test]
    fn rejects_out_of_range_arch_count() {
        for count in [0u32, 17, u32::MAX] {
            let mut bytes = fat(AIR64, 28, 4);
            bytes[4..8].copy_from_slice(&count.to_be_bytes());
            assert_eq!(
                air_slice(&bytes),
                Err(c"Mach-O fat architecture count must be in 1..=16")
            );
        }
    }
}
