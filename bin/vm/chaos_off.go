//go:build !pizdoschaos

package main

// The compiler inlines these away in production builds, as in monorepo/ay.
func chaosFail(string) bool {
	return false
}

func setupVmChaos() {}

func setupCompilerChaos(*Instance) {}
