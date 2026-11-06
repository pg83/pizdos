//! Cranelift for one defined function.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use cranelift_codegen::binemit::Reloc;
use cranelift_codegen::control::ControlPlane;
use cranelift_codegen::ir::ExternalName;
use cranelift_codegen::isa::{self, OwnedTargetIsa, TargetIsa};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_codegen::{Context, RelocTarget};
use cranelift_frontend::FunctionBuilderContext;

use crate::module::Module;
use crate::translate;

pub struct Relocation {
    /// Offset in the function's code.
    pub offset: u32,
    pub kind: Reloc,
    pub namespace: u32,
    pub index: u32,
    pub addend: i64,
}

/// One function's code and what the artifact needs to know about it.
pub struct Compiled {
    pub code: Vec<u8>,
    /// Stack bytes from the stack pointer in the body up to and including
    /// the return address.
    pub frame: u32,
    pub relocations: Vec<Relocation>,
}

/// State reused from one function to the next; one per thread.
pub struct Scratch {
    builder: FunctionBuilderContext,
    context: Context,
}

impl Default for Scratch {
    fn default() -> Scratch {
        Scratch {
            builder: FunctionBuilderContext::new(),
            context: Context::new(),
        }
    }
}

/// x86-64-v2: SSE4.1 rounds without libcalls, POPCNT counts in one
/// instruction.
pub fn x86_64() -> Result<OwnedTargetIsa, String> {
    let mut flags = settings::builder();

    for (name, value) in [
        ("opt_level", "speed"),
        ("preserve_frame_pointers", "true"),
        ("enable_probestack", "false"),
        ("unwind_info", "false"),
        ("use_colocated_libcalls", "false"),
    ] {
        flags.set(name, value).map_err(|e| format!("{name}: {e}"))?;
    }

    let mut builder = isa::lookup_by_name("x86_64-unknown-linux").map_err(|e| e.to_string())?;

    for feature in ["has_sse3", "has_ssse3", "has_sse41", "has_sse42", "has_popcnt"] {
        builder.enable(feature).map_err(|e| format!("{feature}: {e}"))?;
    }

    builder.finish(settings::Flags::new(flags)).map_err(|e| e.to_string())
}

pub fn function(module: &Module, isa: &dyn TargetIsa, index: u32, scratch: &mut Scratch) -> Result<Compiled, String> {
    let context = &mut scratch.context;
    context.clear();
    context.func = translate::translate(module, index, &mut scratch.builder, isa.frontend_config())?;
    let compiled = context
        .compile(isa, &mut ControlPlane::default())
        .map_err(|e| format!("{:?}", e.inner))?;
    let code = compiled.code_buffer().to_vec();
    let frame = compiled
        .buffer
        .frame_layout()
        .map_or(0, |layout| layout.frame_to_fp_offset)
        + 16;
    let relocs = compiled.buffer.relocs().to_vec();
    let names = context.func.params.user_named_funcs();
    let mut relocations = Vec::with_capacity(relocs.len());

    for reloc in relocs {
        let RelocTarget::ExternalName(ExternalName::User(name)) = reloc.target else {
            return Err(format!("relocation to {:?}", reloc.target));
        };
        let name = &names[name];
        relocations.push(Relocation {
            offset: reloc.offset,
            kind: reloc.kind,
            namespace: name.namespace,
            index: name.index,
            addend: reloc.addend,
        });
    }

    Ok(Compiled {
        code,
        frame,
        relocations,
    })
}
