//! The artifact: native code for every defined function of one module.
//!
//! Little-endian. A header of eight u32:
//!
//!     magic      "PZAO"
//!     version    1
//!     arch       1: x86_64
//!     functions  entries of the function table, imports included
//!     imports    the first `imports` entries are imports
//!     relocations
//!     code       bytes of code
//!     max_frame  the largest `frame` of any function
//!
//! then `functions` entries of four u32 (offset and size of the code, frame,
//! canonical type index; an import has offset and size 0xffffffff and frame
//! 0), then `relocations` entries of four u32 (offset in the code, kind 1:
//! an absolute 64-bit address, target 1: an import by function index or
//! 2: a kernel helper by abi::Helper, the index), then the code. Calls
//! between defined functions are already resolved; the code runs wherever
//! the kernel maps it once the relocations are written. A function's frame
//! is the stack it takes from the stack pointer in its body up to and
//! including its return address.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use cranelift_codegen::binemit::Reloc;

use crate::compile::Compiled;
use crate::module::Module;
use crate::translate::{DEFINED, HELPER, IMPORTED};

const MAGIC: u32 = u32::from_le_bytes(*b"PZAO");
const VERSION: u32 = 1;
const X86_64: u32 = 1;
const ABS64: u32 = 1;
const ALIGN: usize = 16;

fn put(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub fn write(module: &Module, functions: &[Compiled]) -> Result<Vec<u8>, String> {
    let mut code = Vec::new();
    let mut offsets = Vec::with_capacity(functions.len());

    for function in functions {
        code.resize(code.len().next_multiple_of(ALIGN), 0xcc);
        offsets.push(code.len());
        code.extend_from_slice(&function.code);
    }

    let mut relocations = Vec::new();

    for (function, start) in functions.iter().zip(&offsets) {
        for relocation in &function.relocations {
            let site = start + relocation.offset as usize;

            match (relocation.namespace, relocation.kind) {
                (DEFINED, Reloc::X86CallPCRel4 | Reloc::X86CallPLTRel4) => {
                    let target = offsets[(relocation.index - module.imports) as usize] as i64;
                    let value = i32::try_from(target + relocation.addend - site as i64).map_err(|e| e.to_string())?;
                    code[site..site + 4].copy_from_slice(&value.to_le_bytes());
                }
                (IMPORTED | HELPER, Reloc::Abs8) if relocation.addend == 0 => {
                    relocations.push([site as u32, ABS64, relocation.namespace, relocation.index]);
                }
                (namespace, kind) => {
                    return Err(format!(
                        "unexpected relocation {kind:?} to {namespace}:{}",
                        relocation.index
                    ));
                }
            }
        }
    }

    let mut out = Vec::new();
    let max_frame = functions.iter().map(|f| f.frame).max().unwrap_or(0);

    for value in [
        MAGIC,
        VERSION,
        X86_64,
        module.functions.len() as u32,
        module.imports,
        relocations.len() as u32,
        code.len() as u32,
        max_frame,
    ] {
        put(&mut out, value);
    }

    for (index, ty) in module.functions.iter().enumerate() {
        let canonical = module.canonical[*ty as usize];

        match index.checked_sub(module.imports as usize) {
            None => {
                for value in [u32::MAX, u32::MAX, 0, canonical] {
                    put(&mut out, value);
                }
            }
            Some(defined) => {
                let function = &functions[defined];

                for value in [
                    offsets[defined] as u32,
                    function.code.len() as u32,
                    function.frame,
                    canonical,
                ] {
                    put(&mut out, value);
                }
            }
        }
    }

    for relocation in relocations {
        for value in relocation {
            put(&mut out, value);
        }
    }

    out.extend_from_slice(&code);

    Ok(out)
}
