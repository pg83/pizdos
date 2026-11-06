//! aot as a host tool: files in and out, functions compiled on all cores.
//!
//!     aot <module.wasm> <module.aot>

use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use aot::{Compiled, Compiler, Scratch};

fn compile(compiler: &Compiler, jobs: usize) -> Result<Vec<Compiled>, String> {
    let count = compiler.functions();
    let next = AtomicUsize::new(0);
    let results: Vec<Mutex<Option<Result<Compiled, String>>>> = (0..count).map(|_| Mutex::new(None)).collect();

    std::thread::scope(|scope| {
        for _ in 0..jobs.max(1) {
            scope.spawn(|| {
                let mut scratch = Scratch::default();

                loop {
                    let slot = next.fetch_add(1, Ordering::Relaxed);

                    if slot >= count {
                        break;
                    }

                    *results[slot].lock().unwrap() = Some(compiler.function(slot, &mut scratch));
                }
            });
        }
    });

    results
        .into_iter()
        .map(|slot| slot.into_inner().unwrap().expect("every function compiled"))
        .collect()
}

fn run(input: &str, output: &str) -> Result<(), String> {
    let bytes = std::fs::read(input).map_err(|e| format!("{input}: {e}"))?;
    let compiler = Compiler::new(&bytes).map_err(|e| format!("{input}: {e}"))?;
    let jobs = std::env::var("BUILD_JOBS")
        .ok()
        .and_then(|jobs| jobs.parse().ok())
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()));
    let functions = compile(&compiler, jobs).map_err(|e| format!("{input}: {e}"))?;
    let artifact = compiler.artifact(&functions).map_err(|e| format!("{input}: {e}"))?;
    std::fs::write(output, artifact).map_err(|e| format!("{output}: {e}"))
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.len() != 3 {
        eprintln!("usage: aot <module.wasm> <module.aot>");
        std::process::exit(2);
    }

    if let Err(error) = run(&args[1], &args[2]) {
        eprintln!("aot: {error}");
        std::process::exit(1);
    }
}
