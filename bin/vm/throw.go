package main

import "fmt"

type Exception struct{ cause error }

func (e *Exception) Error() string { return e.message() }

func (e *Exception) message() string { return e.cause.Error() }

func throw(err error) {
	if err != nil {
		panic(&Exception{err})
	}
}

func throw2[T any](value T, err error) T {
	throw(err)

	return value
}

func throw3[T, U any](value T, other U, err error) (T, U) {
	throw(err)

	return value, other
}

func throwFmt(format string, args ...any) {
	throw(fmt.Errorf(format, args...))
}

// try runs cb and returns the Exception it threw, letting any other panic
// propagate.
func try(cb func()) *Exception {
	return tryAs[*Exception](cb)
}

// tryAs catches panics of T and lets every other panic propagate. Exception
// boundaries and Wasm trap boundaries use the same recovery policy.
func tryAs[T any](cb func()) (caught T) {
	defer func() {
		if e := recover(); e != nil {
			value, ok := e.(T)

			if !ok {
				panic(e)
			}

			caught = value
		}
	}()

	cb()

	return
}
