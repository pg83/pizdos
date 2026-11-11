"""The stand's suite of CPython's interpreter: pure Python, no module of C
doing the work, so it measures the eval loop, calls, attributes, objects,
generators and exceptions.

    python3 -S pybench.py NAME REPEAT
    python3 -S pybench.py --list

It runs benchmark NAME REPEAT times, prints the sha256 of its results, which
native and vm must agree on, and on standard error `time SECONDS` of the
work alone. Floats see no libm but sqrt, so results are the same bits
everywhere; PYTHONHASHSEED=0 fixes the order of sets.
"""
import hashlib
import sys
import time


# richards: Martin Richards' simulation of an operating system's scheduler,
# tasks and packets as objects.

IDLE, WORKER, HANDLER_A, HANDLER_B, DEVICE_A, DEVICE_B = range(6)
K_DEV, K_WORK = 1000, 1001
BUFSIZE = 4


class Packet:
    def __init__(self, link, ident, kind):
        self.link = link
        self.ident = ident
        self.kind = kind
        self.datum = 0
        self.data = [0] * BUFSIZE

    def append_to(self, lst):
        self.link = None

        if lst is None:
            return self

        p = lst
        nxt = p.link

        while nxt is not None:
            p = nxt
            nxt = p.link

        p.link = self

        return lst


class TaskRec:
    pass


class DeviceTaskRec(TaskRec):
    def __init__(self):
        self.pending = None


class IdleTaskRec(TaskRec):
    def __init__(self):
        self.control = 1
        self.count = 10000


class HandlerTaskRec(TaskRec):
    def __init__(self):
        self.work_in = None
        self.device_in = None

    def work_in_add(self, p):
        self.work_in = p.append_to(self.work_in)

        return self.work_in

    def device_in_add(self, p):
        self.device_in = p.append_to(self.device_in)

        return self.device_in


class WorkerTaskRec(TaskRec):
    def __init__(self):
        self.destination = HANDLER_A
        self.count = 0


class TaskState:
    def __init__(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False

    def packet_pending_state(self):
        self.packet_pending = True
        self.task_waiting = False
        self.task_holding = False

        return self

    def waiting(self):
        self.packet_pending = False
        self.task_waiting = True
        self.task_holding = False

        return self

    def running(self):
        self.packet_pending = False
        self.task_waiting = False
        self.task_holding = False

        return self

    def waiting_with_packet(self):
        self.packet_pending = True
        self.task_waiting = True
        self.task_holding = False

        return self

    def is_task_holding_or_waiting(self):
        return self.task_holding or (not self.packet_pending and self.task_waiting)

    def is_waiting_with_packet(self):
        return self.packet_pending and self.task_waiting and not self.task_holding


class Richards:
    def __init__(self):
        self.task_tab = [None] * 10
        self.task_list = None
        self.hold_count = 0
        self.qpkt_count = 0
        self.current_id = 0


class Task(TaskState):
    def __init__(self, sim, ident, priority, inp, initial, handle):
        TaskState.__init__(self)
        self.sim = sim
        self.link = sim.task_list
        self.ident = ident
        self.priority = priority
        self.input = inp
        self.packet_pending = initial.packet_pending
        self.task_waiting = initial.task_waiting
        self.task_holding = initial.task_holding
        self.handle = handle
        sim.task_list = self
        sim.task_tab[ident] = self

    def add_packet(self, p, old):
        if self.input is None:
            self.input = p
            self.packet_pending = True

            if self.priority > old.priority:
                return self
        else:
            p.append_to(self.input)

        return old

    def run_task(self):
        if self.is_waiting_with_packet():
            msg = self.input
            self.input = msg.link

            if self.input is None:
                self.running()
            else:
                self.packet_pending_state()
        else:
            msg = None

        return self.fn(msg, self.handle)

    def wait_task(self):
        self.task_waiting = True

        return self

    def hold(self):
        self.sim.hold_count += 1
        self.task_holding = True

        return self.link

    def release(self, i):
        t = self.find(i)
        t.task_holding = False

        if t.priority > self.priority:
            return t

        return self

    def qpkt(self, pkt):
        t = self.find(pkt.ident)
        self.sim.qpkt_count += 1
        pkt.link = None
        pkt.ident = self.sim.current_id

        return t.add_packet(pkt, self)

    def find(self, i):
        t = self.sim.task_tab[i]

        if t is None:
            raise ValueError(f"bad task id {i}")

        return t


class DeviceTask(Task):
    def fn(self, pkt, r):
        if pkt is None:
            pkt = r.pending

            if pkt is None:
                return self.wait_task()

            r.pending = None

            return self.qpkt(pkt)

        r.pending = pkt

        return self.hold()


class HandlerTask(Task):
    def fn(self, pkt, r):
        if pkt is not None:
            if pkt.kind == K_WORK:
                r.work_in_add(pkt)
            else:
                r.device_in_add(pkt)

        work = r.work_in

        if work is None:
            return self.wait_task()

        count = work.datum

        if count >= BUFSIZE:
            r.work_in = work.link

            return self.qpkt(work)

        dev = r.device_in

        if dev is None:
            return self.wait_task()

        r.device_in = dev.link
        dev.datum = work.data[count]
        work.datum = count + 1

        return self.qpkt(dev)


class IdleTask(Task):
    def fn(self, pkt, r):
        r.count -= 1

        if r.count == 0:
            return self.hold()

        if r.control & 1 == 0:
            r.control //= 2

            return self.release(DEVICE_A)

        r.control = r.control // 2 ^ 0xd008

        return self.release(DEVICE_B)


class WorkTask(Task):
    def fn(self, pkt, r):
        if pkt is None:
            return self.wait_task()

        dest = HANDLER_B if r.destination == HANDLER_A else HANDLER_A
        r.destination = dest
        pkt.ident = dest
        pkt.datum = 0

        for i in range(BUFSIZE):
            r.count += 1

            if r.count > 26:
                r.count = 1

            pkt.data[i] = ord("A") + r.count - 1

        return self.qpkt(pkt)


def richards(iterations):
    results = []

    for _ in range(iterations):
        sim = Richards()
        IdleTask(sim, IDLE, 1, None, TaskState().running(), IdleTaskRec())
        wkq = Packet(None, 0, K_WORK)
        wkq = Packet(wkq, 0, K_WORK)
        WorkTask(sim, WORKER, 1000, wkq, TaskState().waiting_with_packet(), WorkerTaskRec())
        wkq = Packet(None, DEVICE_A, K_DEV)
        wkq = Packet(wkq, DEVICE_A, K_DEV)
        wkq = Packet(wkq, DEVICE_A, K_DEV)
        HandlerTask(sim, HANDLER_A, 2000, wkq, TaskState().waiting_with_packet(), HandlerTaskRec())
        wkq = Packet(None, DEVICE_B, K_DEV)
        wkq = Packet(wkq, DEVICE_B, K_DEV)
        wkq = Packet(wkq, DEVICE_B, K_DEV)
        HandlerTask(sim, HANDLER_B, 3000, wkq, TaskState().waiting_with_packet(), HandlerTaskRec())
        DeviceTask(sim, DEVICE_A, 4000, None, TaskState().waiting(), DeviceTaskRec())
        DeviceTask(sim, DEVICE_B, 5000, None, TaskState().waiting(), DeviceTaskRec())
        t = sim.task_list

        while t is not None:
            if t.is_task_holding_or_waiting():
                t = t.link
            else:
                sim.current_id = t.ident
                t = t.run_task()

        results.append((sim.qpkt_count, sim.hold_count))

    return results


# deltablue: the incremental constraint solver of Maloney and Freeman-Benson,
# its chain and projection tests.

class Strength:
    def __init__(self, strength, name):
        self.strength = strength
        self.name = name

    @classmethod
    def stronger(cls, s1, s2):
        return s1.strength < s2.strength

    @classmethod
    def weaker(cls, s1, s2):
        return s1.strength > s2.strength

    @classmethod
    def weakest_of(cls, s1, s2):
        return s1 if cls.weaker(s1, s2) else s2

    def next_weaker(self):
        return STRENGTHS[self.strength + 1]


REQUIRED = Strength(0, "required")
STRONG_PREFERRED = Strength(1, "strongPreferred")
PREFERRED = Strength(2, "preferred")
STRONG_DEFAULT = Strength(3, "strongDefault")
NORMAL = Strength(4, "normal")
WEAK_DEFAULT = Strength(5, "weakDefault")
WEAKEST = Strength(6, "weakest")
STRENGTHS = [REQUIRED, STRONG_PREFERRED, PREFERRED, STRONG_DEFAULT, NORMAL, WEAK_DEFAULT, WEAKEST]


class Constraint:
    def __init__(self, strength, planner):
        self.strength = strength
        self.planner = planner

    def add_constraint(self):
        self.add_to_graph()
        self.planner.incremental_add(self)

    def satisfy(self, mark):
        self.choose_method(mark)

        if not self.is_satisfied():
            if self.strength == REQUIRED:
                raise ValueError("could not satisfy a required constraint")

            return None

        self.mark_inputs(mark)
        out = self.output()
        overridden = out.determined_by

        if overridden is not None:
            overridden.mark_unsatisfied()

        out.determined_by = self

        if not self.planner.add_propagate(self, mark):
            raise ValueError("cycle encountered")

        out.mark = mark

        return overridden

    def destroy_constraint(self):
        if self.is_satisfied():
            self.planner.incremental_remove(self)
        else:
            self.remove_from_graph()

    def is_input(self):
        return False


class UnaryConstraint(Constraint):
    def __init__(self, v, strength, planner):
        Constraint.__init__(self, strength, planner)
        self.my_output = v
        self.satisfied = False
        self.add_constraint()

    def add_to_graph(self):
        self.my_output.add_constraint(self)
        self.satisfied = False

    def choose_method(self, mark):
        self.satisfied = self.my_output.mark != mark and Strength.stronger(self.strength, self.my_output.walk_strength)

    def is_satisfied(self):
        return self.satisfied

    def mark_inputs(self, mark):
        pass

    def output(self):
        return self.my_output

    def recalculate(self):
        self.my_output.walk_strength = self.strength
        self.my_output.stay = not self.is_input()

        if self.my_output.stay:
            self.execute()

    def mark_unsatisfied(self):
        self.satisfied = False

    def inputs_known(self, mark):
        return True

    def remove_from_graph(self):
        if self.my_output is not None:
            self.my_output.remove_constraint(self)
            self.satisfied = False


class StayConstraint(UnaryConstraint):
    def execute(self):
        pass


class EditConstraint(UnaryConstraint):
    def is_input(self):
        return True

    def execute(self):
        pass


NONE, FORWARD, BACKWARD = 0, 1, 2


class BinaryConstraint(Constraint):
    def __init__(self, v1, v2, strength, planner):
        Constraint.__init__(self, strength, planner)
        self.v1 = v1
        self.v2 = v2
        self.direction = NONE
        self.add_constraint()

    def choose_method(self, mark):
        if self.v1.mark == mark:
            if self.v2.mark != mark and Strength.stronger(self.strength, self.v2.walk_strength):
                self.direction = FORWARD
            else:
                self.direction = NONE
        elif self.v2.mark == mark:
            if self.v1.mark != mark and Strength.stronger(self.strength, self.v1.walk_strength):
                self.direction = BACKWARD
            else:
                self.direction = NONE
        elif Strength.weaker(self.v1.walk_strength, self.v2.walk_strength):
            self.direction = BACKWARD if Strength.stronger(self.strength, self.v1.walk_strength) else NONE
        else:
            self.direction = FORWARD if Strength.stronger(self.strength, self.v2.walk_strength) else BACKWARD

    def add_to_graph(self):
        self.v1.add_constraint(self)
        self.v2.add_constraint(self)
        self.direction = NONE

    def is_satisfied(self):
        return self.direction != NONE

    def mark_inputs(self, mark):
        self.input().mark = mark

    def input(self):
        return self.v1 if self.direction == FORWARD else self.v2

    def output(self):
        return self.v2 if self.direction == FORWARD else self.v1

    def recalculate(self):
        ihn = self.input()
        out = self.output()
        out.walk_strength = Strength.weakest_of(self.strength, ihn.walk_strength)
        out.stay = ihn.stay

        if out.stay:
            self.execute()

    def mark_unsatisfied(self):
        self.direction = NONE

    def inputs_known(self, mark):
        i = self.input()

        return i.mark == mark or i.stay or i.determined_by is None

    def remove_from_graph(self):
        if self.v1 is not None:
            self.v1.remove_constraint(self)

        if self.v2 is not None:
            self.v2.remove_constraint(self)

        self.direction = NONE


class ScaleConstraint(BinaryConstraint):
    def __init__(self, src, scale, offset, dest, strength, planner):
        self.direction = NONE
        self.scale = scale
        self.offset = offset
        BinaryConstraint.__init__(self, src, dest, strength, planner)

    def add_to_graph(self):
        BinaryConstraint.add_to_graph(self)
        self.scale.add_constraint(self)
        self.offset.add_constraint(self)

    def remove_from_graph(self):
        BinaryConstraint.remove_from_graph(self)

        if self.scale is not None:
            self.scale.remove_constraint(self)

        if self.offset is not None:
            self.offset.remove_constraint(self)

    def mark_inputs(self, mark):
        BinaryConstraint.mark_inputs(self, mark)
        self.scale.mark = mark
        self.offset.mark = mark

    def execute(self):
        if self.direction == FORWARD:
            self.v2.value = self.v1.value * self.scale.value + self.offset.value
        else:
            self.v1.value = (self.v2.value - self.offset.value) // self.scale.value

    def recalculate(self):
        ihn = self.input()
        out = self.output()
        out.walk_strength = Strength.weakest_of(self.strength, ihn.walk_strength)
        out.stay = ihn.stay and self.scale.stay and self.offset.stay

        if out.stay:
            self.execute()


class EqualityConstraint(BinaryConstraint):
    def execute(self):
        self.output().value = self.input().value


class Variable:
    def __init__(self, name, value=0):
        self.name = name
        self.value = value
        self.constraints = []
        self.determined_by = None
        self.mark = 0
        self.walk_strength = WEAKEST
        self.stay = True

    def add_constraint(self, constraint):
        self.constraints.append(constraint)

    def remove_constraint(self, constraint):
        self.constraints.remove(constraint)

        if self.determined_by == constraint:
            self.determined_by = None


class Planner:
    def __init__(self):
        self.current_mark = 0

    def incremental_add(self, constraint):
        mark = self.new_mark()
        overridden = constraint.satisfy(mark)

        while overridden is not None:
            overridden = overridden.satisfy(mark)

    def incremental_remove(self, constraint):
        out = constraint.output()
        constraint.mark_unsatisfied()
        constraint.remove_from_graph()
        unsatisfied = self.remove_propagate_from(out)
        strength = REQUIRED

        while True:
            for u in unsatisfied:
                if u.strength == strength:
                    self.incremental_add(u)

            strength = strength.next_weaker() if strength != WEAKEST else None

            if strength is None:
                break

    def new_mark(self):
        self.current_mark += 1

        return self.current_mark

    def make_plan(self, sources):
        mark = self.new_mark()
        plan = []
        todo = sources

        while todo:
            c = todo.pop(0)

            if c.output().mark != mark and c.inputs_known(mark):
                plan.append(c)
                c.output().mark = mark
                self.add_constraints_consuming_to(c.output(), todo)

        return plan

    def extract_plan_from_constraints(self, constraints):
        return self.make_plan([c for c in constraints if c.is_input() and c.is_satisfied()])

    def add_propagate(self, c, mark):
        todo = [c]

        while todo:
            d = todo.pop(0)

            if d.output().mark == mark:
                self.incremental_remove(c)

                return False

            d.recalculate()
            self.add_constraints_consuming_to(d.output(), todo)

        return True

    def remove_propagate_from(self, out):
        out.determined_by = None
        out.walk_strength = WEAKEST
        out.stay = True
        unsatisfied = []
        todo = [out]

        while todo:
            v = todo.pop(0)

            for c in v.constraints:
                if not c.is_satisfied():
                    unsatisfied.append(c)

            determining = v.determined_by

            for next_c in v.constraints:
                if next_c != determining and next_c.is_satisfied():
                    next_c.recalculate()
                    todo.append(next_c.output())

        return unsatisfied

    def add_constraints_consuming_to(self, v, coll):
        determining = v.determined_by

        for c in v.constraints:
            if c != determining and c.is_satisfied():
                coll.append(c)


def execute_plan(plan):
    for c in plan:
        c.execute()


def chain_test(n):
    planner = Planner()
    prev = first = last = None

    for i in range(n + 1):
        v = Variable(f"v{i}")

        if prev is not None:
            EqualityConstraint(prev, v, REQUIRED, planner)

        if i == 0:
            first = v

        if i == n:
            last = v

        prev = v

    StayConstraint(last, STRONG_DEFAULT, planner)
    edit = EditConstraint(first, PREFERRED, planner)
    plan = planner.extract_plan_from_constraints([edit])
    values = []

    for i in range(100):
        first.value = i
        execute_plan(plan)
        values.append(last.value)

    return sum(values)


def projection_test(n):
    planner = Planner()
    scale = Variable("scale", 10)
    offset = Variable("offset", 1000)
    src = dst = None
    dests = []

    for i in range(n):
        src = Variable(f"src{i}", i)
        dst = Variable(f"dst{i}", i)
        dests.append(dst)
        StayConstraint(src, NORMAL, planner)
        ScaleConstraint(src, scale, offset, dst, REQUIRED, planner)

    out = []

    for var, value in ((src, 17), (dst, 1050), (scale, 5), (offset, 2000)):
        edit = EditConstraint(var, PREFERRED, planner)
        plan = planner.extract_plan_from_constraints([edit])

        for _ in range(10):
            var.value = value
            execute_plan(plan)

        edit.destroy_constraint()
        out.append(dst.value)

    out.append(sum(d.value for d in dests))

    return out


def deltablue(n):
    return chain_test(n), projection_test(n)


# Numeric loops.

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


def spectral_norm(n):
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


def fannkuch(n):
    """The pancake flips of every permutation of n: checksum and most flips."""
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


def permutations(iterable):
    """itertools.permutations in Python, as its documentation writes it."""
    pool = tuple(iterable)
    n = len(pool)
    indices = list(range(n))
    cycles = list(range(n, 0, -1))
    yield tuple(pool[i] for i in indices)

    while n:
        for i in reversed(range(n)):
            cycles[i] -= 1

            if cycles[i] == 0:
                indices[i:] = indices[i + 1:] + indices[i:i + 1]
                cycles[i] = n - i
            else:
                j = cycles[i]
                indices[i], indices[-j] = indices[-j], indices[i]
                yield tuple(pool[i] for i in indices)
                break
        else:
            return


def nqueens(n):
    """The placements of n queens, among the permutations of the columns."""
    cols = range(n)
    found = 0

    for vec in permutations(cols):
        if n == len({vec[i] + i for i in cols}) == len({vec[i] - i for i in cols}):
            found += 1

    return found


# Objects of floats.

class Vec:
    __slots__ = ("x", "y", "z")

    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z

    def __add__(self, o):
        return Vec(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o):
        return Vec(self.x - o.x, self.y - o.y, self.z - o.z)

    def scale(self, k):
        return Vec(self.x * k, self.y * k, self.z * k)

    def dot(self, o):
        return self.x * o.x + self.y * o.y + self.z * o.z

    def norm(self):
        return self.scale(1.0 / self.dot(self) ** 0.5)


class Sphere:
    def __init__(self, center, radius, color, reflect):
        self.center, self.radius, self.color, self.reflect = center, radius, color, reflect

    def hit(self, origin, direction):
        oc = origin - self.center
        b = oc.dot(direction)
        c = oc.dot(oc) - self.radius * self.radius
        disc = b * b - c

        if disc < 0:
            return None

        t = -b - disc ** 0.5

        return t if t > 1e-6 else None

    def normal(self, p):
        return (p - self.center).norm()


class Plane:
    def __init__(self, point, normal, color, reflect):
        self.point, self.n, self.color, self.reflect = point, normal, color, reflect

    def hit(self, origin, direction):
        denom = self.n.dot(direction)

        if abs(denom) < 1e-9:
            return None

        t = (self.point - origin).dot(self.n) / denom

        return t if t > 1e-6 else None

    def normal(self, p):
        return self.n


def trace(scene, light, origin, direction, depth):
    best, shape = None, None

    for s in scene:
        t = s.hit(origin, direction)

        if t is not None and (best is None or t < best):
            best, shape = t, s

    if shape is None:
        return Vec(0.1, 0.1, 0.2)

    p = origin + direction.scale(best)
    n = shape.normal(p)
    to_light = (light - p).norm()
    lit = max(n.dot(to_light), 0.0)

    for s in scene:
        if s is not shape and s.hit(p, to_light) is not None:
            lit *= 0.3

            break

    color = shape.color.scale(0.2 + 0.8 * lit)

    if depth < 3 and shape.reflect > 0:
        r = direction - n.scale(2 * direction.dot(n))
        color = color.scale(1 - shape.reflect) + trace(scene, light, p, r, depth + 1).scale(shape.reflect)

    return color


def raytrace(size):
    """Spheres over a plane, a light, shadows and reflections."""
    scene = [
        Sphere(Vec(0, 1, 5), 1, Vec(0.9, 0.2, 0.2), 0.3),
        Sphere(Vec(-2, 1, 6), 1, Vec(0.2, 0.9, 0.2), 0.5),
        Sphere(Vec(2, 0.7, 4), 0.7, Vec(0.2, 0.2, 0.9), 0.1),
        Plane(Vec(0, 0, 0), Vec(0, 1, 0), Vec(0.8, 0.8, 0.8), 0.2),
    ]
    light = Vec(-5, 8, -3)
    eye = Vec(0, 1.5, -2)
    total = 0.0

    for y in range(size):
        for x in range(size):
            d = Vec((x - size / 2) / size, (size / 2 - y) / size, 1).norm()
            c = trace(scene, light, eye, d, 0)
            total += c.x + c.y + c.z

    return total


class Point:
    __slots__ = ("x", "y", "z")

    def __init__(self, i):
        self.x = x = i * 0.5 + 1.0
        self.y = x * x - 3.0
        self.z = x * 0.75 + self.y / (x + 1.0)

    def normalize(self):
        norm = (self.x * self.x + self.y * self.y + self.z * self.z) ** 0.5
        self.x /= norm
        self.y /= norm
        self.z /= norm

    def maximize(self, other):
        self.x = self.x if self.x > other.x else other.x
        self.y = self.y if self.y > other.y else other.y
        self.z = self.z if self.z > other.z else other.z

        return self


def float_points(count):
    """Points made, normalized and folded by maximum."""
    points = [Point(i) for i in range(count)]

    for p in points:
        p.normalize()

    best = points[0]

    for p in points[1:]:
        best = best.maximize(p)

    return best.x, best.y, best.z


# The eval loop's own work.

def unpack_sequence(loops):
    t = (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    total = 0

    for _ in range(loops):
        a, b, c, d, e, f, g, h, i, j = t
        a, b, c, d, e, f, g, h, i, j = t
        a, b, c, d, e, f, g, h, i, j = t
        a, b, c, d, e, f, g, h, i, j = t
        a, b, c, d, e, f, g, h, i, j = t
        total += a + j

    return total


class Tree:
    def __init__(self, left, value, right):
        self.left, self.value, self.right = left, value, right

    def __iter__(self):
        if self.left:
            yield from self.left

        yield self.value

        if self.right:
            yield from self.right


def tree(lo, hi):
    if lo >= hi:
        return None

    mid = (lo + hi) // 2

    return Tree(tree(lo, mid), mid, tree(mid + 1, hi))


def generators(depth):
    """A balanced tree walked by recursive generators, and pipelines."""
    t = tree(0, 2 ** depth)
    total = sum(t)
    evens = sum(x for x in t if x % 2 == 0)
    squares = sum(x * x for x in (y + 1 for y in t))

    return total, evens, squares


async def coroutine_fib(n):
    if n <= 1:
        return n

    return await coroutine_fib(n - 1) + await coroutine_fib(n - 2)


def coroutines(n):
    """Coroutines awaiting coroutines, run without an event loop."""
    coro = coroutine_fib(n)

    try:
        coro.send(None)
    except StopIteration as stop:
        return stop.value


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def calls(n):
    """Recursion through a plain function."""
    return fib(n)


def dicts(n):
    """Dictionaries and sets of strings and tuples: counting, grouping,
    membership."""
    words = [f"w{(i * 7919) % 1009}" for i in range(n)]
    counts = {}

    for w in words:
        counts[w] = counts.get(w, 0) + 1

    groups = {}

    for w, c in counts.items():
        groups.setdefault(c, []).append(w)

    pairs = {(w, i % 13) for i, w in enumerate(words)}
    seen = sum(1 for w in words if (w, 3) in pairs)

    return len(counts), sorted((k, len(v)) for k, v in groups.items()), len(pairs), seen


class Missing(Exception):
    pass


def lookup(table, key, depth):
    if depth:
        return lookup(table, key, depth - 1)

    if key not in table:
        raise Missing(key)

    return table[key]


def exceptions(n):
    """Exceptions raised through frames and caught."""
    table = {i: i * i for i in range(0, n, 3)}
    total = missing = 0

    for i in range(n):
        try:
            total += lookup(table, i, 3)
        except Missing:
            missing += 1
        finally:
            total += 1

    return total, missing


BENCHMARKS = {
    "richards": (richards, 2),
    "deltablue": (deltablue, 1500),
    "nbody": (nbody, 20000),
    "spectral_norm": (spectral_norm, 100),
    "fannkuch": (fannkuch, 8),
    "nqueens": (nqueens, 7),
    "raytrace": (raytrace, 50),
    "float": (float_points, 60000),
    "unpack_sequence": (unpack_sequence, 100000),
    "generators": (generators, 15),
    "coroutines": (coroutines, 22),
    "calls": (calls, 25),
    "dicts": (dicts, 100000),
    "exceptions": (exceptions, 60000),
}


def main():
    if sys.argv[1:] == ["--list"]:
        print("\n".join(BENCHMARKS))

        return

    name, repeat = sys.argv[1], int(sys.argv[2])
    work, arg = BENCHMARKS[name]
    digest = hashlib.sha256()
    start = time.perf_counter()

    for _ in range(repeat):
        digest.update(repr(work(arg)).encode())

    elapsed = time.perf_counter() - start
    print(digest.hexdigest())
    print(f"time {elapsed:.6f}", file=sys.stderr)


if __name__ == "__main__":
    main()
