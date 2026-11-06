//! One wasm function body to CLIF.
//!
//! The operand stack is a vector of SSA values, wasm locals are frontend
//! variables, and every block, loop and if gets a continuation block whose
//! parameters carry its results. Code after an unconditional branch is
//! skipped up to the `else` or `end` that makes it reachable again.

use alloc::borrow::Cow;
use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

use cranelift_codegen::ir::condcodes::{FloatCC, IntCC};
use cranelift_codegen::ir::immediates::{Ieee32, Ieee64};
use cranelift_codegen::ir::types::{F32, F64, I8, I16, I32, I64};
use cranelift_codegen::ir::{
    AbiParam, AliasRegionData, Block, BlockArg, BlockCall, ExtFuncData, ExternalName, FuncRef, Function, InstBuilder,
    JumpTableData, MemFlagsData, SigRef, Signature, TrapCode, Type, UserExternalName, UserFuncName, Value,
};
use cranelift_codegen::isa::{CallConv, TargetFrontendConfig};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Variable};
use wasmparser::{BlockType, FuncType, MemArg, Operator, ValType};

use crate::abi::{self, Helper, Trap};
use crate::module::Module;

/// Namespaces of the external names a function calls: a defined function
/// by function index, an import by function index, a kernel helper.
pub const DEFINED: u32 = 0;
pub const IMPORTED: u32 = 1;
pub const HELPER: u32 = 2;

pub fn value_type(ty: ValType) -> Result<Type, String> {
    match ty {
        ValType::I32 => Ok(I32),
        ValType::I64 => Ok(I64),
        ValType::F32 => Ok(F32),
        ValType::F64 => Ok(F64),
        other => Err(format!("unsupported value type {other}")),
    }
}

pub fn signature(ty: &FuncType) -> Result<Signature, String> {
    if ty.results().len() > 1 {
        return Err("a function returns more than one value".into());
    }

    let mut signature = Signature::new(CallConv::SystemV);
    signature.params.push(AbiParam::new(I64));

    for param in ty.params() {
        signature.params.push(AbiParam::new(value_type(*param)?));
    }

    for result in ty.results() {
        signature.returns.push(AbiParam::new(value_type(*result)?));
    }

    Ok(signature)
}

fn helper_signature(helper: Helper) -> Signature {
    let mut signature = Signature::new(CallConv::SystemV);
    let params = match helper {
        Helper::Trap => 1,
        Helper::StackCheck => 0,
        Helper::MemoryGrow => 1,
        Helper::MemoryCopy | Helper::MemoryFill => 3,
        Helper::MemoryInit => 4,
        Helper::DataDrop => 1,
    };
    signature.params.push(AbiParam::new(I64));
    signature.params.extend((0..params).map(|_| AbiParam::new(I32)));

    if helper == Helper::MemoryGrow {
        signature.returns.push(AbiParam::new(I32));
    }

    signature
}

enum Kind {
    Function,
    Block,
    Loop,
    If { otherwise: Block, has_else: bool },
}

struct Frame {
    kind: Kind,
    /// Where a branch to this frame goes: a loop's header, otherwise `end`.
    target: Block,
    /// The continuation; its parameters are the frame's results.
    end: Block,
    params: usize,
    results: usize,
    /// Operand stack height below the frame's parameters.
    height: usize,
    /// Whether `end` has a predecessor.
    reached: bool,
}

struct Translator<'m, 'f> {
    module: &'m Module<'m>,
    b: FunctionBuilder<'f>,
    vmctx: Value,
    locals: Vec<Variable>,
    stack: Vec<Value>,
    control: Vec<Frame>,
    reachable: bool,
    /// Nesting depth of blocks opened in unreachable code.
    dead: u32,
    calls: BTreeMap<(u32, u32), FuncRef>,
    signatures: BTreeMap<u32, SigRef>,
    /// The blocks that call `trap`, one per code, filled at the end in
    /// creation order.
    traps: Vec<(Trap, Block)>,
    /// Accesses to the context, linear memory and the table: three alias
    /// regions, so a store to memory keeps a loaded context word valid.
    vmctx_flags: MemFlagsData,
    heap_flags: MemFlagsData,
    table_flags: MemFlagsData,
}

fn args(values: &[Value]) -> Vec<BlockArg> {
    values.iter().map(|v| BlockArg::Value(*v)).collect()
}

fn region(func: &mut Function, user_id: u32, description: &'static str) -> MemFlagsData {
    let region = func.dfg.alias_regions.insert(AliasRegionData {
        user_id,
        description: Cow::Borrowed(description),
    });

    MemFlagsData::new().with_alias_region(Some(region))
}

pub fn translate(
    module: &Module,
    index: u32,
    context: &mut FunctionBuilderContext,
    config: TargetFrontendConfig,
) -> Result<Function, String> {
    let ty = module.function_type(index);
    let mut func = Function::with_name_signature(UserFuncName::user(DEFINED, index), signature(ty)?);
    let vmctx_flags = region(&mut func, 0, "vmctx").with_notrap().with_aligned();
    // In bounds by the explicit check before every access.
    let heap_flags = region(&mut func, 1, "memory").with_notrap();
    let table_flags = region(&mut func, 2, "table")
        .with_notrap()
        .with_aligned()
        .with_readonly();
    let body = &module.bodies[(index - module.imports) as usize];
    let mut b = FunctionBuilder::new(&mut func, context);
    let entry = b.create_block();
    b.append_block_params_for_function_params(entry);
    b.switch_to_block(entry);
    b.seal_block(entry);
    let vmctx = b.block_params(entry)[0];
    let params: Vec<Value> = b.block_params(entry)[1..].to_vec();
    let mut locals = Vec::new();

    for (param, value) in ty.params().iter().zip(params) {
        let var = b.declare_var(value_type(*param)?);
        b.def_var(var, value);
        locals.push(var);
    }

    let mut reader = body.get_locals_reader().map_err(|e| e.to_string())?;

    for _ in 0..reader.get_count() {
        let (count, ty) = reader.read().map_err(|e| e.to_string())?;
        let ty = value_type(ty)?;

        for _ in 0..count {
            let var = b.declare_var(ty);
            let zero = match ty {
                F32 => b.ins().f32const(Ieee32::with_bits(0)),
                F64 => b.ins().f64const(Ieee64::with_bits(0)),
                _ => b.ins().iconst(ty, 0),
            };
            b.def_var(var, zero);
            locals.push(var);
        }
    }

    let end = b.create_block();

    for result in ty.results() {
        b.append_block_param(end, value_type(*result)?);
    }

    let mut t = Translator {
        module,
        b,
        vmctx,
        locals,
        stack: Vec::new(),
        control: Vec::new(),
        reachable: true,
        dead: 0,
        calls: BTreeMap::new(),
        signatures: BTreeMap::new(),
        traps: Vec::new(),
        vmctx_flags,
        heap_flags,
        table_flags,
    };
    t.control.push(Frame {
        kind: Kind::Function,
        target: end,
        end,
        params: 0,
        results: ty.results().len(),
        height: 0,
        reached: false,
    });
    t.stack_check();
    let mut operators = body.get_operators_reader().map_err(|e| e.to_string())?;

    while !operators.eof() {
        let op = operators.read().map_err(|e| e.to_string())?;
        t.operator(&op).map_err(|e| format!("{op:?}: {e}"))?;
    }

    t.fill_traps();
    t.b.finalize(config);

    Ok(func)
}

impl Translator<'_, '_> {
    fn push(&mut self, value: Value) {
        self.stack.push(value);
    }

    fn pop(&mut self) -> Value {
        self.stack.pop().expect("validated: operand stack underflow")
    }

    fn pop2(&mut self) -> (Value, Value) {
        let rhs = self.pop();
        let lhs = self.pop();

        (lhs, rhs)
    }

    fn top(&self, count: usize) -> Vec<Value> {
        self.stack[self.stack.len() - count..].to_vec()
    }

    fn popn(&mut self, count: usize) -> Vec<Value> {
        let values = self.top(count);
        self.stack.truncate(self.stack.len() - count);

        values
    }

    fn block_type(&self, ty: BlockType) -> Result<(Vec<Type>, Vec<Type>), String> {
        match ty {
            BlockType::Empty => Ok((Vec::new(), Vec::new())),
            BlockType::Type(ty) => Ok((Vec::new(), vec![value_type(ty)?])),
            BlockType::FuncType(index) => {
                let ty = &self.module.types[index as usize];
                let params = ty.params().iter().map(|t| value_type(*t)).collect::<Result<_, _>>()?;
                let results = ty.results().iter().map(|t| value_type(*t)).collect::<Result<_, _>>()?;

                Ok((params, results))
            }
        }
    }

    fn block_with_params(&mut self, types: &[Type]) -> Block {
        let block = self.b.create_block();

        for ty in types {
            self.b.append_block_param(block, *ty);
        }

        block
    }

    fn jump(&mut self, block: Block, values: &[Value]) {
        self.b.ins().jump(block, &args(values));
    }

    /// Continue in `block`, which every predecessor has already branched to.
    fn enter(&mut self, block: Block) {
        self.b.switch_to_block(block);
        self.b.seal_block(block);
    }

    fn vmctx_load(&mut self, ty: Type, offset: i32) -> Value {
        self.b.ins().load(ty, self.vmctx_flags, self.vmctx, offset)
    }

    fn call_ref(&mut self, namespace: u32, index: u32, signature: Signature, colocated: bool) -> FuncRef {
        if let Some(func) = self.calls.get(&(namespace, index)) {
            return *func;
        }

        let name = self
            .b
            .func
            .declare_imported_user_function(UserExternalName::new(namespace, index));
        let signature = self.b.import_signature(signature);
        let func = self.b.import_function(ExtFuncData {
            name: ExternalName::User(name),
            signature,
            colocated,
            patchable: false,
        });
        self.calls.insert((namespace, index), func);

        func
    }

    fn helper(&mut self, helper: Helper, values: &[Value]) -> Option<Value> {
        let func = self.call_ref(HELPER, helper as u32, helper_signature(helper), false);
        let mut call_args = vec![self.vmctx];
        call_args.extend_from_slice(values);
        let call = self.b.ins().call(func, &call_args);

        self.b.inst_results(call).first().copied()
    }

    /// The block that calls `trap` with `code`; shared by all checks of
    /// the function.
    fn trap_block(&mut self, code: Trap) -> Block {
        if let Some((_, block)) = self.traps.iter().find(|(c, _)| *c == code) {
            return *block;
        }

        let block = self.b.create_block();
        self.b.set_cold_block(block);
        self.traps.push((code, block));

        block
    }

    fn fill_traps(&mut self) {
        for (code, block) in core::mem::take(&mut self.traps) {
            self.enter(block);
            let code = self.b.ins().iconst(I32, code as i64);
            self.helper(Helper::Trap, &[code]);
            // `trap` does not return.
            self.b.ins().trap(TrapCode::unwrap_user(1));
        }
    }

    fn trap_if(&mut self, condition: Value, code: Trap) {
        let trap = self.trap_block(code);
        let next = self.b.create_block();
        self.b.ins().brif(condition, trap, &[], next, &[]);
        self.enter(next);
    }

    fn trap_now(&mut self, code: Trap) {
        let trap = self.trap_block(code);
        self.jump(trap, &[]);
        self.reachable = false;
    }

    /// Call `stack_check` when the stack pointer is below the limit the
    /// kernel keeps in the context; done on entry and on every loop header.
    fn stack_check(&mut self) {
        let address = self.b.ins().iadd_imm_s(self.vmctx, abi::STACK_LIMIT as i64);
        // An atomic load: the kernel may raise the limit from another thread
        // and a loop must see it.
        let limit = self.b.ins().atomic_load(I64, MemFlagsData::trusted(), address);
        let sp = self.b.ins().get_stack_pointer(I64);
        let low = self.b.ins().icmp(IntCC::UnsignedLessThan, sp, limit);
        let slow = self.b.create_block();
        let next = self.b.create_block();
        self.b.set_cold_block(slow);
        self.b.ins().brif(low, slow, &[], next, &[]);
        self.enter(slow);
        self.helper(Helper::StackCheck, &[]);
        self.jump(next, &[]);
        self.enter(next);
    }

    /// The host address of a `size`-byte access, after the bounds check.
    fn heap_address(&mut self, memarg: &MemArg, size: u64) -> (Value, i32) {
        let index = self.pop();
        let index = self.b.ins().uextend(I64, index);
        let end = self.b.ins().iadd_imm_s(index, (memarg.offset + size) as i64);
        let memory_size = self.vmctx_load(I64, abi::MEMORY_SIZE);
        let out = self.b.ins().icmp(IntCC::UnsignedGreaterThan, end, memory_size);
        self.trap_if(out, Trap::MemoryOutOfBounds);
        let base = self.b.ins().load(
            I64,
            self.vmctx_flags.with_readonly().with_can_move(),
            self.vmctx,
            abi::MEMORY_BASE,
        );
        let address = self.b.ins().iadd(base, index);

        match i32::try_from(memarg.offset) {
            Ok(offset) => (address, offset),
            Err(_) => (self.b.ins().iadd_imm_s(address, memarg.offset as i64), 0),
        }
    }

    fn load(
        &mut self,
        memarg: &MemArg,
        size: u64,
        op: impl FnOnce(&mut FunctionBuilder, MemFlagsData, Value, i32) -> Value,
    ) {
        let (address, offset) = self.heap_address(memarg, size);
        let value = op(&mut self.b, self.heap_flags, address, offset);
        self.push(value);
    }

    fn store(
        &mut self,
        memarg: &MemArg,
        size: u64,
        op: impl FnOnce(&mut FunctionBuilder, MemFlagsData, Value, Value, i32),
    ) {
        let value = self.pop();
        let (address, offset) = self.heap_address(memarg, size);
        op(&mut self.b, self.heap_flags, value, address, offset);
    }

    fn unary(&mut self, op: impl FnOnce(&mut FunctionBuilder, Value) -> Value) {
        let value = self.pop();
        let result = op(&mut self.b, value);
        self.push(result);
    }

    fn binary(&mut self, op: impl FnOnce(&mut FunctionBuilder, Value, Value) -> Value) {
        let (lhs, rhs) = self.pop2();
        let result = op(&mut self.b, lhs, rhs);
        self.push(result);
    }

    fn icmp(&mut self, cc: IntCC) {
        let (lhs, rhs) = self.pop2();
        let bit = self.b.ins().icmp(cc, lhs, rhs);
        let result = self.b.ins().uextend(I32, bit);
        self.push(result);
    }

    fn fcmp(&mut self, cc: FloatCC) {
        let (lhs, rhs) = self.pop2();
        let bit = self.b.ins().fcmp(cc, lhs, rhs);
        let result = self.b.ins().uextend(I32, bit);
        self.push(result);
    }

    fn eqz(&mut self) {
        let value = self.pop();
        let bit = self.b.ins().icmp_imm_s(IntCC::Equal, value, 0);
        let result = self.b.ins().uextend(I32, bit);
        self.push(result);
    }

    fn extend(&mut self, narrow: Type, wide: Type) {
        let value = self.pop();
        let narrowed = self.b.ins().ireduce(narrow, value);
        let result = self.b.ins().sextend(wide, narrowed);
        self.push(result);
    }

    /// Integer division and remainder with wasm's traps checked first, so
    /// the machine instruction never faults.
    fn divide(&mut self, ty: Type, signed: bool, remainder: bool) {
        let (lhs, rhs) = self.pop2();
        let zero = self.b.ins().icmp_imm_s(IntCC::Equal, rhs, 0);
        self.trap_if(zero, Trap::IntegerDivisionByZero);
        let result = match (signed, remainder) {
            (false, false) => self.b.ins().udiv(lhs, rhs),
            (false, true) => self.b.ins().urem(lhs, rhs),
            (true, false) => {
                let min = if ty == I32 { i32::MIN as i64 } else { i64::MIN };
                let is_min = self.b.ins().icmp_imm_s(IntCC::Equal, lhs, min);
                let minus_one = self.b.ins().icmp_imm_s(IntCC::Equal, rhs, -1);
                let overflow = self.b.ins().band(is_min, minus_one);
                self.trap_if(overflow, Trap::IntegerOverflow);
                self.b.ins().sdiv(lhs, rhs)
            }
            (true, true) => {
                // x rem -1 is 0, as x rem 1 is; dividing MIN by -1 would fault.
                let minus_one = self.b.ins().icmp_imm_s(IntCC::Equal, rhs, -1);
                let one = self.b.ins().iconst(ty, 1);
                let divisor = self.b.ins().select(minus_one, one, rhs);
                self.b.ins().srem(lhs, divisor)
            }
        };
        self.push(result);
    }

    /// A trapping float to integer conversion: NaN and values whose
    /// truncation does not fit trap, the rest convert exactly.
    fn truncate(&mut self, to: Type, signed: bool) {
        let value = self.pop();
        let from = self.b.func.dfg.value_type(value);
        let nan = self.b.ins().fcmp(FloatCC::Unordered, value, value);
        self.trap_if(nan, Trap::BadConversionToInteger);
        // Exclusive bounds of the values that truncate into range.
        let (low, high) = match (signed, to == I32) {
            (true, true) => (-2147483649.0, 2147483648.0),
            (true, false) => (-9223372036854777856.0, 9223372036854775808.0),
            (false, true) => (-1.0, 4294967296.0),
            (false, false) => (-1.0, 18446744073709551616.0),
        };
        let (low, high) = if from == F32 {
            // As f32 the signed lower bounds round to -2^31 and -2^63, which
            // are in range: there the lower bound is inclusive.
            let (low, high) = (low as f32, high as f32);
            (self.b.ins().f32const(low), self.b.ins().f32const(high))
        } else {
            (self.b.ins().f64const(low), self.b.ins().f64const(high))
        };
        let low_cc = if from == F32 && signed {
            FloatCC::GreaterThanOrEqual
        } else {
            FloatCC::GreaterThan
        };
        let above = self.b.ins().fcmp(low_cc, value, low);
        let below = self.b.ins().fcmp(FloatCC::LessThan, value, high);
        let fits = self.b.ins().band(above, below);
        let overflow = self.b.ins().icmp_imm_s(IntCC::Equal, fits, 0);
        self.trap_if(overflow, Trap::IntegerOverflow);
        let result = if signed {
            self.b.ins().fcvt_to_sint_sat(to, value)
        } else {
            self.b.ins().fcvt_to_uint_sat(to, value)
        };
        self.push(result);
    }

    fn branch_target(&mut self, depth: u32) -> (Block, usize) {
        let index = self.control.len() - 1 - depth as usize;
        let frame = &mut self.control[index];

        match frame.kind {
            Kind::Loop => (frame.target, frame.params),
            _ => {
                frame.reached = true;
                (frame.target, frame.results)
            }
        }
    }

    fn call(&mut self, function: u32) -> Result<(), String> {
        let ty = self.module.function_type(function);
        let params = ty.params().len();
        let signature = signature(ty)?;
        let func = if function < self.module.imports {
            self.call_ref(IMPORTED, function, signature, false)
        } else {
            self.call_ref(DEFINED, function, signature, true)
        };
        let mut call_args = vec![self.vmctx];
        call_args.extend(self.popn(params));
        let call = self.b.ins().call(func, &call_args);
        let results = self.b.inst_results(call).to_vec();
        self.stack.extend(results);

        Ok(())
    }

    fn call_indirect(&mut self, type_index: u32) -> Result<(), String> {
        let ty = &self.module.types[type_index as usize];
        let params = ty.params().len();
        let signature = match self.signatures.get(&type_index) {
            Some(signature) => *signature,
            None => {
                let signature = self.b.import_signature(signature(ty)?);
                self.signatures.insert(type_index, signature);
                signature
            }
        };
        let index = self.pop();
        let index = self.b.ins().uextend(I64, index);
        let size = self.vmctx_load(I64, abi::TABLE_SIZE);
        let out = self.b.ins().icmp(IntCC::UnsignedGreaterThanOrEqual, index, size);
        self.trap_if(out, Trap::TableOutOfBounds);
        let table = self.vmctx_load(I64, abi::TABLE_BASE);
        let offset = self.b.ins().imul_imm_s(index, abi::TABLE_ENTRY);
        let entry = self.b.ins().iadd(table, offset);
        let entry_type = self.b.ins().load(I64, self.table_flags, entry, 8);
        let expected = self.module.canonical[type_index as usize] as i64;
        let mismatch = self.b.ins().icmp_imm_s(IntCC::NotEqual, entry_type, expected);
        self.trap_if(mismatch, Trap::BadSignature);
        let code = self.b.ins().load(I64, self.table_flags, entry, 0);
        let mut call_args = vec![self.vmctx];
        call_args.extend(self.popn(params));
        let call = self.b.ins().call_indirect(signature, code, &call_args);
        let results = self.b.inst_results(call).to_vec();
        self.stack.extend(results);

        Ok(())
    }

    fn global_get(&mut self, index: u32) -> Result<(), String> {
        let global = &self.module.globals[index as usize];
        let ty = value_type(global.ty)?;
        let offset = abi::GLOBALS + 8 * index as i32;
        let value = match (global.constant, ty) {
            (Some(bits), F32) => self.b.ins().f32const(Ieee32::with_bits(bits as u32)),
            (Some(bits), F64) => self.b.ins().f64const(Ieee64::with_bits(bits)),
            (Some(bits), _) => self.b.ins().iconst(ty, bits as i64),
            (None, _) if !global.mutable => {
                self.b
                    .ins()
                    .load(ty, self.vmctx_flags.with_readonly().with_can_move(), self.vmctx, offset)
            }
            (None, _) => self.vmctx_load(ty, offset),
        };
        self.push(value);

        Ok(())
    }

    fn global_set(&mut self, index: u32) {
        let value = self.pop();
        let offset = abi::GLOBALS + 8 * index as i32;
        self.b.ins().store(self.vmctx_flags, value, self.vmctx, offset);
    }

    fn open(&mut self, kind: Kind, target: Block, end: Block, params: usize, results: usize) {
        let height = self.stack.len() - params;
        self.control.push(Frame {
            kind,
            target,
            end,
            params,
            results,
            height,
            reached: false,
        });
    }

    fn operator(&mut self, op: &Operator) -> Result<(), String> {
        if !self.reachable {
            return self.unreachable_operator(op);
        }

        match *op {
            Operator::Nop => {}
            Operator::Unreachable => self.trap_now(Trap::Unreachable),
            Operator::Block { blockty } => {
                let (params, results) = self.block_type(blockty)?;
                let end = self.block_with_params(&results);
                self.open(Kind::Block, end, end, params.len(), results.len());
            }
            Operator::Loop { blockty } => {
                let (params, results) = self.block_type(blockty)?;
                let header = self.block_with_params(&params);
                let end = self.block_with_params(&results);
                let values = self.popn(params.len());
                self.jump(header, &values);
                // The header is sealed at `end`, once every back edge is known.
                self.b.switch_to_block(header);
                let header_params = self.b.block_params(header).to_vec();
                self.stack.extend(header_params);
                self.open(Kind::Loop, header, end, params.len(), results.len());
                self.stack_check();
            }
            Operator::If { blockty } => {
                let (params, results) = self.block_type(blockty)?;
                let condition = self.pop();
                let then = self.b.create_block();
                let otherwise = self.block_with_params(&params);
                let end = self.block_with_params(&results);
                let values = self.top(params.len());
                self.b.ins().brif(condition, then, &[], otherwise, &args(&values));
                self.b.seal_block(otherwise);
                self.enter(then);
                self.open(
                    Kind::If {
                        otherwise,
                        has_else: false,
                    },
                    end,
                    end,
                    params.len(),
                    results.len(),
                );
            }
            Operator::Else => self.else_()?,
            Operator::End => self.end(),
            Operator::Br { relative_depth } => {
                let (target, arity) = self.branch_target(relative_depth);
                let values = self.top(arity);
                self.jump(target, &values);
                self.reachable = false;
            }
            Operator::BrIf { relative_depth } => {
                let condition = self.pop();
                let (target, arity) = self.branch_target(relative_depth);
                let values = self.top(arity);
                let next = self.b.create_block();
                self.b.ins().brif(condition, target, &args(&values), next, &[]);
                self.enter(next);
            }
            Operator::BrTable { ref targets } => self.br_table(targets)?,
            Operator::Return => {
                let (target, arity) = self.branch_target(self.control.len() as u32 - 1);
                let values = self.top(arity);
                self.jump(target, &values);
                self.reachable = false;
            }
            Operator::Call { function_index } => self.call(function_index)?,
            Operator::CallIndirect {
                type_index,
                table_index: 0,
            } => self.call_indirect(type_index)?,
            Operator::Drop => {
                self.pop();
            }
            Operator::Select | Operator::TypedSelect { .. } => {
                let condition = self.pop();
                let (lhs, rhs) = self.pop2();
                let result = self.b.ins().select(condition, lhs, rhs);
                self.push(result);
            }
            Operator::LocalGet { local_index } => {
                let value = self.b.use_var(self.locals[local_index as usize]);
                self.push(value);
            }
            Operator::LocalSet { local_index } => {
                let value = self.pop();
                self.b.def_var(self.locals[local_index as usize], value);
            }
            Operator::LocalTee { local_index } => {
                let value = *self.stack.last().expect("validated: local.tee operand");
                self.b.def_var(self.locals[local_index as usize], value);
            }
            Operator::GlobalGet { global_index } => self.global_get(global_index)?,
            Operator::GlobalSet { global_index } => self.global_set(global_index),

            Operator::I32Load { ref memarg } => self.load(memarg, 4, |b, f, a, o| b.ins().load(I32, f, a, o)),
            Operator::I64Load { ref memarg } => self.load(memarg, 8, |b, f, a, o| b.ins().load(I64, f, a, o)),
            Operator::F32Load { ref memarg } => self.load(memarg, 4, |b, f, a, o| b.ins().load(F32, f, a, o)),
            Operator::F64Load { ref memarg } => self.load(memarg, 8, |b, f, a, o| b.ins().load(F64, f, a, o)),
            Operator::I32Load8S { ref memarg } => self.load(memarg, 1, |b, f, a, o| b.ins().sload8(I32, f, a, o)),
            Operator::I32Load8U { ref memarg } => self.load(memarg, 1, |b, f, a, o| b.ins().uload8(I32, f, a, o)),
            Operator::I32Load16S { ref memarg } => self.load(memarg, 2, |b, f, a, o| b.ins().sload16(I32, f, a, o)),
            Operator::I32Load16U { ref memarg } => self.load(memarg, 2, |b, f, a, o| b.ins().uload16(I32, f, a, o)),
            Operator::I64Load8S { ref memarg } => self.load(memarg, 1, |b, f, a, o| b.ins().sload8(I64, f, a, o)),
            Operator::I64Load8U { ref memarg } => self.load(memarg, 1, |b, f, a, o| b.ins().uload8(I64, f, a, o)),
            Operator::I64Load16S { ref memarg } => self.load(memarg, 2, |b, f, a, o| b.ins().sload16(I64, f, a, o)),
            Operator::I64Load16U { ref memarg } => self.load(memarg, 2, |b, f, a, o| b.ins().uload16(I64, f, a, o)),
            Operator::I64Load32S { ref memarg } => self.load(memarg, 4, |b, f, a, o| b.ins().sload32(f, a, o)),
            Operator::I64Load32U { ref memarg } => self.load(memarg, 4, |b, f, a, o| b.ins().uload32(f, a, o)),
            Operator::I32Store { ref memarg } | Operator::F32Store { ref memarg } => {
                self.store(memarg, 4, |b, f, v, a, o| {
                    b.ins().store(f, v, a, o);
                })
            }
            Operator::I64Store { ref memarg } | Operator::F64Store { ref memarg } => {
                self.store(memarg, 8, |b, f, v, a, o| {
                    b.ins().store(f, v, a, o);
                })
            }
            Operator::I32Store8 { ref memarg } | Operator::I64Store8 { ref memarg } => {
                self.store(memarg, 1, |b, f, v, a, o| {
                    b.ins().istore8(f, v, a, o);
                })
            }
            Operator::I32Store16 { ref memarg } | Operator::I64Store16 { ref memarg } => {
                self.store(memarg, 2, |b, f, v, a, o| {
                    b.ins().istore16(f, v, a, o);
                })
            }
            Operator::I64Store32 { ref memarg } => self.store(memarg, 4, |b, f, v, a, o| {
                b.ins().istore32(f, v, a, o);
            }),
            Operator::MemorySize { mem: 0 } => {
                let size = self.vmctx_load(I64, abi::MEMORY_SIZE);
                let pages = self.b.ins().ushr_imm_s(size, 16);
                let pages = self.b.ins().ireduce(I32, pages);
                self.push(pages);
            }
            Operator::MemoryGrow { mem: 0 } => {
                let delta = self.pop();
                let result = self.helper(Helper::MemoryGrow, &[delta]).expect("memory_grow returns");
                self.push(result);
            }
            Operator::MemoryCopy { dst_mem: 0, src_mem: 0 } => {
                let values = self.popn(3);
                self.helper(Helper::MemoryCopy, &values);
            }
            Operator::MemoryFill { mem: 0 } => {
                let values = self.popn(3);
                self.helper(Helper::MemoryFill, &values);
            }
            Operator::MemoryInit { data_index, mem: 0 } => {
                let segment = self.b.ins().iconst(I32, data_index as i64);
                let mut values = vec![segment];
                values.extend(self.popn(3));
                self.helper(Helper::MemoryInit, &values);
            }
            Operator::DataDrop { data_index } => {
                let segment = self.b.ins().iconst(I32, data_index as i64);
                self.helper(Helper::DataDrop, &[segment]);
            }

            Operator::I32Const { value } => {
                let value = self.b.ins().iconst(I32, value as i64);
                self.push(value);
            }
            Operator::I64Const { value } => {
                let value = self.b.ins().iconst(I64, value);
                self.push(value);
            }
            Operator::F32Const { value } => {
                let value = self.b.ins().f32const(Ieee32::with_bits(value.bits()));
                self.push(value);
            }
            Operator::F64Const { value } => {
                let value = self.b.ins().f64const(Ieee64::with_bits(value.bits()));
                self.push(value);
            }

            Operator::I32Eqz | Operator::I64Eqz => self.eqz(),
            Operator::I32Eq | Operator::I64Eq => self.icmp(IntCC::Equal),
            Operator::I32Ne | Operator::I64Ne => self.icmp(IntCC::NotEqual),
            Operator::I32LtS | Operator::I64LtS => self.icmp(IntCC::SignedLessThan),
            Operator::I32LtU | Operator::I64LtU => self.icmp(IntCC::UnsignedLessThan),
            Operator::I32GtS | Operator::I64GtS => self.icmp(IntCC::SignedGreaterThan),
            Operator::I32GtU | Operator::I64GtU => self.icmp(IntCC::UnsignedGreaterThan),
            Operator::I32LeS | Operator::I64LeS => self.icmp(IntCC::SignedLessThanOrEqual),
            Operator::I32LeU | Operator::I64LeU => self.icmp(IntCC::UnsignedLessThanOrEqual),
            Operator::I32GeS | Operator::I64GeS => self.icmp(IntCC::SignedGreaterThanOrEqual),
            Operator::I32GeU | Operator::I64GeU => self.icmp(IntCC::UnsignedGreaterThanOrEqual),
            Operator::F32Eq | Operator::F64Eq => self.fcmp(FloatCC::Equal),
            Operator::F32Ne | Operator::F64Ne => self.fcmp(FloatCC::NotEqual),
            Operator::F32Lt | Operator::F64Lt => self.fcmp(FloatCC::LessThan),
            Operator::F32Gt | Operator::F64Gt => self.fcmp(FloatCC::GreaterThan),
            Operator::F32Le | Operator::F64Le => self.fcmp(FloatCC::LessThanOrEqual),
            Operator::F32Ge | Operator::F64Ge => self.fcmp(FloatCC::GreaterThanOrEqual),

            Operator::I32Clz | Operator::I64Clz => self.unary(|b, v| b.ins().clz(v)),
            Operator::I32Ctz | Operator::I64Ctz => self.unary(|b, v| b.ins().ctz(v)),
            Operator::I32Popcnt | Operator::I64Popcnt => self.unary(|b, v| b.ins().popcnt(v)),
            Operator::I32Add | Operator::I64Add => self.binary(|b, l, r| b.ins().iadd(l, r)),
            Operator::I32Sub | Operator::I64Sub => self.binary(|b, l, r| b.ins().isub(l, r)),
            Operator::I32Mul | Operator::I64Mul => self.binary(|b, l, r| b.ins().imul(l, r)),
            Operator::I32DivS => self.divide(I32, true, false),
            Operator::I32DivU => self.divide(I32, false, false),
            Operator::I32RemS => self.divide(I32, true, true),
            Operator::I32RemU => self.divide(I32, false, true),
            Operator::I64DivS => self.divide(I64, true, false),
            Operator::I64DivU => self.divide(I64, false, false),
            Operator::I64RemS => self.divide(I64, true, true),
            Operator::I64RemU => self.divide(I64, false, true),
            Operator::I32And | Operator::I64And => self.binary(|b, l, r| b.ins().band(l, r)),
            Operator::I32Or | Operator::I64Or => self.binary(|b, l, r| b.ins().bor(l, r)),
            Operator::I32Xor | Operator::I64Xor => self.binary(|b, l, r| b.ins().bxor(l, r)),
            // Cranelift masks shift amounts by the type width, as wasm does.
            Operator::I32Shl | Operator::I64Shl => self.binary(|b, l, r| b.ins().ishl(l, r)),
            Operator::I32ShrS | Operator::I64ShrS => self.binary(|b, l, r| b.ins().sshr(l, r)),
            Operator::I32ShrU | Operator::I64ShrU => self.binary(|b, l, r| b.ins().ushr(l, r)),
            Operator::I32Rotl | Operator::I64Rotl => self.binary(|b, l, r| b.ins().rotl(l, r)),
            Operator::I32Rotr | Operator::I64Rotr => self.binary(|b, l, r| b.ins().rotr(l, r)),

            Operator::F32Abs | Operator::F64Abs => self.unary(|b, v| b.ins().fabs(v)),
            Operator::F32Neg | Operator::F64Neg => self.unary(|b, v| b.ins().fneg(v)),
            Operator::F32Ceil | Operator::F64Ceil => self.unary(|b, v| b.ins().ceil(v)),
            Operator::F32Floor | Operator::F64Floor => self.unary(|b, v| b.ins().floor(v)),
            Operator::F32Trunc | Operator::F64Trunc => self.unary(|b, v| b.ins().trunc(v)),
            Operator::F32Nearest | Operator::F64Nearest => self.unary(|b, v| b.ins().nearest(v)),
            Operator::F32Sqrt | Operator::F64Sqrt => self.unary(|b, v| b.ins().sqrt(v)),
            Operator::F32Add | Operator::F64Add => self.binary(|b, l, r| b.ins().fadd(l, r)),
            Operator::F32Sub | Operator::F64Sub => self.binary(|b, l, r| b.ins().fsub(l, r)),
            Operator::F32Mul | Operator::F64Mul => self.binary(|b, l, r| b.ins().fmul(l, r)),
            Operator::F32Div | Operator::F64Div => self.binary(|b, l, r| b.ins().fdiv(l, r)),
            // Cranelift's fmin and fmax propagate NaN and order -0 below +0, as wasm does.
            Operator::F32Min | Operator::F64Min => self.binary(|b, l, r| b.ins().fmin(l, r)),
            Operator::F32Max | Operator::F64Max => self.binary(|b, l, r| b.ins().fmax(l, r)),
            Operator::F32Copysign | Operator::F64Copysign => self.binary(|b, l, r| b.ins().fcopysign(l, r)),

            Operator::I32WrapI64 => self.unary(|b, v| b.ins().ireduce(I32, v)),
            Operator::I64ExtendI32S => self.unary(|b, v| b.ins().sextend(I64, v)),
            Operator::I64ExtendI32U => self.unary(|b, v| b.ins().uextend(I64, v)),
            Operator::I32TruncF32S | Operator::I32TruncF64S => self.truncate(I32, true),
            Operator::I32TruncF32U | Operator::I32TruncF64U => self.truncate(I32, false),
            Operator::I64TruncF32S | Operator::I64TruncF64S => self.truncate(I64, true),
            Operator::I64TruncF32U | Operator::I64TruncF64U => self.truncate(I64, false),
            Operator::I32TruncSatF32S | Operator::I32TruncSatF64S => {
                self.unary(|b, v| b.ins().fcvt_to_sint_sat(I32, v))
            }
            Operator::I32TruncSatF32U | Operator::I32TruncSatF64U => {
                self.unary(|b, v| b.ins().fcvt_to_uint_sat(I32, v))
            }
            Operator::I64TruncSatF32S | Operator::I64TruncSatF64S => {
                self.unary(|b, v| b.ins().fcvt_to_sint_sat(I64, v))
            }
            Operator::I64TruncSatF32U | Operator::I64TruncSatF64U => {
                self.unary(|b, v| b.ins().fcvt_to_uint_sat(I64, v))
            }
            Operator::F32ConvertI32S | Operator::F32ConvertI64S => self.unary(|b, v| b.ins().fcvt_from_sint(F32, v)),
            Operator::F32ConvertI32U | Operator::F32ConvertI64U => self.unary(|b, v| b.ins().fcvt_from_uint(F32, v)),
            Operator::F64ConvertI32S | Operator::F64ConvertI64S => self.unary(|b, v| b.ins().fcvt_from_sint(F64, v)),
            Operator::F64ConvertI32U | Operator::F64ConvertI64U => self.unary(|b, v| b.ins().fcvt_from_uint(F64, v)),
            Operator::F32DemoteF64 => self.unary(|b, v| b.ins().fdemote(F32, v)),
            Operator::F64PromoteF32 => self.unary(|b, v| b.ins().fpromote(F64, v)),
            Operator::I32ReinterpretF32 => self.unary(|b, v| b.ins().bitcast(I32, MemFlagsData::new(), v)),
            Operator::I64ReinterpretF64 => self.unary(|b, v| b.ins().bitcast(I64, MemFlagsData::new(), v)),
            Operator::F32ReinterpretI32 => self.unary(|b, v| b.ins().bitcast(F32, MemFlagsData::new(), v)),
            Operator::F64ReinterpretI64 => self.unary(|b, v| b.ins().bitcast(F64, MemFlagsData::new(), v)),
            Operator::I32Extend8S => self.extend(I8, I32),
            Operator::I32Extend16S => self.extend(I16, I32),
            Operator::I64Extend8S => self.extend(I8, I64),
            Operator::I64Extend16S => self.extend(I16, I64),
            Operator::I64Extend32S => self.extend(I32, I64),

            ref other => return Err(format!("unsupported operator {other:?}")),
        }

        Ok(())
    }

    /// Skip code after an unconditional branch, tracking nesting so the
    /// `else` or `end` of a live frame is still seen.
    fn unreachable_operator(&mut self, op: &Operator) -> Result<(), String> {
        match op {
            Operator::Block { .. } | Operator::Loop { .. } | Operator::If { .. } => self.dead += 1,
            Operator::Else if self.dead == 0 => self.else_()?,
            Operator::End if self.dead == 0 => self.end(),
            Operator::End => self.dead -= 1,
            _ => {}
        }

        Ok(())
    }

    fn else_(&mut self) -> Result<(), String> {
        let index = self.control.len() - 1;
        let frame = &mut self.control[index];
        let Kind::If {
            otherwise,
            ref mut has_else,
        } = frame.kind
        else {
            return Err("else outside if".into());
        };
        *has_else = true;
        let (end, results, height) = (frame.end, frame.results, frame.height);

        if self.reachable {
            self.control[index].reached = true;
            let values = self.top(results);
            self.jump(end, &values);
        }

        self.stack.truncate(height);
        // `otherwise` was sealed at the `if`; its only predecessor is the brif.
        self.b.switch_to_block(otherwise);
        let params = self.b.block_params(otherwise).to_vec();
        self.stack.extend(params);
        self.reachable = true;

        Ok(())
    }

    fn end(&mut self) {
        let mut frame = self.control.pop().expect("validated: end of a frame");

        if self.reachable {
            frame.reached = true;
            let values = self.top(frame.results);
            self.jump(frame.end, &values);
        }

        match frame.kind {
            Kind::Loop => self.b.seal_block(frame.target),
            Kind::If {
                otherwise,
                has_else: false,
            } => {
                // No else: the false edge carries the parameters straight to the end.
                frame.reached = true;
                self.b.switch_to_block(otherwise);
                let params = self.b.block_params(otherwise).to_vec();
                self.jump(frame.end, &params);
            }
            _ => {}
        }

        self.stack.truncate(frame.height);
        self.enter(frame.end);
        let results = self.b.block_params(frame.end).to_vec();
        self.reachable = frame.reached;

        if let Kind::Function = frame.kind {
            if self.reachable {
                self.b.ins().return_(&results);
            } else {
                self.b.ins().trap(TrapCode::unwrap_user(1));
            }
        } else {
            self.stack.extend(results);
        }
    }

    fn br_table(&mut self, targets: &wasmparser::BrTable) -> Result<(), String> {
        let index = self.pop();
        let depths = targets
            .targets()
            .collect::<Result<Vec<u32>, _>>()
            .map_err(|e| e.to_string())?;
        let (default, arity) = self.branch_target(targets.default());
        let values = self.top(arity);
        let mut blocks = Vec::with_capacity(depths.len());

        for depth in &depths {
            blocks.push(self.branch_target(*depth).0);
        }

        // A jump table entry carries no arguments: values go through a
        // trampoline block per distinct target.
        let mut trampolines: BTreeMap<Block, Block> = BTreeMap::new();

        if arity > 0 {
            for block in blocks.iter().chain(core::iter::once(&default)) {
                if !trampolines.contains_key(block) {
                    let trampoline = self.b.create_block();
                    trampolines.insert(*block, trampoline);
                }
            }
        }

        let via = |block: &Block| trampolines.get(block).copied().unwrap_or(*block);
        let pool = &mut self.b.func.dfg.value_lists;
        let entries: Vec<BlockCall> = blocks
            .iter()
            .map(|block| BlockCall::new(via(block), [], pool))
            .collect();
        let default_call = BlockCall::new(via(&default), [], pool);
        let table = self.b.create_jump_table(JumpTableData::new(default_call, &entries));
        self.b.ins().br_table(index, table);
        for (target, trampoline) in trampolines {
            self.enter(trampoline);
            self.jump(target, &values);
        }

        self.reachable = false;

        Ok(())
    }
}
