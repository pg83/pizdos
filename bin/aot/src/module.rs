//! What the translator needs to know about a module: function types, the
//! function index space, globals and the code. The module is validated
//! first, so the translator may take a well-formed module for granted.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use wasmparser::{
    CompositeInnerType, FuncType, FunctionBody, Operator, Parser, Payload, TypeRef, ValType, Validator, WasmFeatures,
};

/// What clang emits for wasm32 with -mbulk-memory: the MVP, mutable
/// globals, sign extension, non-trapping conversions, multi-value blocks,
/// bulk memory, and the reference-types encoding of call_indirect.
pub const FEATURES: WasmFeatures = WasmFeatures::MVP
    .union(WasmFeatures::MUTABLE_GLOBAL)
    .union(WasmFeatures::SIGN_EXTENSION)
    .union(WasmFeatures::SATURATING_FLOAT_TO_INT)
    .union(WasmFeatures::MULTI_VALUE)
    .union(WasmFeatures::BULK_MEMORY)
    .union(WasmFeatures::REFERENCE_TYPES);

pub struct Global {
    pub ty: ValType,
    pub mutable: bool,
    /// The value of an immutable global initialized by a constant, as raw bits.
    pub constant: Option<u64>,
}

pub struct Module<'a> {
    pub types: Vec<FuncType>,
    /// Type index to the first type index with the same signature: the
    /// identity call_indirect checks.
    pub canonical: Vec<u32>,
    /// Function index to type index; imports come first.
    pub functions: Vec<u32>,
    pub imports: u32,
    pub globals: Vec<Global>,
    pub bodies: Vec<FunctionBody<'a>>,
}

impl<'a> Module<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Module<'a>, String> {
        let mut validator = Validator::new_with_features(FEATURES);
        let mut module = Module {
            types: Vec::new(),
            canonical: Vec::new(),
            functions: Vec::new(),
            imports: 0,
            globals: Vec::new(),
            bodies: Vec::new(),
        };

        for payload in Parser::new(0).parse_all(bytes) {
            let payload = payload.map_err(|e| e.to_string())?;

            if let wasmparser::ValidPayload::Func(func, body) =
                validator.payload(&payload).map_err(|e| e.to_string())?
            {
                func.into_validator(Default::default())
                    .validate(&body)
                    .map_err(|e| e.to_string())?;
            }

            match payload {
                Payload::TypeSection(reader) => {
                    for group in reader {
                        for ty in group.map_err(|e| e.to_string())?.into_types() {
                            match ty.composite_type.inner {
                                CompositeInnerType::Func(func) => module.add_type(func),
                                _ => return Err("only function types are supported".into()),
                            }
                        }
                    }
                }
                Payload::ImportSection(reader) => {
                    for import in reader.into_imports() {
                        match import.map_err(|e| e.to_string())?.ty {
                            TypeRef::Func(index) => {
                                module.functions.push(index);
                                module.imports += 1;
                            }
                            _ => return Err("only functions can be imported".into()),
                        }
                    }
                }
                Payload::FunctionSection(reader) => {
                    for index in reader {
                        module.functions.push(index.map_err(|e| e.to_string())?);
                    }
                }
                Payload::GlobalSection(reader) => {
                    for global in reader {
                        let global = global.map_err(|e| e.to_string())?;
                        let constant = match global
                            .init_expr
                            .get_operators_reader()
                            .read()
                            .map_err(|e| e.to_string())?
                        {
                            _ if global.ty.mutable => None,
                            Operator::I32Const { value } => Some(value as u32 as u64),
                            Operator::I64Const { value } => Some(value as u64),
                            Operator::F32Const { value } => Some(value.bits() as u64),
                            Operator::F64Const { value } => Some(value.bits()),
                            _ => None,
                        };
                        module.globals.push(Global {
                            ty: global.ty.content_type,
                            mutable: global.ty.mutable,
                            constant,
                        });
                    }
                }
                Payload::CodeSectionEntry(body) => module.bodies.push(body),
                _ => {}
            }
        }

        Ok(module)
    }

    fn add_type(&mut self, func: FuncType) {
        let index = self.types.len() as u32;
        let canonical = self.types.iter().position(|t| *t == func).map_or(index, |i| i as u32);
        self.types.push(func);
        self.canonical.push(canonical);
    }

    pub fn function_type(&self, function: u32) -> &FuncType {
        &self.types[self.functions[function as usize] as usize]
    }
}
