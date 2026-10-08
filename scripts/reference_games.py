#!/usr/bin/env python3
"""Kuhn and Leduc poker written a second time, from the published rules.

The C++ builder deals RANKS with the right probabilities. This file deals
actual CARDS from a deck (so Leduc has 30 ordered deals, not 9) and keeps its
own betting logic. If both descriptions of the rules lead to the same game
value under the linear program, a mistake would have to be made identically in
two differently shaped implementations to go unnoticed.

    python3 scripts/reference_games.py leduc > leduc_reference.json

The output has the same JSON layout as `zerosum export`. With --suits each
card is its own information (936 Leduc information sets, the count quoted by
libraries that do not merge suits); the default merges suits (288).
"""

import argparse
import itertools
import json
import sys


def build(game, suits=False):
    if game == "kuhn":
        deck = [("J", 0), ("Q", 0), ("K", 0)]
        rounds, bet_size, max_bets = 1, [1], 1
    elif game == "leduc":
        deck = [(rank, suit) for rank in "JQK" for suit in (0, 1)]
        rounds, bet_size, max_bets = 2, [2, 4], 2
    else:
        raise ValueError("game must be kuhn or leduc")
    order = {"J": 0, "Q": 1, "K": 2}
    label = (lambda card: card[0] + "sh"[card[1]]) if suits else (lambda card: card[0])

    nodes, infosets, ids = [], [], {}

    def add(node):
        nodes.append(node)
        return len(nodes) - 1

    def strength(card, board):
        return (10 if board is not None and card[0] == board[0] else 0) + order[card[0]]

    def play(hands, board, rnd, lines, pot):
        """lines: betting so far, one string per round. pot: chips put in by each player."""
        line = lines[rnd]
        player = len(line) % 2
        to_call = pot[1 - player] - pot[player]
        bets = sum(1 for action in line if action in "br")
        actions = (["f", "c"] if to_call else ["k"])
        if bets < max_bets:
            actions.append("r" if to_call else "b")

        key = f"{player}:{label(hands[player])}:{label(board) if board else '-'}:{'/'.join(lines[:rnd + 1])}"
        if key not in ids:
            ids[key] = len(infosets)
            infosets.append({"player": player, "key": key, "actions": "".join(actions)})
        index = add({"t": "D", "c": [], "i": ids[key]})

        for action in actions:
            new_lines = lines[:rnd] + [line + action]
            new_pot = list(pot)
            if action == "f":
                # The folder loses what they already put in.
                child = add({"t": "T", "u": float(-pot[0] if player == 0 else pot[1])})
            elif action in "br":
                new_pot[player] = pot[1 - player] + bet_size[rnd]
                child = play(hands, board, rnd, new_lines, new_pot)
            else:
                if action == "c":
                    new_pot[player] = pot[1 - player]
                closes = action == "c" or (action == "k" and line == "k")
                if not closes:
                    child = play(hands, board, rnd, new_lines, new_pot)
                elif rnd + 1 < rounds:
                    child = reveal(hands, new_lines, new_pot)
                else:
                    a, b = strength(hands[0], board), strength(hands[1], board)
                    child = add({"t": "T", "u": float(0 if a == b else new_pot[1] if a > b else -new_pot[0])})
            nodes[index]["c"].append(child)
        return index

    def reveal(hands, lines, pot):
        rest = [card for card in deck if card not in hands]
        index = add({"t": "C", "c": [], "p": [1.0 / len(rest)] * len(rest)})
        for card in rest:
            nodes[index]["c"].append(play(hands, card, 1, lines + [""], pot))
        return index

    deals = list(itertools.permutations(deck, 2))
    root = add({"t": "C", "c": [], "p": [1.0 / len(deals)] * len(deals)})
    for hands in deals:
        nodes[root]["c"].append(play(hands, None, 0, [""], [1, 1]))
    return {"name": game, "nodes": nodes, "infosets": infosets}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("game", choices=["kuhn", "leduc"])
    parser.add_argument("--suits", action="store_true", help="do not merge suits into ranks")
    args = parser.parse_args()
    json.dump(build(args.game, args.suits), sys.stdout, separators=(",", ":"))
    print()


if __name__ == "__main__":
    main()
