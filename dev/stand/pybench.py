"""The stand's Python workload: pure Python, run REPEAT times.

    python3 -S pybench.py REPEAT

It prints the sha256 of every result, which native and wasm2o must agree
on, and on standard error `time SECONDS` of the work alone, imports and
startup excluded. Results use no libm beyond sqrt, so they are the same
bits everywhere; PYTHONHASHSEED=0 fixes the order of sets.
"""
import hashlib
import itertools
import json
import re
import sys
import time


def nbody(steps):
    """The planets of the n-body benchmark, advanced by symplectic Euler."""
    pi = 3.14159265358979323
    mass = 4 * pi * pi
    days = 365.24
    bodies = [
        [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], mass],
        [[4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01],
         [1.66007664274403694e-03 * days, 7.69901118419740425e-03 * days, -6.90460016972063023e-05 * days], 9.54791938424326609e-04 * mass],
        [[8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01],
         [-2.76742510726862411e-03 * days, 4.99852801234917238e-03 * days, 2.30417297573763929e-05 * days], 2.85885980666130812e-04 * mass],
        [[1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01],
         [2.96460137564761618e-03 * days, 2.37847173959480950e-03 * days, -2.96589568540237556e-05 * days], 4.36624404335156298e-05 * mass],
        [[1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01],
         [2.68067772490389322e-03 * days, 1.62824170038242295e-03 * days, -9.51592254519715870e-05 * days], 5.15138902046611451e-05 * mass],
    ]
    pairs = [(bodies[i], bodies[j]) for i in range(len(bodies)) for j in range(i + 1, len(bodies))]

    for _ in range(steps):
        for ([x1, y1, z1], v1, m1), ([x2, y2, z2], v2, m2) in pairs:
            dx, dy, dz = x1 - x2, y1 - y2, z1 - z2
            d2 = dx * dx + dy * dy + dz * dz
            mag = 0.01 / (d2 * d2 ** 0.5)
            b1, b2 = m1 * mag, m2 * mag
            v1[0] -= dx * b2
            v1[1] -= dy * b2
            v1[2] -= dz * b2
            v2[0] += dx * b1
            v2[1] += dy * b1
            v2[2] += dz * b1

        for r, [vx, vy, vz], m in bodies:
            r[0] += 0.01 * vx
            r[1] += 0.01 * vy
            r[2] += 0.01 * vz

    return [b[0] for b in bodies]


def fannkuch(n):
    """The pancake flips of every permutation of n: the checksum and the
    most flips."""
    perm = list(range(n))
    count = list(range(1, n + 1))
    checksum = most = 0
    sign = 1

    while True:
        p = perm[:]
        flips = 0

        while p[0]:
            k = p[0]
            p[:k + 1] = p[k::-1]
            flips += 1

        most = max(most, flips)
        checksum += flips if sign > 0 else -flips

        if sign > 0:
            perm[0], perm[1] = perm[1], perm[0]
            sign = -1
        else:
            perm[1], perm[2] = perm[2], perm[1]
            sign = 1

            for i in range(2, n):
                if count[i]:
                    break

                count[i] = i + 1
                perm.insert(i + 1, perm.pop(0))
            else:
                return checksum, most

            count[i] -= 1


def spectral(n):
    """Power iterations of the infinite matrix's spectral norm."""
    def a(i, j):
        return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

    def times(u, transpose):
        if transpose:
            return [sum(a(j, i) * uj for j, uj in enumerate(u)) for i in range(n)]

        return [sum(a(i, j) * uj for j, uj in enumerate(u)) for i in range(n)]

    u = [1.0] * n

    for _ in range(8):
        v = times(times(u, False), True)
        u = times(times(v, False), True)

    vbv = sum(x * y for x, y in zip(u, v))
    vv = sum(x * x for x in v)

    return (vbv / vv) ** 0.5


class Vector:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x, self.y = x, y

    def __add__(self, other):
        return Vector(self.x + other.x, self.y + other.y)

    def scaled(self, k):
        return Vector(self.x * k, self.y * k)

    @property
    def norm2(self):
        return self.x * self.x + self.y * self.y


class Particle:
    def __init__(self, position, velocity):
        self.position, self.velocity = position, velocity

    def step(self, gravity):
        self.velocity = self.velocity + gravity
        self.position = self.position + self.velocity.scaled(0.5)

        if self.position.y < 0:
            self.position = Vector(self.position.x, -self.position.y)
            self.velocity = Vector(self.velocity.x, -self.velocity.y * 0.9)


def objects(count):
    """Method calls, properties and allocation of small objects."""
    gravity = Vector(0.0, -0.25)
    particles = [Particle(Vector(i * 1.5, 10.0 + i % 7), Vector(1.0 - i % 3, 0.0)) for i in range(count)]

    for _ in range(40):
        for p in particles:
            p.step(gravity)

    return sum(p.position.norm2 for p in particles)


WORDS = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"]


def text(lines):
    """Lines of words and numbers from a linear congruential generator."""
    state = 12345
    out = []

    for i in range(lines):
        words = []

        for _ in range(8):
            state = (state * 1103515245 + 12345) & 0x7fffffff
            words.append(WORDS[state % len(WORDS)] if state % 5 else str(state % 10000))

        out.append(f"{i}: " + " ".join(words))

    return "\n".join(out)


def strings(lines):
    """Splitting, counting, formatting and replacing."""
    data = text(lines)
    counts = {}

    for line in data.splitlines():
        for word in line.split()[1:]:
            counts[word] = counts.get(word, 0) + 1

    top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:20]
    upper = data.upper().replace("ALPHA", "A").count("A")

    return top, upper, len("|".join(f"{k}={v:05d}" for k, v in top))


def regexes(lines):
    """Patterns of numbers, words and their pairs."""
    data = text(lines)
    numbers = [int(n) for n in re.findall(r"\b\d{3,}\b", data)]
    pairs = re.findall(r"(\w+a) (\w+)", data)
    substituted = re.sub(r"(\d+): (\w+)", r"\2#\1", data)

    return sum(numbers), len(pairs), hashlib.sha256(substituted.encode()).hexdigest()


def documents(count):
    """JSON of nested records, dumped and loaded again."""
    records = [{"id": i, "name": f"item{i}", "tags": WORDS[i % 5:i % 5 + 4], "price": i * 1.25,
                "nested": {"flag": i % 2 == 0, "values": list(range(i % 9))}} for i in range(count)]
    total = 0

    for _ in range(5):
        data = json.dumps(records, sort_keys=True)
        back = json.loads(data)
        total += len(data) + sum(r["id"] for r in back)

    return total


def generators(n):
    """Generators, comprehensions and builtins over them."""
    def primes(limit):
        sieve = bytearray([1]) * (limit + 1)
        sieve[0:2] = b"\0\0"

        for i in range(2, int(limit ** 0.5) + 1):
            if sieve[i]:
                sieve[i * i::i] = bytes(len(range(i * i, limit + 1, i)))

        return (i for i, flag in enumerate(sieve) if flag)

    squares = sum(x * x for x in range(n) if x % 3)
    found = list(primes(n))
    grouped = {k: len(list(v)) for k, v in itertools.groupby(sorted(found, key=lambda p: p % 10), key=lambda p: p % 10)}

    return squares, len(found), grouped


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


WORKLOADS = [
    (nbody, 20000),
    (fannkuch, 8),
    (spectral, 120),
    (objects, 3000),
    (strings, 20000),
    (regexes, 12000),
    (documents, 3000),
    (generators, 1000000),
    (fib, 27),
]


def main():
    repeat = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    digest = hashlib.sha256()
    start = time.perf_counter()

    for _ in range(repeat):
        for work, arg in WORKLOADS:
            digest.update(repr(work(arg)).encode())

    elapsed = time.perf_counter() - start
    print(digest.hexdigest())
    print(f"time {elapsed:.6f}", file=sys.stderr)


if __name__ == "__main__":
    main()
