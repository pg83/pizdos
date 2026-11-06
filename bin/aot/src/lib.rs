//! aot: native code for a wasm module of the system, ahead of time. A
//! module's bytes in, an artifact's bytes out; nothing else is touched, so
//! the crate is no_std and runs both as the host tool (cli/) and as a wasm
//! module inside the kernel (wasm/).
//!
//! abi.rs is the contract the code keeps with the kernel, artifact.rs the
//! artifact format.

#![no_std]

extern crate alloc;

pub mod abi;
mod artifact;
mod compile;
mod module;
mod translate;

use alloc::string::String;
use alloc::vec::Vec;

use cranelift_codegen::isa::OwnedTargetIsa;

pub use compile::{Compiled, Scratch};

/// A parsed, validated module and the target; functions compile one at a
/// time and in any order, the artifact comes out the same.
pub struct Compiler<'a> {
    module: module::Module<'a>,
    isa: OwnedTargetIsa,
}

impl<'a> Compiler<'a> {
    pub fn new(wasm: &'a [u8]) -> Result<Compiler<'a>, String> {
        Ok(Compiler {
            module: module::Module::parse(wasm)?,
            isa: compile::x86_64()?,
        })
    }

    /// The number of defined functions.
    pub fn functions(&self) -> usize {
        self.module.bodies.len()
    }

    /// The defined function at `slot`, counted from the first after the
    /// imports.
    pub fn function(&self, slot: usize, scratch: &mut Scratch) -> Result<Compiled, String> {
        let index = self.module.imports + slot as u32;

        compile::function(&self.module, self.isa.as_ref(), index, scratch)
            .map_err(|e| alloc::format!("function {index}: {e}"))
    }

    /// The artifact of every defined function, compiled in slot order.
    pub fn artifact(&self, functions: &[Compiled]) -> Result<Vec<u8>, String> {
        artifact::write(&self.module, functions)
    }
}

/// The artifact of a module, one function after another.
pub fn compile(wasm: &[u8]) -> Result<Vec<u8>, String> {
    let compiler = Compiler::new(wasm)?;
    let mut scratch = Scratch::default();
    let functions = (0..compiler.functions())
        .map(|slot| compiler.function(slot, &mut scratch))
        .collect::<Result<Vec<_>, _>>()?;

    compiler.artifact(&functions)
}
