"""wasm2o's fork of Go's runtime, for the Go of build.py (1.25): the files
of this directory join package runtime, and EDITS change these of
$GOROOT/src/runtime, each anchor found exactly once. dev/go.py builds
with them as an overlay.
"""

EDITS = [
    ("runtime2.go",
     "\tvalgrindStackID uintptr\n}\n",
     "\tvalgrindStackID uintptr\n\n\t// interrupt is wasm2o's (interrupt.go).\n\tinterrupt gInterrupt\n}\n"),
    ("signal_unix.go",
     "\t// Check if this G wants to be preempted and is safe to\n\t// preempt.\n\tif wantAsyncPreempt(gp) {\n",
     "\t// Check if this G wants to be preempted and is safe to\n\t// preempt; an interruption goes first.\n\tif interruptG(gp, ctxt) {\n\t} else if wantAsyncPreempt(gp) {\n"),
]
