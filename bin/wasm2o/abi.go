package main

// The instance is one block of memory the loader allocates, zeroed, of the
// size the object's metadata names. Its fixed header comes first; the
// module's globals, tables, function descriptors and segment flags follow at
// the offsets of the metadata.
const (
	offMem        = 0  // base of linear memory
	offMemSize    = 8  // size of linear memory in bytes
	offMemBound   = 16 // accessible memory end - boundSlack: see mCheck
	offStackLimit = 24 // the most bytes of goroutine stack the code may use; beyond, it traps
	offHost       = 32 // free for the loader
	offTokens     = 40 // the last invocation token handed out: see jumpFrame
	offJump       = 48 // the jump frame, jumpWords words
	offBase       = offJump + 8*jumpWords
	offInterrupt  = offBase + 8 // non-zero: a return from an import calls the interrupt stub (goabi.go)
	headerSize    = offInterrupt + 8
)

// The jump frame is what the adapter of an import that returns twice
// (goabi.go) writes before it calls Go: the caller's frame as a later
// landing restores it. Stack addresses are offsets below g.stack.hi less
// the base, which stack copies keep; the base is 0 but in a child of fork,
// whose frames are the parent's in another place: there it keeps the
// parent's offsets right. The token tells the caller's invocation from a
// later one in the same place: a function calling such an import keeps it
// in the word below its saved RBP, zero from its prologue until the first
// call.
const (
	jumpPC    = 0 // the return address
	jumpSP    = 1 // the stack pointer after the return
	jumpBP    = 2 // the caller's RBP
	jumpR10   = 3 // the callee-saved registers
	jumpR11   = 4
	jumpR12   = 5
	jumpToken = 6
	jumpWords = 7
)

// boundSlack is how far past a checked address an access may reach without
// a check of its own: an access at address a, offset o and width n is in
// bounds when a <= size - boundSlack and o + n <= boundSlack.
const boundSlack = 1 << 16

// Diagnostic counters per function: guards, fallbacks, omitted checks,
// and ordinary checks that actually execute. Only profiled objects have them.
const tailCounterBytes = 32

// Each table is {entries, size, max}: entries is an array of descriptor
// pointers, null for a null reference.
const (
	tableEntries = 0
	tableSize    = 8
	tableMax     = 12
	tableBytes   = 16
)

// A function descriptor is {code, type id}; a funcref is its address.
const descBytes = 16
