//go:build linux && amd64

package main

import (
	"crypto/md5"
	"crypto/sha256"
	"crypto/sha512"
	"strconv"
	"strings"
	"syscall"
)

// crypt of the schemes of a setting's prefix: $1$ MD5, $5$ SHA-256 and $6$
// SHA-512, by their reference algorithms, and traditional DES of a two
// character salt. bcrypt's $2 and the extended DES of _ are refused with
// EINVAL. The result is a static string, a heap block of the static area.

const cryptAlphabet = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

// to64 appends n characters of v, its low six bits first.
func to64(out []byte, v uint32, n int) []byte {
	for range n {
		out = append(out, cryptAlphabet[v&0x3f])
		v >>= 6
	}

	return out
}

func md5crypt(key, setting string) (string, bool) {
	salt := strings.TrimPrefix(setting, "$1$")

	if i := strings.IndexByte(salt, '$'); i >= 0 {
		salt = salt[:i]
	}

	salt = salt[:min(len(salt), 8)]
	pw := []byte(key)
	ctx := md5.New()
	ctx.Write(pw)
	ctx.Write([]byte("$1$" + salt))
	alt := md5.Sum([]byte(key + salt + key))

	for n := len(pw); n > 0; n -= 16 {
		ctx.Write(alt[:min(n, 16)])
	}

	for i := len(pw); i != 0; i >>= 1 {
		if i&1 != 0 {
			ctx.Write([]byte{0})
		} else {
			ctx.Write(pw[:1])
		}
	}

	final := ctx.Sum(nil)

	for i := range 1000 {
		round := md5.New()

		if i&1 != 0 {
			round.Write(pw)
		} else {
			round.Write(final)
		}

		if i%3 != 0 {
			round.Write([]byte(salt))
		}

		if i%7 != 0 {
			round.Write(pw)
		}

		if i&1 != 0 {
			round.Write(final)
		} else {
			round.Write(pw)
		}

		final = round.Sum(nil)
	}

	out := []byte("$1$" + salt + "$")

	for _, g := range [][3]int{{0, 6, 12}, {1, 7, 13}, {2, 8, 14}, {3, 9, 15}, {4, 10, 5}} {
		out = to64(out, uint32(final[g[0]])<<16|uint32(final[g[1]])<<8|uint32(final[g[2]]), 4)
	}

	return string(to64(out, uint32(final[11]), 2)), true
}

// shacrypt is Drepper's SHA-crypt: $5$ with SHA-256, $6$ with SHA-512.
func shacrypt(key, setting string, size int) (string, bool) {
	prefix := "$5$"
	newHash := sha256.New

	if size == 64 {
		prefix, newHash = "$6$", sha512.New
	}

	rest := strings.TrimPrefix(setting, prefix)
	rounds, custom := 5000, false

	if r, ok := strings.CutPrefix(rest, "rounds="); ok {
		digits, after, found := strings.Cut(r, "$")
		n, err := strconv.ParseUint(digits, 10, 64)

		if !found || err != nil {
			return "", false
		}

		rounds, custom, rest = int(min(max(n, 1000), 999999999)), true, after
	}

	salt := rest

	if i := strings.IndexByte(salt, '$'); i >= 0 {
		salt = salt[:i]
	}

	salt = salt[:min(len(salt), 16)]
	pw := []byte(key)
	sum := func(parts ...[]byte) []byte {
		h := newHash()

		for _, part := range parts {
			h.Write(part)
		}

		return h.Sum(nil)
	}
	repeat := func(b []byte, n int) []byte {
		out := make([]byte, 0, n)

		for len(out) < n {
			out = append(out, b[:min(len(b), n-len(out))]...)
		}

		return out
	}
	b := sum(pw, []byte(salt), pw)
	a := newHash()
	a.Write(pw)
	a.Write([]byte(salt))
	a.Write(repeat(b, len(pw)))

	for n := len(pw); n > 0; n >>= 1 {
		if n&1 != 0 {
			a.Write(b)
		} else {
			a.Write(pw)
		}
	}

	c := a.Sum(nil)
	dp := newHash()

	for range len(pw) {
		dp.Write(pw)
	}

	p := repeat(dp.Sum(nil), len(pw))
	ds := newHash()

	for range 16 + int(c[0]) {
		ds.Write([]byte(salt))
	}

	s := repeat(ds.Sum(nil), len(salt))

	for i := range rounds {
		h := newHash()

		if i&1 != 0 {
			h.Write(p)
		} else {
			h.Write(c)
		}

		if i%3 != 0 {
			h.Write(s)
		}

		if i%7 != 0 {
			h.Write(p)
		}

		if i&1 != 0 {
			h.Write(c)
		} else {
			h.Write(p)
		}

		c = h.Sum(nil)
	}

	out := []byte(prefix)

	if custom {
		out = append(out, "rounds="+strconv.Itoa(rounds)+"$"...)
	}

	out = append(out, salt+"$"...)
	word := func(x, y, z int) uint32 {
		return uint32(c[x])<<16 | uint32(c[y])<<8 | uint32(c[z])
	}

	if size == 32 {
		for i := range 10 {
			x := []int{0, 21, 12, 3, 24, 15, 6, 27, 18, 9}[i]
			y := []int{10, 1, 22, 13, 4, 25, 16, 7, 28, 19}[i]
			z := []int{20, 11, 2, 23, 14, 5, 26, 17, 8, 29}[i]
			out = to64(out, word(x, y, z), 4)
		}

		return string(to64(out, uint32(c[31])<<8|uint32(c[30]), 3)), true
	}

	for i := range 21 {
		x, y, z := (i*22)%63, (i*22+21)%63, (i*22+42)%63
		out = to64(out, word(x, y, z), 4)
	}

	return string(to64(out, uint32(c[63]), 2)), true
}

// The tables of DES.
var (
	desIP = [64]byte{58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
		57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7}
	desFP = [64]byte{40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
		36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25}
	desE = [48]byte{32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
		16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1}
	desP   = [32]byte{16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25}
	desPC1 = [56]byte{57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
		63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4}
	desPC2 = [48]byte{14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
		41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32}
	desShifts = [16]byte{1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1}
	desS      = [8][64]byte{
		{14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
			4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
		{15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
			0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
		{10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
			13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
		{7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
			10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
		{2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
			4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
		{12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
			9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
		{4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
			1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
		{13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
			7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
	}
)

// permute picks bits of an n-bit input, bit 1 its highest, by a table.
func permute(in uint64, n int, table []byte) uint64 {
	out := uint64(0)

	for _, b := range table {
		out = out<<1 | in>>(n-int(b))&1
	}

	return out
}

// descrypt is traditional crypt: DES of a zero block 25 times under the
// first eight characters of the key, the salt's twelve bits swapping bits of
// the expansion.
func descrypt(key, setting string) (string, bool) {
	if len(setting) < 2 {
		return "", false
	}

	saltBits := uint32(0)

	for i := range 2 {
		v := strings.IndexByte(cryptAlphabet, setting[i])

		if v < 0 {
			return "", false
		}

		saltBits |= uint32(v) << (6 * i)
	}

	k := uint64(0)

	for i := range 8 {
		c := byte(0)

		if i < len(key) {
			c = key[i]
		}

		k = k<<8 | uint64(c<<1)
	}

	cd := permute(k, 64, desPC1[:])
	c, d := cd>>28, cd&0xfffffff
	var subkeys [16]uint64

	for i, s := range desShifts {
		c = (c<<s | c>>(28-s)) & 0xfffffff
		d = (d<<s | d>>(28-s)) & 0xfffffff
		subkeys[i] = permute(c<<28|d, 56, desPC2[:])
	}

	block := uint64(0)

	for range 25 {
		lr := permute(block, 64, desIP[:])
		l, r := uint32(lr>>32), uint32(lr)

		for _, sk := range subkeys {
			e := permute(uint64(r), 32, desE[:])

			// bit i of the salt swaps bits i and i+24 of the expansion,
			// counting from its first.
			for i := range 12 {
				if saltBits>>i&1 != 0 {
					hi, lo := uint(47-i), uint(23-i)
					x := (e>>hi ^ e>>lo) & 1
					e ^= x<<hi | x<<lo
				}
			}

			e ^= sk
			s := uint64(0)

			for j := range 8 {
				six := e >> (42 - 6*j) & 0x3f
				row := six>>4&2 | six&1
				col := six >> 1 & 0xf
				s = s<<4 | uint64(desS[j][row*16+col])
			}

			l, r = r, l^uint32(permute(s, 32, desP[:]))
		}

		block = permute(uint64(r)<<32|uint64(l), 64, desFP[:])
	}

	out := []byte(setting[:2])

	for shift := 58; shift >= 0; shift -= 6 {
		out = append(out, cryptAlphabet[block>>uint(shift)&0x3f])
	}

	out = append(out, cryptAlphabet[block<<2&0x3f])

	return string(out), true
}

func (k *Kernel) cryptFunctions() map[string]any {
	return map[string]any{
		"crypt": libcCrypt,
	}
}

func libcCrypt(ctx uintptr, key, setting uint32) uint32 {
	p := proc(ctx)
	pw, s := p.str(key), p.str(setting)
	var out string
	ok := false

	switch {
	case strings.HasPrefix(s, "$1$"):
		out, ok = md5crypt(pw, s)
	case strings.HasPrefix(s, "$5$"):
		out, ok = shacrypt(pw, s, 32)
	case strings.HasPrefix(s, "$6$"):
		out, ok = shacrypt(pw, s, 64)
	case !strings.HasPrefix(s, "$") && !strings.HasPrefix(s, "_"):
		out, ok = descrypt(pw, s)
	}

	if !ok {
		p.setErrno(syscall.EINVAL)

		return 0
	}

	block := p.staticResult(staticCrypt, uint32(len(out))+1)

	if block != 0 {
		p.putString(block, out)
	}

	return block
}
