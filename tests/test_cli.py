#!/usr/bin/env python3
"""Command-line tests, and the linear-program cross-check of the solvers.

    python3 tests/test_cli.py build

The first group needs only the standard library. The second group needs NumPy
and SciPy and is skipped, with a message, when they are not installed.
"""

import csv
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, os.path.join(ROOT, "scripts"))

try:
    import lp_oracle
    import reference_games
    HAVE_LP = True
except ImportError:  # NumPy or SciPy is missing.
    HAVE_LP = False


def run(*args, stdin=None, expect=0):
    done = subprocess.run([os.path.join(BUILD, "zerosum"), *map(str, args)],
                          input=stdin, capture_output=True, text=True, timeout=300)
    if done.returncode != expect:
        raise AssertionError(f"zerosum {' '.join(map(str, args))} exited {done.returncode}: {done.stderr}")
    return done.stdout if expect == 0 else done.stderr


def table(text):
    return list(csv.DictReader(io.StringIO(text)))


def fields(text):
    return {line.split()[0]: line.split()[1:] for line in text.splitlines()}


class CommandLine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.leduc = os.path.join(cls.tmp.name, "leduc.txt")
        cls.kuhn = os.path.join(cls.tmp.name, "kuhn.txt")
        run("solve", "leduc", "--iterations", 3000, "--save", cls.leduc)
        run("solve", "kuhn", "--iterations", 20000, "--save", cls.kuhn)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_info_reports_the_published_game_sizes(self):
        self.assertIn("information_sets 12 (player 0: 6, player 1: 6)", run("info", "kuhn"))
        out = run("info", "leduc")
        self.assertIn("information_sets 288 ", out)
        self.assertIn("nodes 1936 ", out)

    def test_solve_reports_a_shrinking_bracket_as_csv(self):
        rows = table(run("solve", "leduc", "--algo", "dcfr", "--iterations", 400, "--every", 100, "--csv"))
        self.assertEqual([int(r["iteration"]) for r in rows], [100, 200, 300, 400])
        gaps = [float(r["exploitability_ma"]) for r in rows]
        self.assertEqual(gaps, sorted(gaps, reverse=True))
        for r in rows:
            self.assertLessEqual(float(r["value_lower"]), -0.0856064)
            self.assertGreaterEqual(float(r["value_upper"]), -0.0856065)
            self.assertEqual(r["algorithm"], "dcfr")
        self.assertLess(int(rows[0]["nodes_touched"]), int(rows[-1]["nodes_touched"]))

    def test_a_saved_strategy_round_trips_exactly(self):
        solved = run("solve", "leduc", "--iterations", 3000)
        gap = float(solved.split("exploitability")[1].split()[0])
        report = fields(run("eval", "leduc", "--strategy", self.leduc))
        self.assertAlmostEqual(float(report["exploitability_ma_per_hand"][0]), gap, places=4)
        low, high = map(float, report["game_value_between"])
        self.assertLess(low, -0.0856064)
        self.assertGreater(high, -0.0856065)
        self.assertLess(high - low, 2e-4)
        shown = run("show", "kuhn", "--strategy", self.kuhn).splitlines()
        self.assertEqual(len(shown), 12)
        king = next(line for line in shown if line.startswith("1:K:-:b"))
        self.assertEqual(king.split()[1:], ["f", "0.000", "c", "1.000"])

    def test_exploit_table(self):
        rows = {r["opponent"]: r for r in table(run("exploit", "leduc", "--strategy", self.leduc, "--csv"))}
        self.assertEqual(set(rows), {"random", "station", "maniac", "rock"})
        for r in rows.values():
            nash, best = float(r["nash_profit_ma"]), float(r["max_profit_ma"])
            self.assertGreater(nash, 0)
            self.assertLess(nash, best)
            # What the best counter-strategy wins is exactly what the opponent leaks.
            self.assertAlmostEqual(best, float(r["opponent_exploitability_ma"]), places=4)
            self.assertGreater(float(r["max_exploit_own_exploitability_ma"]), 100)
        self.assertAlmostEqual(float(rows["rock"]["max_profit_ma"]), 700.0, places=4)
        self.assertIn("equilibrium", run("exploit", "leduc", "--strategy", self.leduc))

    def test_frontier_has_both_methods_and_the_restricted_one_dominates(self):
        rows = table(run("frontier", "leduc", "--strategy", self.leduc, "--opponent", "rock",
                         "--steps", 5, "--iterations", 1500))
        mixed = [(float(r["profit_ma"]), float(r["own_exploitability_ma"])) for r in rows if r["method"] == "mix"]
        restricted = [(float(r["profit_ma"]), float(r["own_exploitability_ma"])) for r in rows if r["method"] == "restricted"]
        self.assertEqual(len(mixed), 6)
        self.assertEqual(len(restricted), 6)
        self.assertAlmostEqual(mixed[-1][0], 700.0, places=3)
        # Coin-flip mixing is a straight line in profit.
        step = mixed[1][0] - mixed[0][0]
        for a, b in zip(mixed, mixed[1:]):
            self.assertAlmostEqual(b[0] - a[0], step, places=3)

        def mix_risk_at(profit):  # Interpolate the mixing curve.
            for (p0, r0), (p1, r1) in zip(mixed, mixed[1:]):
                if p0 <= profit <= p1:
                    return r0 + (r1 - r0) * (profit - p0) / (p1 - p0)
            raise AssertionError(profit)

        for profit, risk in restricted[1:-1]:
            self.assertLess(risk, 0.6 * mix_risk_at(profit))

    def test_bench_reports_each_algorithm(self):
        rows = table(run("bench", "kuhn", "--target", 5, "--max-nodes", 5e7, "--trials", 2))
        self.assertEqual([r["algorithm"] for r in rows], ["cfr", "lcfr", "dcfr", "cfr+", "mccfr", "mccfr"])
        for r in rows:
            self.assertEqual(r["reached"], "1", r)
            self.assertLessEqual(float(r["final_exploitability_ma"]), 5.0)
        # A budget too small to finish is reported as not reached, not as success.
        starved = table(run("bench", "leduc", "--target", 0.001, "--max-nodes", 2e5, "--trials", 1))
        self.assertEqual({r["reached"] for r in starved}, {"0"})

    def test_export_is_a_well_formed_tree(self):
        tree = json.loads(run("export", "leduc"))
        self.assertEqual(len(tree["nodes"]), 1936)
        self.assertEqual(len(tree["infosets"]), 288)
        seen = [0] * len(tree["nodes"])
        for node in tree["nodes"]:
            for child in node.get("c", []):
                seen[child] += 1
            if node["t"] == "C":
                self.assertAlmostEqual(sum(node["p"]), 1.0, places=12)
            if node["t"] == "D":
                self.assertEqual(len(node["c"]), len(tree["infosets"][node["i"]]["actions"]))
        self.assertEqual(seen[0], 0)
        self.assertEqual(set(seen[1:]), {1})  # Every other node has exactly one parent.

    def test_play_deals_hands_and_keeps_score(self):
        out = run("play", "kuhn", "--strategy", self.kuhn, "--seed", 4, stdin="k\nc\nb\nk\nf\nb\nq\n")
        self.assertIn("You are player 0", out)
        self.assertIn("your card", out)
        self.assertIn("result:", out)
        self.assertIn("antes per hand over", out)
        again = run("play", "kuhn", "--strategy", self.kuhn, "--seed", 4, stdin="k\nc\nb\nk\nf\nb\nq\n")
        self.assertEqual(out, again)  # Same seed, same deals.
        other_seat = run("play", "leduc", "--strategy", self.leduc, "--seat", 1, "--seed", 2, stdin="x\nk\nq\n")
        self.assertIn("You are player 1", other_seat)

    def test_bad_input_is_rejected_with_exit_code_2(self):
        self.assertIn("unknown game", run("info", "holdem", expect=2))
        self.assertIn("unknown command", run("sing", "kuhn", expect=2))
        self.assertIn("unknown algorithm", run("solve", "kuhn", "--algo", "simplex", expect=2))
        self.assertIn("unknown option", run("solve", "kuhn", "--fast", expect=2))
        self.assertIn("missing value", run("solve", "kuhn", "--iterations", expect=2))
        self.assertIn("--strategy FILE is required", run("eval", "kuhn", expect=2))
        self.assertIn("different game", run("eval", "kuhn", "--strategy", self.leduc, expect=2))
        self.assertIn("unknown opponent", run("frontier", "kuhn", "--strategy", self.kuhn, "--opponent", "shark", expect=2))
        broken = os.path.join(self.tmp.name, "broken.txt")
        with open(self.kuhn) as source, open(broken, "w") as out:
            lines = source.readlines()
            out.writelines(lines[:-1])  # One probability is missing.
        self.assertIn("missing", run("eval", "kuhn", "--strategy", broken, expect=2))
        with open(broken, "w") as out:
            data = next(i for i, line in enumerate(lines) if not line.startswith("#"))
            key, action, probability = lines[data].split()
            lines[data] = f"{key} {action} {(float(probability) + 0.25) % 1.0}\n"
            out.writelines(lines)
        self.assertIn("do not sum to 1", run("eval", "kuhn", "--strategy", broken, expect=2))
        self.assertIn("Usage", run("help"))


@unittest.skipUnless(HAVE_LP, "NumPy and SciPy are needed for the linear-program check")
class LinearProgramOracle(unittest.TestCase):
    """Regret minimization against an exact method that shares nothing with it."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def lp(self, tree, name):
        value, behavior, form = lp_oracle.solve_tree(tree)
        path = os.path.join(self.tmp.name, name)
        lp_oracle.write_strategy(path, tree, behavior)
        return value, path, form

    def test_kuhn_value_is_minus_one_eighteenth(self):
        value, path, form = self.lp(json.loads(run("export", "kuhn")), "kuhn_lp.txt")
        self.assertAlmostEqual(value, -1 / 18, places=12)
        self.assertEqual(form.count, [13, 13])
        report = fields(run("eval", "kuhn", "--strategy", path))
        self.assertLess(float(report["exploitability_ma_per_hand"][0]), 1e-9)

    def test_lp_equilibrium_is_unexploitable_according_to_the_cpp_evaluator(self):
        for game in ("leduc", "leduc5"):
            value, path, _ = self.lp(json.loads(run("export", game)), game + "_lp.txt")
            report = fields(run("eval", game, "--strategy", path))
            self.assertLess(float(report["exploitability_ma_per_hand"][0]), 1e-6)
            self.assertAlmostEqual(float(report["value_to_first_player"][0]), value, places=9)

    def test_every_solver_brackets_the_lp_value(self):
        value, _, _ = self.lp(json.loads(run("export", "leduc")), "leduc_lp.txt")
        self.assertAlmostEqual(value, -0.085606424078, places=10)
        for algo, iterations in (("cfr", 3000), ("lcfr", 3000), ("dcfr", 3000), ("cfr+", 3000), ("mccfr", 300000)):
            row = table(run("solve", "leduc", "--algo", algo, "--iterations", iterations, "--csv"))[0]
            self.assertLessEqual(float(row["value_lower"]), value + 1e-9, algo)
            self.assertGreaterEqual(float(row["value_upper"]), value - 1e-9, algo)
        best = table(run("solve", "leduc", "--algo", "cfr+", "--iterations", 20000, "--csv"))[0]
        self.assertLess(float(best["value_upper"]) - float(best["value_lower"]), 2e-5)

    def test_rules_written_twice_give_the_same_game(self):
        for game, expected in (("kuhn", -1 / 18), ("leduc", -0.085606424078)):
            reference = reference_games.build(game)
            exported = json.loads(run("export", game))
            self.assertEqual(sorted((i["key"], i["actions"], i["player"]) for i in reference["infosets"]),
                             sorted((i["key"], i["actions"], i["player"]) for i in exported["infosets"]))
            value, path, _ = self.lp(reference, game + "_reference.txt")
            self.assertAlmostEqual(value, expected, places=10)
            # An equilibrium found from the Python rules is unexploitable under the C++ rules.
            report = fields(run("eval", game, "--strategy", path))
            self.assertLess(float(report["exploitability_ma_per_hand"][0]), 1e-6)

    def test_unmerged_suits_give_936_information_sets_and_the_same_value(self):
        tree = reference_games.build("leduc", suits=True)
        self.assertEqual(len(tree["infosets"]), 936)
        value, _, _ = lp_oracle.solve_tree(tree)
        self.assertAlmostEqual(value, -0.085606424078, places=10)

    def test_imperfect_recall_is_refused(self):
        tree = json.loads(run("export", "kuhn"))
        # Point one of the first player's later decisions at an earlier infoset.
        later = next(n for n in tree["nodes"] if n["t"] == "D" and tree["infosets"][n["i"]]["key"].endswith(":kb"))
        first = next(i for i, s in enumerate(tree["infosets"]) if s["key"] == "0:J:-:")
        later["i"] = first
        with self.assertRaises(ValueError):
            lp_oracle.SequenceForm(tree)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        BUILD = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
