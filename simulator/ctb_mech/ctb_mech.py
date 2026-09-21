"""CTB mechanistic model — producer / progressive materialization / drain.

Standalone realization sibling to the outer C++ simulator's
`ComputeArray`. Does NOT replace it; the outer simulator's per-pass
average cycle model remains valid for its milestone-A/B/C validation
regime. This model exposes the sub-structure that the outer model
lumps into `compute_per_pass_cycles`.

State classes (see DESIGN_NOTE.md):
- Producer FSM (S_EXECUTE / S_WAIT / S_NEXT).
- Outstanding-work credit r_pending (independent of resident storage).
- Per-row output FIFOs Q_r(t) (resident storage; can overflow).
- Drain FSM (D_IDLE / D_ACTIVE with col_pop_saturated latch and q0/q1).
- Progressive row materialization schedule.

Do not fit constants. Do not encode "D=3 optimal" or "K=576 safe".
Outputs emerge from the interacting state.

Usage:
    from ctb_mech import CTBMechanistic, CTBParams
    sim = CTBMechanistic(CTBParams(K=1024, D=3))
    result = sim.run()
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class CTBParams:
    """All parameters. RTL-derived defaults from DESIGN_NOTE.md."""

    # Workload
    K: int = 1024
    K_TILE: int = 16

    # Array geometry
    ARRAY_M: int = 16
    ARRAY_N: int = 16

    # Producer FSM cycle counts (per pass, unstalled)
    T_execute: int = 21
    T_wait: int = 12          # bounded by SWAIT_EARLY_THRESHOLD=11 + 1 cyc entry
    T_next_unblocked: int = 1

    # Credit
    D: int = 3                # DECOUPLE_MAX

    # Drain
    drain_ii: int = 2         # Y2: II=2 (cyc/fire)
    drain_tail: int = 4       # Cyc from last fire to drain_end pulse (fire → capture II lat → accum → pulse)
    drain_autorestart_gap: int = 1  # Cyc between drain_end and next fire on auto-restart
    drain_start_delay: int = 2  # Cyc from out_ready-high to first fire (D_IDLE → D_ACTIVE + NBA)

    # FIFO
    CAP: int = 32

    # Progressive row materialization
    #   Row 15 (last) first push at t_pass_end + pipe_row15
    #   Row r first push at t_pass_end + pipe_row15 - (ARRAY_M-1 - r)
    #   (i.e., row 0 arrives ARRAY_M-1 cycles earlier than row 15).
    pipe_row15: int = 37
    row_skew: int = 1

    # Safety limit for the simulator loop
    max_cycles: int = 500_000

    @property
    def num_passes(self) -> int:
        return max(1, self.K // self.K_TILE)

    @property
    def pipe_row0(self) -> int:
        return self.pipe_row15 - (self.ARRAY_M - 1) * self.row_skew


# Producer states
S_IDLE = "IDLE"
S_EXECUTE = "EXECUTE"
S_WAIT = "WAIT"
S_NEXT = "NEXT"
S_DONE = "DONE"


@dataclass
class CTBResult:
    K: int
    D: int
    num_passes: int
    service_cycles: int
    exec_cycles: int
    wait_cycles: int
    next_cycles: int          # includes credit stall
    credit_stall_cyc: int     # subset of next_cycles: r_pending==D while non-final
    pass_end_n: int
    drain_end_n: int
    rp_peak: int
    rp_hist: List[int] = field(default_factory=list)
    fifo_peak_all: int = 0
    fifo_peak_per_row: List[int] = field(default_factory=list)
    overflow: bool = False
    overflow_first_cycle: int = -1
    overflow_row: int = -1
    L_first: int = -1         # first pass_end → first drain_end
    L_last: int = -1          # last pass_end → last drain_end
    regime: str = "unknown"
    pass_end_ts: List[int] = field(default_factory=list)
    drain_end_ts: List[int] = field(default_factory=list)


class CTBMechanistic:
    def __init__(self, p: CTBParams):
        self.p = p

        # --- Producer state ---
        self.prod_state = S_IDLE
        self.prod_state_cyc = 0
        self.pass_idx = 0                       # next pass to emit
        self.passes_started = 0                 # count of S_EXECUTE entries
        self.pass_end_ts: List[int] = []

        # --- Credit ---
        self.r_pending = 0
        self.rp_hist = [0] * 16
        self.rp_peak = 0

        # --- Row FIFOs: list of pending (t_push, pass_id) for each row ---
        # Occupancy is len(fifo[r]) at any moment.
        self.fifo_pushes_per_row: List[int] = [0] * p.ARRAY_M
        self.fifo_pops_per_row: List[int] = [0] * p.ARRAY_M
        self.fifo_peak_per_row: List[int] = [0] * p.ARRAY_M
        self.fifo_peak_all = 0
        self.overflow = False
        self.overflow_first_cycle = -1
        self.overflow_row = -1

        # --- Row-push schedule (list of (t_push, row, pass_id, col)) ---
        # We schedule pushes on pass_end.
        self.pending_pushes: List = []          # list of (t, row, pass_id, col)

        # --- Drain FSM ---
        self.drain_state = "D_IDLE"
        self.drain_col = 0                      # next col to fire
        self.col_pop_saturated = False
        self.inflight = 0                       # cyc since last fire (waits drain_ii)
        self.q0_v = False
        self.q1_v = False
        self.q0_col = 0
        self.q1_col = 0
        # drain_start pulse: latched high while producer is in S_WAIT and
        # `drain_end` hasn't yet occurred for the current pass. We treat this
        # as a per-pass one-shot.
        self.drain_start_armed = False
        self.drain_end_ts: List[int] = []
        self.drains_completed = 0               # count of drain_end pulses
        self.drain_first_fire_pending = False   # waiting to fire first read of this drain
        self.out_ready_first_cyc = -1           # cycle out_ready first went high this drain
        self.drain_last_fire_cyc = -1           # cycle of last fire (for tail timing)

        # --- FIFO-push capture queue (initialize eagerly; see student-repo cleanup note) ---
        self._captures: List = []
        self.drain_cold_start = True            # True on first (D_IDLE→D_ACTIVE) entry; False on auto-restart
        self.autorestart_fire_earliest = -1     # earliest allowed first-fire cycle on auto-restart

        # --- Counters ---
        self.cyc_execute = 0
        self.cyc_wait = 0
        self.cyc_next = 0
        self.cyc_credit_stall = 0
        self.service_cycles = 0                 # cyc where state != IDLE and != DONE

        # --- Cycle ---
        self.t = 0

    # ------------------------------------------------------------
    # Row push scheduling
    # ------------------------------------------------------------
    def _schedule_pushes_for_pass(self, pass_id: int, t_pass_end: int) -> None:
        """When a pass emits pass_end at t_pass_end, schedule 16 pushes
        per row across a 16-cycle window (col=0..15). Row r's first
        push arrives at t_pass_end + pipe_row0 + r * row_skew."""
        p = self.p
        for r in range(p.ARRAY_M):
            delta_r = p.pipe_row0 + r * p.row_skew
            for c in range(p.ARRAY_N):
                t_push = t_pass_end + delta_r + c
                self.pending_pushes.append((t_push, r, pass_id, c))

    # ------------------------------------------------------------
    # Per-cycle tick
    # ------------------------------------------------------------
    def step(self):
        t = self.t
        p = self.p

        # 1. Row push events at this cycle.
        remaining = []
        for (t_push, r, pass_id, col) in self.pending_pushes:
            if t_push == t:
                # Push into row r's FIFO.
                self.fifo_pushes_per_row[r] += 1
                occ = self.fifo_pushes_per_row[r] - self.fifo_pops_per_row[r]
                if occ > self.fifo_peak_per_row[r]:
                    self.fifo_peak_per_row[r] = occ
                if occ > self.fifo_peak_all:
                    self.fifo_peak_all = occ
                if occ > p.CAP:
                    self.overflow = True
                    if self.overflow_first_cycle == -1:
                        self.overflow_first_cycle = t
                        self.overflow_row = r
            elif t_push < t:
                # Late — should not happen; but drop to keep queue small.
                pass
            else:
                remaining.append((t_push, r, pass_id, col))
        self.pending_pushes = remaining

        # 2. Drain capture (q0/q1 fill).
        # If a fire happened `drain_ii` cyc ago, capture now.
        # We model capture as happening `drain_ii` cyc after a fire event.
        # Track fires by scheduling the capture events.
        # For simplicity, we do capture in same loop as fire below.

        # 3. Pulse tracking to update r_pending later.
        pass_end_this_cyc = False
        drain_end_this_cyc = False

        # 4. Producer FSM update.
        if self.prod_state == S_IDLE:
            if self.pass_idx == 0:
                # Kick off pass 0.
                self.prod_state = S_EXECUTE
                self.prod_state_cyc = 0
                self.passes_started = 1
            # else nothing (should not happen unless done)
        elif self.prod_state == S_EXECUTE:
            self.cyc_execute += 1
            self.prod_state_cyc += 1
            if self.prod_state_cyc == p.T_execute:
                # end of S_EXECUTE → pass_end pulse fires (last cycle of
                # S_EXECUTE), state transitions to S_WAIT next cycle
                pass_end_this_cyc = True
                self.pass_end_ts.append(t)
                # Schedule row pushes now (using this cycle as pass_end).
                self._schedule_pushes_for_pass(self.pass_idx, t)
                self.prod_state = S_WAIT
                self.prod_state_cyc = 0
                self.drain_start_armed = True   # producer arms drain
        elif self.prod_state == S_WAIT:
            self.cyc_wait += 1
            self.prod_state_cyc += 1
            if self.prod_state_cyc == p.T_wait:
                self.prod_state = S_NEXT
                self.prod_state_cyc = 0
        elif self.prod_state == S_NEXT:
            self.cyc_next += 1
            self.prod_state_cyc += 1
            # Check advance condition.
            is_final = self.pass_idx == p.num_passes - 1
            if is_final:
                # Final pass waits for drain of THIS pass to complete
                # (drain_end fired and r_pending is 0 or 1 depending on
                # ordering). RTL: `if (!drain_busy)` = drain FSM back in
                # D_IDLE. We approximate as: r_pending == 0.
                if self.r_pending == 0:
                    self.prod_state = S_DONE
                    # producer done; drain (if any) still runs to
                    # completion.
            else:
                if self.r_pending < p.D:
                    # Advance to next pass.
                    self.pass_idx += 1
                    self.prod_state = S_EXECUTE
                    self.prod_state_cyc = 0
                    self.passes_started += 1
                else:
                    # Credit stall.
                    self.cyc_credit_stall += 1
        elif self.prod_state == S_DONE:
            pass

        # 5. Drain FSM update.
        if self.drain_state == "D_IDLE":
            # Enter D_ACTIVE if drain_start_armed OR r_pending > 0.
            if self.drain_start_armed or self.r_pending > 0:
                self.drain_state = "D_ACTIVE"
                self.drain_col = 0
                self.col_pop_saturated = False
                self.inflight = 0
                self.q0_v = False
                self.q1_v = False
                self.out_ready_first_cyc = -1
                self.drain_first_fire_pending = True
                self.drain_cold_start = True
        elif self.drain_state == "D_ACTIVE":
            # Track when out_ready first went high in this drain (only
            # meaningful on cold start).
            if self.drain_cold_start and self.out_ready_first_cyc == -1 \
                    and self._out_ready():
                self.out_ready_first_cyc = t

            # First-fire: cold start needs out_ready + drain_start_delay;
            # auto-restart needs autorestart_fire_earliest + out_ready.
            if self.drain_cold_start:
                first_fire_ok = (
                    self.drain_first_fire_pending
                    and not self.col_pop_saturated
                    and self.out_ready_first_cyc != -1
                    and t >= self.out_ready_first_cyc + p.drain_start_delay
                    and self._out_ready()
                    and not (self.q0_v and self.q1_v)
                )
            else:
                first_fire_ok = (
                    self.drain_first_fire_pending
                    and not self.col_pop_saturated
                    and self._out_ready()
                    and t >= self.autorestart_fire_earliest
                    and not (self.q0_v and self.q1_v)
                )
            # Subsequent fires: gated by II timing.
            can_fire = (
                not self.drain_first_fire_pending
                and not self.col_pop_saturated
                and self.inflight >= p.drain_ii
                and not (self.q0_v and self.q1_v)
                and self._out_ready()
            )

            if can_fire or first_fire_ok:
                self._fire_drain(t)
                self.drain_last_fire_cyc = t
                self.inflight = 0
                self.drain_first_fire_pending = False
                if self.drain_col == p.ARRAY_N - 1:
                    self.col_pop_saturated = True
                    self.drain_col = p.ARRAY_N
                else:
                    self.drain_col += 1

            # Track in-flight cycles between fires.
            self.inflight = min(self.inflight + 1, p.drain_ii + 1)

            self._process_captures(t)

            # drain_end pulse: col_pop_saturated + drain_tail cyc after last fire
            # + no q pending + no captures pending.
            tail_ready = (
                self.drain_last_fire_cyc != -1
                and t >= self.drain_last_fire_cyc + p.drain_tail
            )
            if (self.col_pop_saturated and tail_ready
                    and not self.q0_v and not self.q1_v
                    and not self._captures_pending()):
                drain_end_this_cyc = True
                self.drain_end_ts.append(t)
                self.drains_completed += 1
                self.drain_start_armed = False
                self.drain_last_fire_cyc = -1
                self.out_ready_first_cyc = -1

                r_pending_post_pulses = self.r_pending
                if pass_end_this_cyc:
                    r_pending_post_pulses += 1

                if r_pending_post_pulses > 1:
                    # Auto-restart. Preserve D_ACTIVE state; small gap.
                    self.drain_col = 0
                    self.col_pop_saturated = False
                    self.inflight = 0
                    self.drain_first_fire_pending = True
                    self.drain_cold_start = False
                    self.autorestart_fire_earliest = t + p.drain_autorestart_gap
                else:
                    self.drain_state = "D_IDLE"

        # 6. r_pending update (net zero if both pulses).
        prev_rp = self.r_pending
        if pass_end_this_cyc and drain_end_this_cyc:
            pass  # net zero
        elif pass_end_this_cyc:
            self.r_pending = min(self.r_pending + 1, 15)
        elif drain_end_this_cyc:
            self.r_pending = max(self.r_pending - 1, 0)

        # 7. Bookkeeping.
        if self.r_pending > self.rp_peak:
            self.rp_peak = self.r_pending
        self.rp_hist[min(self.r_pending, 15)] += 1

        # Service = cyc where producer or drain is doing something.
        if self.prod_state not in (S_IDLE, S_DONE) or self.drain_state != "D_IDLE":
            self.service_cycles += 1

        self.t += 1

    def _out_ready(self) -> bool:
        """Combinational out_ready = all row FIFOs have ≥1 entry
        (available to pop)."""
        for r in range(self.p.ARRAY_M):
            occ = self.fifo_pushes_per_row[r] - self.fifo_pops_per_row[r]
            if occ == 0:
                return False
        return True

    # --- Capture queue: each fire schedules a capture drain_ii cyc later ---
    # We model this with a simple list of (capture_t, col) entries.
    def _fire_drain(self, t: int):
        # Schedule capture at t + drain_ii (Y2 semantics).
        cap_t = t + self.p.drain_ii
        cap_col = self.drain_col
        self._captures.append((cap_t, cap_col))
        # Pop from every row FIFO at fire time (matches
        # out_rd_en = {ARRAY_M{1'b1}} in RTL).
        for r in range(self.p.ARRAY_M):
            occ = self.fifo_pushes_per_row[r] - self.fifo_pops_per_row[r]
            if occ > 0:
                self.fifo_pops_per_row[r] += 1

    def _process_captures(self, t: int):
        remaining = []
        for (cap_t, cap_col) in self._captures:
            if cap_t == t:
                # This capture arrives → fill q0 or q1.
                if not self.q0_v:
                    self.q0_v = True
                    self.q0_col = cap_col
                elif not self.q1_v:
                    self.q1_v = True
                    self.q1_col = cap_col
                # else: buf_full — model treats this as a stall (we do
                # not fire when buf_full, so this branch should be rare).
            else:
                remaining.append((cap_t, cap_col))
        self._captures = remaining

        # Accumulate: if q0 valid and not a drain-clear cycle, drain q0
        # into out_mem this cycle, then shift q1 → q0.
        if self.q0_v:
            # (accumulate to out_mem — semantic no-op in this model)
            if self.q1_v:
                self.q0_col = self.q1_col
                self.q1_v = False
                self.q0_v = True
            else:
                self.q0_v = False

    def _captures_pending(self) -> bool:
        return len(self._captures) > 0

    # ------------------------------------------------------------
    def run(self) -> CTBResult:
        p = self.p
        while self.t < p.max_cycles:
            self.step()
            done = (self.prod_state == S_DONE
                    and self.drain_state == "D_IDLE"
                    and self.r_pending == 0
                    and not self.pending_pushes
                    and not self._captures_pending())
            if done:
                break

        # Regime classification.
        L_first = -1
        L_last = -1
        if self.pass_end_ts and self.drain_end_ts:
            L_first = self.drain_end_ts[0] - self.pass_end_ts[0]
            if len(self.pass_end_ts) == len(self.drain_end_ts):
                L_last = self.drain_end_ts[-1] - self.pass_end_ts[-1]

        # rp_peak clamps at D+1 (producer can push r_pending to D+1
        # briefly after pass_end).
        rp_peak = self.rp_peak

        # Regime classification. Three orthogonal bottleneck axes:
        # (a) producer own cycles T_p, (b) drain service T_d,
        # (c) credit depth D. The active constraint is whichever binds
        # at steady state.
        #
        # T_d_measured from drain_end spacing is a POST-CREDIT number:
        # when D is small enough to throttle the drain, the spacing
        # inflates to match the credit-serialized cadence. Using it as
        # the drain-only bound then double-counts the credit effect and
        # can mislabel "credit-limited" as "drain-limited".
        #
        # Instead compare the observed per-pass cadence to the intrinsic
        # no-credit bottleneck cadence
        #   T_bottleneck_no_credit = max(T_p_native, T_d_ideal_per_pass)
        # where T_d_ideal_per_pass is the drain's own steady-state per-
        # pass service time when unimpeded by credit — one drain fires
        # every `drain_ii` cyc across ARRAY_N columns of the pass.
        if self.overflow:
            regime = "storage-unsafe"
        else:
            svc = max(self.service_cycles, 1)
            stall_frac = self.cyc_credit_stall / svc
            per_pass = svc / max(p.num_passes, 1)
            T_p_native = p.T_execute + p.T_wait + p.T_next_unblocked
            T_d_ideal_per_pass = p.drain_ii * p.ARRAY_N
            T_bottleneck_no_credit = max(T_p_native, T_d_ideal_per_pass)
            # Credit headroom in cyc/pass: how much slower is the
            # observed cadence than the credit-free bottleneck.
            credit_headroom = per_pass - T_bottleneck_no_credit

            producer_bound = (per_pass <= T_p_native + 2
                              and stall_frac < 0.05
                              and rp_peak < p.D)

            if p.num_passes < 3:
                regime = "dormant"
            elif producer_bound:
                regime = "producer-limited"
            elif stall_frac > 0.05 and rp_peak == p.D:
                # Both credit-limited and drain-limited can drive
                # rp_peak to D and stall_frac above 5%. Distinguish
                # them by whether the observed per-pass cadence
                # actually exceeds what T_p and the drain-only rate
                # jointly bound. If yes, raising D would reclaim the
                # gap → credit-limited. If no, the observed cadence
                # already matches the intrinsic bottleneck → drain-
                # limited (credit is fully engaged but not the
                # binding constraint).
                if credit_headroom > 5:
                    regime = "credit-limited"
                else:
                    regime = "drain-limited"
            elif rp_peak <= p.D and stall_frac <= 0.05:
                regime = "drain-limited"
            else:
                regime = "unclear"

        return CTBResult(
            K=p.K,
            D=p.D,
            num_passes=p.num_passes,
            service_cycles=self.service_cycles,
            exec_cycles=self.cyc_execute,
            wait_cycles=self.cyc_wait,
            next_cycles=self.cyc_next,
            credit_stall_cyc=self.cyc_credit_stall,
            pass_end_n=len(self.pass_end_ts),
            drain_end_n=len(self.drain_end_ts),
            rp_peak=rp_peak,
            rp_hist=list(self.rp_hist),
            fifo_peak_all=self.fifo_peak_all,
            fifo_peak_per_row=list(self.fifo_peak_per_row),
            overflow=self.overflow,
            overflow_first_cycle=self.overflow_first_cycle,
            overflow_row=self.overflow_row,
            L_first=L_first,
            L_last=L_last,
            regime=regime,
            pass_end_ts=list(self.pass_end_ts),
            drain_end_ts=list(self.drain_end_ts),
        )
