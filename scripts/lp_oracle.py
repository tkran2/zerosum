#!/usr/bin/env python3
"""Solve a two-player zero-sum game exactly with a linear program.

This is the independent check on the C++ solvers. It shares no code and no
method with them: the solvers run regret minimization, this file writes the
game in sequence form (Koller, Megiddo and von Stengel, 1994) and hands it to
an off-the-shelf LP solver (SciPy's HiGHS).

    zerosum export leduc > leduc.json
    python3 scripts/lp_oracle.py leduc.json --save leduc_lp.txt
    zerosum eval leduc --strategy leduc_lp.txt     # exploitability ~ 0

A "sequence" is one player's list of own actions so far. Because players
remember what they did, each information set is reached by exactly one of the
owner's sequences, and a strategy becomes a vector of sequence probabilities
tied together by linear "what flows in flows out" constraints. Expected payoff
is then bilinear, x' A y, and the best play against a fixed x is a linear
program whose dual can be merged with the choice of x.
"""

import argparse
import json
import sys

import numpy as np
from scipy.optimize import linprog
from scipy.sparse import coo_matrix, csr_matrix, hstack, vstack


class SequenceForm:
    def __init__(self, tree):
        self.tree = tree
        self.infosets = tree["infosets"]
        nodes = tree["nodes"]

        # Sequence 0 is "no actions yet"; then one per (infoset, action).
        self.seq_of = {}                # (infoset, action) -> sequence index within its player
        self.count = [1, 1]
        for i, infoset in enumerate(self.infosets):
            for a in range(len(infoset["actions"])):
                self.seq_of[(i, a)] = self.count[infoset["player"]]
                self.count[infoset["player"]] += 1
        self.parent = {}                # infoset -> the owner's sequence that leads to it

        payoff = {}
        stack = [(0, 0, 0, 1.0)]        # node, player 0's sequence, player 1's sequence, chance probability
        while stack:
            index, s0, s1, p = stack.pop()
            node = nodes[index]
            if node["t"] == "T":
                payoff[(s0, s1)] = payoff.get((s0, s1), 0.0) + p * node["u"]
            elif node["t"] == "C":
                for child, q in zip(node["c"], node["p"]):
                    if q > 0:
                        stack.append((child, s0, s1, p * q))
            else:
                i = node["i"]
                player = self.infosets[i]["player"]
                mine = s0 if player == 0 else s1
                if self.parent.setdefault(i, mine) != mine:
                    raise ValueError(f"infoset {self.infosets[i]['key']} is reached by two different "
                                     "sequences: the game does not have perfect recall")
                for a, child in enumerate(node["c"]):
                    nxt = self.seq_of[(i, a)]
                    stack.append((child, nxt, s1, p) if player == 0 else (child, s0, nxt, p))

        rows, cols = zip(*payoff.keys())
        self.A = coo_matrix((list(payoff.values()), (rows, cols)), shape=tuple(self.count)).tocsr()
        self.E, self.e = self._flow(0)
        self.F, self.f = self._flow(1)

    def _flow(self, player):
        """Constraints that make a sequence vector a strategy."""
        owned = [i for i, infoset in enumerate(self.infosets) if infoset["player"] == player]
        rows, cols, values = [0], [0], [1.0]           # The empty sequence has probability 1.
        for row, i in enumerate(owned, start=1):
            if i not in self.parent:
                raise ValueError(f"infoset {self.infosets[i]['key']} is never reached")
            rows.append(row); cols.append(self.parent[i]); values.append(-1.0)
            for a in range(len(self.infosets[i]["actions"])):
                rows.append(row); cols.append(self.seq_of[(i, a)]); values.append(1.0)
        matrix = csr_matrix((values, (rows, cols)), shape=(len(owned) + 1, self.count[player]))
        rhs = np.zeros(len(owned) + 1)
        rhs[0] = 1.0
        return matrix, rhs

    def solve(self):
        """Returns (game value to player 0, x, y)."""
        n0, n1 = self.count
        # Player 0: maximize f'q subject to F'q - A'x <= 0, Ex = e, x >= 0.
        # q prices player 1's information sets: f'q is what player 1's best reply concedes.
        k = self.F.shape[0]
        cost = np.concatenate([np.zeros(n0), -self.f])
        first = linprog(cost,
                        A_ub=hstack([-self.A.T, self.F.T]).tocsr(), b_ub=np.zeros(n1),
                        A_eq=hstack([self.E, csr_matrix((self.E.shape[0], k))]).tocsr(), b_eq=self.e,
                        bounds=[(0, None)] * n0 + [(None, None)] * k, method="highs")
        # Player 1: minimize e'p subject to E'p - Ay >= 0, Fy = f, y >= 0.
        k = self.E.shape[0]
        cost = np.concatenate([np.zeros(n1), self.e])
        second = linprog(cost,
                         A_ub=hstack([self.A, -self.E.T]).tocsr(), b_ub=np.zeros(n0),
                         A_eq=hstack([self.F, csr_matrix((self.F.shape[0], k))]).tocsr(), b_eq=self.f,
                         bounds=[(0, None)] * n1 + [(None, None)] * k, method="highs")
        if not (first.success and second.success):
            raise RuntimeError(f"LP failed: {first.message} / {second.message}")
        value_max, value_min = -first.fun, second.fun
        if abs(value_max - value_min) > 1e-7:
            raise RuntimeError(f"the two programs disagree: {value_max} vs {value_min}")
        return value_max, np.clip(first.x[:n0], 0, None), np.clip(second.x[:n1], 0, None)

    def behavior(self, x, y):
        """Sequence probabilities -> the chance of each action at each infoset."""
        out = []
        for i, infoset in enumerate(self.infosets):
            plan = x if infoset["player"] == 0 else y
            reach = plan[self.parent[i]]
            actions = len(infoset["actions"])
            if reach > 1e-12:
                probs = [plan[self.seq_of[(i, a)]] / reach for a in range(actions)]
                total = sum(probs)
                probs = [p / total for p in probs]
            else:
                probs = [1.0 / actions] * actions       # Never reached: any choice is as good.
            out.append(probs)
        return out


def write_strategy(path, tree, behavior):
    with open(path, "w") as f:
        f.write(f"# zerosum strategy for {tree['name']}\n# exact equilibrium from the sequence-form LP\n")
        for infoset, probs in zip(tree["infosets"], behavior):
            for action, p in zip(infoset["actions"], probs):
                f.write(f"{infoset['key']} {action} {float(p)!r}\n")


def solve_tree(tree):
    form = SequenceForm(tree)
    value, x, y = form.solve()
    return value, form.behavior(x, y), form


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tree", help="JSON from `zerosum export GAME`, or - for standard input")
    parser.add_argument("--save", help="write the equilibrium as a zerosum strategy file")
    args = parser.parse_args()
    tree = json.load(sys.stdin if args.tree == "-" else open(args.tree))
    value, behavior, form = solve_tree(tree)
    print(f"game {tree['name']}")
    print(f"sequences {form.count[0]} {form.count[1]}")
    print(f"payoff_entries {form.A.nnz}")
    print(f"value_to_first_player {value:.12f}")
    if args.save:
        write_strategy(args.save, tree, behavior)


if __name__ == "__main__":
    main()
