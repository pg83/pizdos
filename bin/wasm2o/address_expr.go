package main

// Affine address identities are over the low 32 bits. They preserve i32
// wrapping and the low bits of i64 arithmetic followed by i32.wrap_i64.
// A value with a more complicated expression stays its own opaque atom.
type AddressTerm struct {
	value int32
	mul   uint32
}

type AddressExpression struct {
	terms  [4]AddressTerm
	offset uint32
}

type AddressExpressions struct {
	f     *Function
	cache []AddressExpression
	seen  []bool
}

func addressAtom(v int32) AddressExpression {
	return AddressExpression{terms: [4]AddressTerm{{v, 1}}}
}

func addAddressExpression(a, b AddressExpression, factor uint32) (AddressExpression, bool) {
	out := AddressExpression{offset: a.offset + factor*b.offset}

	for _, source := range []struct {
		expr AddressExpression
		mul  uint32
	}{{a, 1}, {b, factor}} {
		for _, term := range source.expr.terms {
			term.mul *= source.mul

			if term.mul == 0 {
				continue
			}

			at := -1

			for j, prior := range out.terms {
				if prior.mul != 0 && prior.value == term.value {
					at = j

					break
				}
			}

			if at < 0 {
				for j, prior := range out.terms {
					if prior.mul == 0 {
						at = j
						out.terms[j].value = term.value

						break
					}
				}
			}

			if at < 0 {
				return AddressExpression{}, false
			}

			out.terms[at].mul += term.mul
		}
	}

	// Canonicalize the few terms without a map or a heap-backed sort.
	n := 0

	for j := range out.terms {
		if out.terms[j].mul != 0 {
			out.terms[n] = out.terms[j]
			n++
		}
	}

	for j := n; j < len(out.terms); j++ {
		out.terms[j] = AddressTerm{}
	}

	for j := 1; j < n; j++ {
		for k := j; k > 0 && out.terms[k].value < out.terms[k-1].value; k-- {
			out.terms[k], out.terms[k-1] = out.terms[k-1], out.terms[k]
		}
	}

	return out, true
}

func (r *AddressExpressions) of(v int32, depth int) AddressExpression {
	if r.seen[v] {
		return r.cache[v]
	}

	f := r.f
	val := f.values[v]
	out := addressAtom(v)
	r.seen[v], r.cache[v] = true, out

	if depth > 32 || val.typ != I32 && val.typ != I64 {
		return out
	}

	args := f.args(v)

	switch val.op {
	case OpConst:
		out = AddressExpression{offset: uint32(val.aux)}
	case OpWrap, OpExtendU, OpExtendS:
		out = r.of(args[0], depth+1)
	case OpAdd, OpSub:
		factor := uint32(1)

		if val.op == OpSub {
			factor = 0xffffffff
		}

		if sum, ok := addAddressExpression(r.of(args[0], depth+1), r.of(args[1], depth+1), factor); ok {
			out = sum
		}
	case OpMul, OpShl:
		a, k := args[0], f.values[args[1]]

		if val.op == OpMul && k.op != OpConst {
			a, k = args[1], f.values[args[0]]
		}

		if k.op == OpConst {
			factor := uint32(k.aux)

			if val.op == OpShl {
				shift := uint64(k.aux) & uint64(typeSize(val.typ)*8-1)
				factor = 0

				if shift < 32 {
					factor = 1 << shift
				}
			}

			if product, ok := addAddressExpression(AddressExpression{}, r.of(a, depth+1), factor); ok {
				out = product
			}
		}
	}

	r.cache[v] = out

	return out
}
