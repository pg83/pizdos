package main

// The instance is one block of memory the loader allocates, zeroed, of the
// size the object's metadata names. Its fixed header comes first; the
// module's globals, tables, function descriptors and segment flags follow at
// the offsets of the metadata.
const (
	offMem        = 0  // base of linear memory
	offMemSize    = 8  // size of linear memory in bytes
	offMemBound   = 16 // accessible memory end - boundSlack: the code keeps it in R13 (mCheck)
	offStackLimit = 24 // the most bytes of goroutine stack the code may use; beyond, it traps
	offHost       = 32 // free for the loader
	offTokens     = 40 // the last invocation token handed out: see jumpFrame
	offJump       = 48 // the jump frame, jumpWords words
	offBase       = offJump + 8*jumpWords
	offInterrupt  = offBase + 8 // non-zero: a return from an import calls the interrupt stub (goabi.go)
	headerSize    = offInterrupt + 8
)

// instanceDistance is how far past the instance's block its memory lies,
// in one mapping (newRegion): the code keeps the memory's base in R15 and
// reaches the instance that far below it.
const instanceDistance = 1 << 30

// gInstance is the offset in g of the instance the goroutine runs, a field
// our runtime adds (lib/goruntime): the code reads it after a call of Go,
// the entry thunks set it. checkGo verifies it.
const gInstance = 472

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
	jumpRegs  = 3 // the five callee-saved registers: RBX, R9, R10, R11, R12
	jumpToken = 8
	jumpWords = 9
)

// jumpSaved is how many callee-saved registers the jump frame keeps.
const jumpSaved = 5

// boundSlack is how far past a checked address an access may reach without
// a check of its own: an access at address a, offset o and width n is in
// bounds when a <= size - boundSlack and o + n <= boundSlack. The memory
// has boundSlack accessible bytes before it too, for an access at a + o
// with -boundSlack <= o < 0 addressed from a in 64 bits.
const boundSlack = 1024

// A profiled object's instance ends in the tail profile's counters, one of
// 8 bytes for each site the object lists (tail_profile.go).
const profileCounterBytes = 8

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
