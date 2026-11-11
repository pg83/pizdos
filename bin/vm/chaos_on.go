//go:build pizdoschaos

package main

import (
	"os"
	"strconv"
	"strings"
	"sync"
)

// Rules are deterministic one-shot failures after N successful visits.
// State lives in the host, and a rule is spent before a resource is acquired.
type ChaosRule struct {
	name string
	skip uint32
}

var chaosState struct {
	mu    sync.Mutex
	rules map[string]uint32
}

func chaosRules(text string, points map[string]uint32) []ChaosRule {
	var rules []ChaosRule
	seen := map[string]bool{}

	if text == "" {
		return rules
	}

	for _, field := range strings.Split(text, ",") {
		name, count, ok := strings.Cut(field, "@")
		n, err := strconv.ParseUint(count, 10, 32)
		_, known := points[name]

		if !ok || err != nil || !known || seen[name] {
			throwFmt("invalid chaos rule %q", field)
		}

		seen[name] = true
		rules = append(rules, ChaosRule{name, uint32(n)})
	}

	return rules
}

func setupVmChaos() {
	points := map[string]uint32{"mmap": 0, "compile-read": 0, "compile-write": 0, "memory-grow": 0,
		"openat": 0, "read": 0, "write": 0, "fork": 0}
	chaosState.rules = map[string]uint32{}

	for _, rule := range chaosRules(os.Getenv("PIZDOS_VM_CHAOS"), points) {
		chaosState.rules[rule.name] = rule.skip
	}
}

func chaosFail(point string) bool {
	chaosState.mu.Lock()

	defer chaosState.mu.Unlock()

	n, ok := chaosState.rules[point]

	if !ok {
		return false
	}

	if n > 0 {
		chaosState.rules[point] = n - 1

		return false
	}

	delete(chaosState.rules, point)

	return true
}

func setupCompilerChaos(i *Instance) {
	points := map[string]uint32{"read": 0, "write": 1, "grow": 2}
	rules := chaosRules(os.Getenv("PIZDOS_AOT_CHAOS"), points)

	for _, rule := range rules {
		entry, _ := i.export("chaosConfigure")
		entry.Interface().(func(uintptr, uint32, uint32))(i.ctx(), points[rule.name], rule.skip)
	}
}
