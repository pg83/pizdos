//go:build !(linux && amd64)

package main

// Loading native code needs linux/amd64; the compiler alone builds anywhere,
// wasip1 included.
func runObject(data []byte, argv []string) int {
	throwFmt("run needs linux/amd64")

	return 1
}
