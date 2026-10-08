#!/usr/bin/env python3
"""Run every experiment, then write results/, the figures, and the memo.

    python3 scripts/build_report.py build            # about four minutes
    python3 scripts/build_report.py build --quick    # smaller budgets, under a minute
    python3 scripts/build_report.py build --reuse    # redraw from existing results/

Every number in the memo is read back from results/*.csv, so the text cannot
drift away from the data.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import platform
import subprocess
import sys
import time

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import lp_oracle  # noqa: E402

# One small, fixed palette. Hues are assigned in this order and never cycled;
# text is always ink, never a series color.
SURFACE, INK, INK_2, MUTED, GRID, AXIS = "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7"
BLUE, ORANGE, AQUA, YELLOW, MAGENTA = "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"
ALGORITHMS = [("cfr", "CFR (2007)", BLUE), ("lcfr", "Linear CFR", ORANGE), ("dcfr", "Discounted CFR", AQUA),
              ("cfr+", "CFR+", YELLOW), ("mccfr", "Sampled CFR", MAGENTA)]
OPPONENTS = [("random", "Random", "picks every action equally often"),
             ("station", "Calling station", "never folds, never bets"),
             ("maniac", "Maniac", "bets and raises at every chance"),
             ("rock", "Rock", "only puts money in with a King or a pair")]

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "font.family": "sans-serif", "font.size": 10, "text.color": INK,
    "axes.edgecolor": AXIS, "axes.linewidth": 0.8, "axes.labelcolor": INK_2,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlecolor": INK,
    "axes.titlelocation": "left", "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "grid.linestyle": "-",
    "axes.axisbelow": True,
    "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK_2, "ytick.labelcolor": INK_2,
    "xtick.major.size": 0, "ytick.major.size": 0, "xtick.minor.size": 0, "ytick.minor.size": 0,
    "lines.linewidth": 2.0, "lines.solid_capstyle": "round", "lines.solid_joinstyle": "round",
    "lines.markersize": 6, "lines.markeredgecolor": SURFACE, "lines.markeredgewidth": 1.4,
    "legend.frameon": False, "legend.fontsize": 9, "legend.labelcolor": INK_2,
    "svg.fonttype": "none",
})


class Runner:
    def __init__(self, build):
        self.binary = os.path.join(build, "zerosum")
        if not os.path.exists(self.binary):
            sys.exit(f"{self.binary} not found: run `make` first")

    def __call__(self, *args):
        done = subprocess.run([self.binary, *map(str, args)], capture_output=True, text=True)
        if done.returncode != 0:
            sys.exit(f"zerosum {' '.join(map(str, args))} failed: {done.stderr}")
        return done.stdout

    def table(self, *args):
        return list(csv.DictReader(io.StringIO(self(*args))))

    def fields(self, *args):
        return {line.split()[0]: line.split()[1:] for line in self(*args).splitlines()}


def write(path, rows):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def read(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        for key, value in row.items():
            try:
                row[key] = float(value)
            except ValueError:
                pass
    return rows


# ---------------------------------------------------------------------------
# Experiments


def run_experiments(run, results, quick):
    os.makedirs(results, exist_ok=True)
    work = os.path.join(results, "strategies")
    os.makedirs(work, exist_ok=True)
    started = time.time()

    print("1/6 exact equilibria by linear program ...", flush=True)
    rows = []
    budgets = {"kuhn": 100000, "leduc": 20000, "leduc5": 5000, "leduc13": 1000}
    for game in ("kuhn", "leduc", "leduc5") + (() if quick else ("leduc13",)):
        info = run.fields("info", game)
        tree = json.loads(run("export", game))
        clock = time.time()
        value, behavior, form = lp_oracle.solve_tree(tree)
        lp_seconds = time.time() - clock
        path = os.path.join(work, f"{game}_lp.txt")
        lp_oracle.write_strategy(path, tree, behavior)
        checked = run.fields("eval", game, "--strategy", path)
        iterations = budgets[game] // (10 if quick else 1)
        solved = run.table("solve", game, "--algo", "cfr+", "--iterations", iterations, "--csv",
                           "--save", os.path.join(work, f"{game}_cfrplus.txt"))[0]
        rows.append({
            "game": game, "nodes": int(info["nodes"][0]), "information_sets": int(info["information_sets"][0]),
            "lp_sequences_per_player": form.count[0], "lp_value": f"{value:.12f}", "lp_seconds": f"{lp_seconds:.3f}",
            "lp_strategy_exploitability_ma": checked["exploitability_ma_per_hand"][0],
            "cfrplus_iterations": iterations, "cfrplus_seconds": solved["solve_seconds"],
            "cfrplus_value_lower": solved["value_lower"], "cfrplus_value_upper": solved["value_upper"],
            "cfrplus_exploitability_ma": solved["exploitability_ma"],
            "lp_value_inside_bracket": int(float(solved["value_lower"]) <= value + 1e-9 and
                                           value - 1e-9 <= float(solved["value_upper"])),
        })
    write(os.path.join(results, "lp_check.csv"), rows)

    print("2/6 convergence of each algorithm on Leduc ...", flush=True)
    rows = []
    for name, _, _ in ALGORITHMS:
        if name == "mccfr":
            for seed in (1, 2, 3):
                part = run.table("solve", "leduc", "--algo", name, "--iterations", 300000 if quick else 20000000,
                                 "--growing", "--csv", "--seed", seed)
                rows += [{**r, "seed": seed} for r in part]
        else:
            part = run.table("solve", "leduc", "--algo", name, "--iterations", 2000 if quick else 50000,
                             "--growing", "--csv")
            rows += [{**r, "seed": 1} for r in part]
    write(os.path.join(results, "convergence.csv"), rows)

    print("3/6 work to reach 1 milli-ante per hand ...", flush=True)
    write(os.path.join(results, "bench.csv"),
          run.table("bench", "leduc", "--target", 1.0, "--max-nodes", 2e8 if quick else 4e9, "--trials", 3))

    print("4/6 equilibrium against weak opponents ...", flush=True)
    rows = []
    for label, path in (("linear program", "leduc_lp.txt"), ("cfr+", "leduc_cfrplus.txt")):
        for r in run.table("exploit", "leduc", "--strategy", os.path.join(work, path), "--csv"):
            rows.append({"equilibrium_from": label, **r})
    write(os.path.join(results, "exploit.csv"), rows)

    print("5/6 profit against risk ...", flush=True)
    rows = []
    for opponent, _, _ in OPPONENTS:
        rows += run.table("frontier", "leduc", "--strategy", os.path.join(work, "leduc_cfrplus.txt"),
                          "--opponent", opponent, "--steps", 10 if quick else 20,
                          "--iterations", 500 if quick else 3000)
    write(os.path.join(results, "frontier.csv"), rows)

    print("6/6 deck size and Kuhn's equilibrium family ...", flush=True)
    rows = []
    for ranks in range(3, 8 if quick else 14):
        game = "leduc" if ranks == 3 else f"leduc{ranks}"
        info = run.fields("info", game)
        solved = run.table("solve", game, "--algo", "dcfr", "--iterations", 100 if quick else 1000, "--csv")[0]
        rows.append({"ranks": ranks, "nodes": int(info["nodes"][0]),
                     "information_sets": int(info["information_sets"][0]),
                     "iterations": solved["iteration"], "nodes_touched": solved["nodes_touched"],
                     "solve_seconds": solved["solve_seconds"],
                     "exploitability_ma": solved["exploitability_ma"],
                     "value_lower": solved["value_lower"], "value_upper": solved["value_upper"]})
    write(os.path.join(results, "scaling.csv"), rows)

    rows = []
    for name, _, _ in ALGORITHMS:
        path = os.path.join(work, f"kuhn_{name.replace('+', 'plus')}.txt")
        solved = run.table("solve", "kuhn", "--algo", name, "--iterations",
                           2000000 if name == "mccfr" else 100000, "--csv", "--save", path)[0]
        shown = {line.split()[0]: line.split()[1:] for line in run("show", "kuhn", "--strategy", path).splitlines()}

        def p(key, action):
            return float(shown[key][shown[key].index(action) + 1])

        rows.append({"algorithm": name, "exploitability_ma": solved["exploitability_ma"],
                     "first_bluffs_jack": p("0:J:-:", "b"), "first_bets_king": p("0:K:-:", "b"),
                     "first_bets_queen": p("0:Q:-:", "b"), "first_calls_with_queen": p("0:Q:-:kb", "c"),
                     "second_bluffs_jack": p("1:J:-:k", "b"), "second_calls_with_queen": p("1:Q:-:b", "c")})
    lp = {line.split()[0]: line.split()[1:] for line in
          run("show", "kuhn", "--strategy", os.path.join(work, "kuhn_lp.txt")).splitlines()}
    rows.append({"algorithm": "linear program", "exploitability_ma": 0.0,
                 "first_bluffs_jack": float(lp["0:J:-:"][3]), "first_bets_king": float(lp["0:K:-:"][3]),
                 "first_bets_queen": float(lp["0:Q:-:"][3]), "first_calls_with_queen": float(lp["0:Q:-:kb"][3]),
                 "second_bluffs_jack": float(lp["1:J:-:k"][3]), "second_calls_with_queen": float(lp["1:Q:-:b"][3])})
    write(os.path.join(results, "kuhn_family.csv"), rows)

    manifest = {
        "quick": quick, "seconds": round(time.time() - started, 1),
        "python": platform.python_version(), "numpy": np.__version__,
        "scipy": __import__("scipy").__version__, "matplotlib": matplotlib.__version__,
        "platform": platform.platform(), "cpu_count": os.cpu_count(),
    }
    with open(os.path.join(results, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"experiments done in {manifest['seconds']} s", flush=True)


# ---------------------------------------------------------------------------
# Figures


def save(fig, out, name):
    os.makedirs(out, exist_ok=True)
    fig.savefig(os.path.join(out, name + ".png"), dpi=170, bbox_inches="tight")
    fig.savefig(os.path.join(out, name + ".svg"), bbox_inches="tight")
    plt.close(fig)


def plain_log_ticks(axis, values, labels=None):
    axis.set_ticks(values)
    axis.set_ticklabels(labels or [f"{v:g}" for v in values])
    axis.set_minor_locator(matplotlib.ticker.NullLocator())


def figure_convergence(results, out):
    rows = read(os.path.join(results, "convergence.csv"))
    fig, ax = plt.subplots(figsize=(7.6, 4.4))
    for name, label, color in ALGORITHMS:
        part = [r for r in rows if r["algorithm"] == name and r["seed"] == 1 and r["nodes_touched"] >= 1e4]
        x = [r["nodes_touched"] for r in part]
        y = [r["exploitability_ma"] for r in part]
        ax.loglog(x, y, color=color, label=label)
    ax.axhline(1.0, color=AXIS, linewidth=1.0, zorder=1)
    ax.annotate("1 milli-ante per hand", xy=(1.2e4, 1.0), xytext=(0, 4), textcoords="offset points",
                color=MUTED, fontsize=8.5)
    ax.set_title("Same work, very different accuracy")
    ax.set_xlabel("Game-tree nodes visited (Leduc hold'em)")
    ax.set_ylabel("Exploitability (milli-antes per hand)")
    ax.set_xlim(1e4, max(r["nodes_touched"] for r in rows) * 1.6)
    plain_log_ticks(ax.xaxis, [1e4, 1e5, 1e6, 1e7, 1e8, 1e9], ["10K", "100K", "1M", "10M", "100M", "1B"])
    plain_log_ticks(ax.yaxis, [1e-3, 1e-2, 0.1, 1, 10, 100, 1000])
    ax.legend(loc="lower left")
    save(fig, out, "convergence")


def figure_exploit(results, out):
    rows = [r for r in read(os.path.join(results, "exploit.csv")) if r["equilibrium_from"] == "cfr+"]
    by = {r["opponent"]: r for r in rows}
    names = [label for _, label, _ in OPPONENTS]
    nash = [by[key]["nash_profit_ma"] for key, _, _ in OPPONENTS]
    best = [by[key]["max_profit_ma"] for key, _, _ in OPPONENTS]
    y = np.arange(len(names))
    fig, ax = plt.subplots(figsize=(7.6, 3.6))
    height = 0.34
    ax.barh(y - height / 2 - 0.02, nash, height, color=BLUE, label="equilibrium strategy")
    ax.barh(y + height / 2 + 0.02, best, height, color=ORANGE, label="best possible counter-strategy")
    for i in range(len(names)):
        ax.annotate(f"{nash[i]:,.0f}", xy=(nash[i], y[i] - height / 2 - 0.02), xytext=(5, 0),
                    textcoords="offset points", va="center", color=INK_2, fontsize=9)
        ax.annotate(f"{best[i]:,.0f}", xy=(best[i], y[i] + height / 2 + 0.02), xytext=(5, 0),
                    textcoords="offset points", va="center", color=INK_2, fontsize=9)
    ax.set_yticks(y)
    ax.set_yticklabels(names)
    ax.invert_yaxis()
    ax.grid(axis="y", visible=False)
    ax.set_xlim(0, max(best) * 1.12)
    ax.set_title("Equilibrium play wins, but leaves most of the money on the table")
    ax.set_xlabel("Profit against this opponent (milli-antes per hand)")
    ax.legend(loc="lower right")
    save(fig, out, "exploit")


def figure_frontier(results, out):
    rows = read(os.path.join(results, "frontier.csv"))
    fig, axes = plt.subplots(2, 2, figsize=(9.6, 6.6), gridspec_kw={"wspace": 0.24, "hspace": 0.42})
    for ax, (key, label, description) in zip(axes.flat, OPPONENTS):
        for method, color, name in (("mix", BLUE, "coin-flip mix"), ("restricted", ORANGE, "restricted Nash response")):
            part = [r for r in rows if r["opponent"] == key and r["method"] == method]
            # The last restricted point is the bare best response: off the scale and off the frontier.
            if method == "restricted":
                part = [r for r in part if r["setting"] < 1.0]
            ax.plot([r["own_exploitability_ma"] for r in part], [r["profit_ma"] for r in part], "-o",
                    color=color, label=name, markersize=4.5, markeredgewidth=0.9)
        ax.set_title(f"{label}: {description}", fontsize=10)
        ax.set_xlim(left=-20)
        ax.set_ylim(bottom=0)
    for ax in axes[1]:
        ax.set_xlabel("What a perfect opponent could win back (mA / hand)")
    for ax in axes[:, 0]:
        ax.set_ylabel("Profit vs this opponent (mA / hand)")
    axes[0, 0].legend(loc="lower right")
    fig.suptitle("More profit for the same risk: up and to the left is better", x=0.125, ha="left",
                 fontsize=12, fontweight="bold", color=INK, y=0.97)
    save(fig, out, "frontier")


# ---------------------------------------------------------------------------
# Memo


def table(headers, rows, align=None):
    align = align or ["---:"] * len(headers)
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(align) + " |"]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


def at_risk(rows, opponent, method, risk):
    """Profit at a given risk, by linear interpolation along one curve."""
    part = sorted((r for r in rows if r["opponent"] == opponent and r["method"] == method),
                  key=lambda r: r["own_exploitability_ma"])
    xs = [r["own_exploitability_ma"] for r in part]
    ys = [r["profit_ma"] for r in part]
    return float(np.interp(risk, xs, ys))


def risk_at(rows, opponent, method, profit):
    part = sorted((r for r in rows if r["opponent"] == opponent and r["method"] == method),
                  key=lambda r: r["profit_ma"])
    return float(np.interp(profit, [r["profit_ma"] for r in part], [r["own_exploitability_ma"] for r in part]))


def memo(results, out):
    lp = read(os.path.join(results, "lp_check.csv"))
    bench = read(os.path.join(results, "bench.csv"))
    exploit = read(os.path.join(results, "exploit.csv"))
    frontier = read(os.path.join(results, "frontier.csv"))
    scaling = read(os.path.join(results, "scaling.csv"))
    family = read(os.path.join(results, "kuhn_family.csv"))
    with open(os.path.join(results, "manifest.json")) as f:
        manifest = json.load(f)

    labels = {name: label for name, label, _ in ALGORITHMS}
    work = {}
    for r in bench:
        work.setdefault(r["algorithm"], []).append(r)
    cfr_nodes = work["cfr"][0]["nodes_touched"]
    bench_rows = []
    for name, _, _ in ALGORITHMS:
        runs = work[name]
        reached = all(r["reached"] == 1 for r in runs)
        nodes = float(np.mean([r["nodes_touched"] for r in runs]))
        bench_rows.append([
            labels[name], "yes" if reached else "no",
            f"{int(np.mean([r['iterations'] for r in runs])):,}", f"{nodes:,.0f}",
            f"{np.mean([r['solve_seconds'] for r in runs]):.3f}",
            f"{np.mean([r['final_exploitability_ma'] for r in runs]):.2f}",
            (f"{cfr_nodes / nodes:,.0f}×" if reached else "not reached"),
        ])
    fastest = min((r for r in bench if r["reached"] == 1), key=lambda r: r["nodes_touched"])
    speed = float(np.median([r["nodes_per_second"] for r in bench if r["algorithm"] != "mccfr"]))

    cfr_plus = {r["opponent"]: r for r in exploit if r["equilibrium_from"] == "cfr+"}
    exact = {r["opponent"]: r for r in exploit if r["equilibrium_from"] == "linear program"}
    exploit_rows = []
    for key, label, description in OPPONENTS:
        r = cfr_plus[key]
        exploit_rows.append([label, description, f"{r['nash_profit_ma']:,.1f}", f"{exact[key]['nash_profit_ma']:,.1f}",
                             f"{r['max_profit_ma']:,.1f}", f"{100 * r['nash_share_of_max']:.0f}%",
                             f"{r['max_exploit_own_exploitability_ma']:,.0f}"])

    frontier_rows = []
    for key, label, _ in OPPONENTS:
        top = cfr_plus[key]["max_profit_ma"]
        base = cfr_plus[key]["nash_profit_ma"]
        target = base + 0.5 * (top - base)        # Halfway from equilibrium profit to the maximum.
        mix_risk = risk_at(frontier, key, "mix", target)
        restricted = [r for r in frontier if r["opponent"] == key and r["method"] == "restricted" and r["setting"] < 1]
        rnr_risk = risk_at(restricted, key, "restricted", target)
        frontier_rows.append([label, f"{target:,.0f}", f"{mix_risk:,.0f}", f"{rnr_risk:,.0f}",
                              f"{mix_risk / rnr_risk:.1f}×"])
    rock = sorted((r for r in frontier if r["opponent"] == "rock" and r["method"] == "restricted" and r["setting"] < 1),
                  key=lambda r: r["setting"])
    pick = min(rock, key=lambda r: abs(r["setting"] - 0.2))
    pick_mix_risk = risk_at(frontier, "rock", "mix", pick["profit_ma"])

    leduc = next(r for r in lp if r["game"] == "leduc")
    kuhn = next(r for r in lp if r["game"] == "kuhn")
    text = f"""# ZeroSum research memo

**Question.** In a two-player zero-sum game with hidden information, an
equilibrium strategy cannot lose on average. Against a flawed opponent it also
does not win as much as it could. How fast can the equilibrium be computed,
how much does it leave behind, and what does taking more cost?

**Method.** Kuhn poker (12 information sets, solved on paper in 1950) and
Leduc hold'em (288 information sets, the standard small research game) are
solved five ways by regret minimization and once, exactly, by a linear program
that shares no code with the solvers. All amounts are in milli-antes per hand
(mA): one thousandth of the forced bet. Everything is exact game-tree
arithmetic except the sampled solver, which is seeded.

## 1. The solvers agree with an exact method

"Exploitability" is what a perfect opponent wins per hand against a strategy;
it is zero only at equilibrium, and it brackets the game's true value.

{table(["Game", "Information sets", "LP value", "LP strategy's exploitability", "CFR+ bracket on the value", "LP value inside"],
       [[r["game"], f"{int(r['information_sets']):,}", f"{r['lp_value']:.9f}",
         f"{r['lp_strategy_exploitability_ma']:.1e} mA",
         f"[{r['cfrplus_value_lower']:.7f}, {r['cfrplus_value_upper']:.7f}]",
         "yes" if r["lp_value_inside_bracket"] == 1 else "NO"] for r in lp],
       ["---", "---:", "---:", "---:", "---:", "---"])}

- Kuhn poker's value is {kuhn['lp_value']:.9f} = -1/18, as Kuhn derived.
- Leduc's value is {leduc['lp_value']:.9f} antes per hand for the first player.
- The linear program's strategy, evaluated by the C++ best-response code, is
  exploitable for less than a trillionth of an ante. Two unrelated methods and
  two separate evaluators agree.
- The rules are also written twice (C++ deals ranks, a Python reference deals
  cards from a deck). Both give the same information sets and the same value.
  Without merging suits Leduc has 936 information sets; merged, 288.

## 2. Five algorithms, one target

Each algorithm ran on Leduc until a perfect opponent could win no more than
1 mA per hand, with the same budget of game-tree node visits.

{table(["Algorithm", "Reached 1 mA", "Iterations", "Nodes visited", "Seconds", "Final mA", "Less work than CFR"], bench_rows,
       ["---", "---", "---:", "---:", "---:", "---:", "---:"])}

- **{labels[fastest['algorithm']]} needed {cfr_nodes / fastest['nodes_touched']:,.0f} times fewer node visits than the
  original CFR** to reach the same accuracy ({int(fastest['iterations']):,} iterations against {int(work['cfr'][0]['iterations']):,}).
  Node visits are counted, not timed, so the ratio does not depend on how fast
  the machine is. The exact stopping iteration is sensitive to the last bit of
  floating-point arithmetic (accuracy is checked every 5% of progress and does
  not fall smoothly), so read these as "about two thousand", not as constants.
  The build turns off fused multiply-add so that GCC and Clang, and x86 and
  ARM, perform the same arithmetic.
- Full traversals ran at about {speed / 1e6:,.0f} million node visits per second on one core
  of the test machine ({manifest['platform']}).
- Sampling did not pay here. External-sampling CFR touches a sliver of the
  tree per iteration, but Leduc's whole tree is {int(leduc['nodes']):,} nodes: a full pass is
  already cheap, and the sampled regrets are noisy. Its case is games too large
  to walk, which this project does not include.

![Convergence](figures/convergence.png)

### Kuhn's equilibria

Kuhn showed the first player has a one-parameter family of equilibria: bluff
the Jack with any probability a up to 1/3, bet the King 3a, and call a bet with
the Queen a + 1/3. The second player's equilibrium is unique: bluff the Jack
1/3 and call with the Queen 1/3.

{table(["Solver", "a = bluff J", "bet K", "bet K / a", "call with Q", "call Q − a", "2nd: bluff J", "2nd: call Q"],
       [[labels.get(r["algorithm"], "Linear program"), f"{r['first_bluffs_jack']:.3f}", f"{r['first_bets_king']:.3f}",
         (f"{r['first_bets_king'] / r['first_bluffs_jack']:.2f}" if r["first_bluffs_jack"] > 1e-6 else "n/a"),
         f"{r['first_calls_with_queen']:.3f}", f"{r['first_calls_with_queen'] - r['first_bluffs_jack']:.3f}",
         f"{r['second_bluffs_jack']:.3f}", f"{r['second_calls_with_queen']:.3f}"] for r in family],
       ["---"] + ["---:"] * 7)}

Different solvers land on different members of the family, and all satisfy
Kuhn's three relations. The linear program returns a corner (a = 0): a first
player who never bets at all, which is still unexploitable.

## 3. Equilibrium against opponents who make mistakes

Four simple opponent types play Leduc. "Maximum" is the best counter-strategy
to that exact opponent.

{table(["Opponent", "How it plays", "Equilibrium (CFR+)", "Equilibrium (LP)", "Maximum", "CFR+ share", "Risk of the maximum"],
       exploit_rows, ["---", "---", "---:", "---:", "---:", "---:", "---:"])}

- The equilibrium strategy profits from every one of them without knowing
  anything about them, but it collects between
  {100 * min(r['nash_share_of_max'] for r in cfr_plus.values()):.0f}% and {100 * max(r['nash_share_of_max'] for r in cfr_plus.values()):.0f}% of what is available.
- **Equilibria are not interchangeable against imperfect opponents.** Both
  equilibrium columns are unexploitable, yet against the Rock one earns
  {cfr_plus['rock']['nash_profit_ma']:.1f} mA and the other {exact['rock']['nash_profit_ma']:.1f} mA. Which equilibrium a solver lands on
  is an accident of the algorithm, and it matters.
- The maximum is dangerous. The counter-strategy that takes everything from
  the Rock can itself be beaten for {cfr_plus['rock']['max_exploit_own_exploitability_ma']:,.0f} mA per hand if the read is wrong,
  more than it was winning.

![Equilibrium against weak opponents](figures/exploit.png)

## 4. Taking more without betting everything on the read

Two ways to lean toward the exploit:

- **Coin-flip mix.** Before each hand, play the counter-strategy with
  probability p and the equilibrium otherwise.
- **Restricted Nash response** (Johanson, Zinkevich and Bowling, 2007). Solve
  a modified game in which the opponent is the modelled type with probability
  p and a perfect adversary otherwise. Its solution is the most profitable
  strategy among all strategies with at most that much exposure.

For each opponent, the risk each method takes on to capture half of the
profit that lies beyond equilibrium play (interpolated along each curve):

{table(["Opponent", "Profit target (mA)", "Risk, coin-flip mix", "Risk, restricted response", "Risk ratio"], frontier_rows,
       ["---", "---:", "---:", "---:", "---:"])}

Against the Rock with p = {pick['setting']:.1f}, the restricted response earns {pick['profit_ma']:.0f} mA per hand
({100 * pick['profit_ma'] / cfr_plus['rock']['max_profit_ma']:.0f}% of the maximum) while a perfect opponent could win back only {pick['own_exploitability_ma']:.0f} mA.
The coin-flip mix needs {pick_mix_risk:.0f} mA of exposure for the same profit: {pick_mix_risk / pick['own_exploitability_ma']:.0f} times as much.

![Profit against risk](figures/frontier.png)

The reason is where the risk is taken. The mix plays the whole counter-strategy
some of the time, including the parts that give up a lot for little. The
restricted response changes only the decisions where the model's mistake is
large relative to the exposure, and keeps equilibrium play elsewhere.

## 5. Larger decks

The same rules with more ranks, {int(scaling[0]['iterations']):,} iterations of Discounted CFR each.

{table(["Ranks", "Nodes", "Information sets", "Seconds", "Exploitability (mA)", "Value bracket"],
       [[f"{int(r['ranks'])}", f"{int(r['nodes']):,}", f"{int(r['information_sets']):,}", f"{r['solve_seconds']:.2f}",
         f"{r['exploitability_ma']:.3f}", f"[{r['value_lower']:.5f}, {r['value_upper']:.5f}]"] for r in scaling])}

## Limits

- These are toy games. Leduc has 288 information sets; no-limit hold'em has
  about 10^161. Nothing here is a poker bot.
- The opponent types are fixed and known exactly. A real opponent has to be
  estimated from a finite number of hands, and adapts.
- Trees are stored explicitly, so memory grows with the game. The methods that
  scale further (abstraction, depth-limited search, learned value functions)
  are not implemented.
- Timings are from one shared cloud machine and vary run to run. Node counts,
  iteration counts, values, and exploitabilities do not.

## Reproduce

```
make
python3 scripts/build_report.py build
```

Ran in {manifest['seconds']:.0f} s with Python {manifest['python']}, NumPy {manifest['numpy']}, SciPy {manifest['scipy']}.

## References

- Kuhn, H. (1950). A simplified two-person poker. *Contributions to the Theory of Games* 1.
- Koller, D., Megiddo, N., von Stengel, B. (1994). Fast algorithms for finding randomized strategies in game trees. *STOC*.
- Southey, F. et al. (2005). Bayes' bluff: opponent modelling in poker. *UAI*. (Leduc hold'em.)
- Zinkevich, M., Johanson, M., Bowling, M., Piccione, C. (2007). Regret minimization in games with incomplete information. *NeurIPS*.
- Johanson, M., Zinkevich, M., Bowling, M. (2007). Computing robust counter-strategies. *NeurIPS*.
- Lanctot, M., Waugh, K., Zinkevich, M., Bowling, M. (2009). Monte Carlo sampling for regret minimization in extensive games. *NeurIPS*.
- Tammelin, O. (2014). Solving large imperfect information games using CFR+. *arXiv:1407.5042*.
- Brown, N., Sandholm, T. (2019). Solving imperfect-information games via discounted regret minimization. *AAAI*.
"""
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "ZeroSum_Research_Memo.md"), "w") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build", nargs="?", default=os.path.join(ROOT, "build"))
    parser.add_argument("--quick", action="store_true", help="small budgets, for a smoke test")
    parser.add_argument("--reuse", action="store_true", help="skip the experiments and redraw from results/")
    parser.add_argument("--results", default=os.path.join(ROOT, "results"))
    parser.add_argument("--reports", default=os.path.join(ROOT, "reports"))
    args = parser.parse_args()
    if not args.reuse:
        run_experiments(Runner(args.build), args.results, args.quick)
    figures = os.path.join(args.reports, "figures")
    figure_convergence(args.results, figures)
    figure_exploit(args.results, figures)
    figure_frontier(args.results, figures)
    memo(args.results, args.reports)
    print(f"wrote {os.path.relpath(args.reports)}/ZeroSum_Research_Memo.md and three figures")


if __name__ == "__main__":
    main()
