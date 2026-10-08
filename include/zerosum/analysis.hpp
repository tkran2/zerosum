#pragma once

#include "zerosum/game.hpp"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zerosum {

// Expected payoff to player 0 when both players follow `strategy`.
inline double expected_value(const Game& game, const Strategy& strategy, std::int32_t index = 0) {
    const Node& node = game.nodes[static_cast<std::size_t>(index)];
    if (node.type == NodeType::Terminal) return node.utility;
    double value = 0;
    if (node.type == NodeType::Chance) {
        for (std::int32_t a = 0; a < node.child_count; ++a)
            value += game.probability(node, a) * expected_value(game, strategy, game.child(node, a));
        return value;
    }
    const auto& infoset = game.infosets[static_cast<std::size_t>(node.infoset)];
    for (std::int32_t a = 0; a < node.child_count; ++a) {
        const double p = strategy[static_cast<std::size_t>(infoset.offset + a)];
        if (p > 0) value += p * expected_value(game, strategy, game.child(node, a));
    }
    return value;
}

namespace detail {

// A set of histories the responder cannot tell apart, each weighted by the
// probability that chance and the opponent bring the game there.
using Weighted = std::vector<std::pair<std::int32_t, double>>;

inline double respond(const Game& game, const Strategy& opponent, int me, Weighted frontier,
                      Strategy* choice) {
    double total = 0;
    std::map<std::int32_t, Weighted> mine; // My next decisions, grouped by what I will know.
    while (!frontier.empty()) {
        const auto [index, weight] = frontier.back();
        frontier.pop_back();
        if (weight == 0) continue;
        const Node& node = game.nodes[static_cast<std::size_t>(index)];
        if (node.type == NodeType::Terminal) {
            total += weight * (me == 0 ? node.utility : -node.utility);
        } else if (node.type == NodeType::Chance) {
            for (std::int32_t a = 0; a < node.child_count; ++a)
                frontier.emplace_back(game.child(node, a), weight * game.probability(node, a));
        } else if (node.player != me) {
            const auto& infoset = game.infosets[static_cast<std::size_t>(node.infoset)];
            for (std::int32_t a = 0; a < node.child_count; ++a)
                frontier.emplace_back(game.child(node, a),
                    weight * opponent[static_cast<std::size_t>(infoset.offset + a)]);
        } else {
            mine[node.infoset].emplace_back(index, weight);
        }
    }
    // At each of my infosets I must pick ONE action for every history in it,
    // so the comparison is between sums over the whole set.
    for (const auto& [id, members] : mine) {
        const auto& infoset = game.infosets[static_cast<std::size_t>(id)];
        double best = 0;
        std::int32_t best_action = 0;
        for (std::int32_t a = 0; a < infoset.action_count; ++a) {
            Weighted next;
            next.reserve(members.size());
            for (const auto& [index, weight] : members)
                next.emplace_back(game.child(game.nodes[static_cast<std::size_t>(index)], a), weight);
            const double value = respond(game, opponent, me, std::move(next), choice);
            if (a == 0 || value > best) {
                best = value;
                best_action = a;
            }
        }
        total += best;
        if (choice)
            for (std::int32_t a = 0; a < infoset.action_count; ++a)
                (*choice)[static_cast<std::size_t>(infoset.offset + a)] = a == best_action ? 1.0 : 0.0;
    }
    return total;
}

} // namespace detail

// The most `me` can win, on average, against the other player's part of
// `strategy`. If `response` is given it receives a strategy that achieves it
// (the opponent's part is copied unchanged).
inline double best_response_value(const Game& game, const Strategy& strategy, int me,
                                  Strategy* response = nullptr) {
    if (response) *response = strategy;
    return detail::respond(game, strategy, me, {{0, 1.0}}, response);
}

struct Exploitability {
    double value{};        // Payoff to player 0 when both follow the strategy.
    double best_vs_p1{};   // Player 0's best-response payoff against player 1's part.
    double best_vs_p0{};   // Player 1's best-response payoff against player 0's part.
    // Average amount per hand a perfect opponent wins from this strategy. Zero
    // exactly at a Nash equilibrium.
    [[nodiscard]] double exploitability() const { return 0.5 * (best_vs_p1 + best_vs_p0); }
    // The game's true value lies between these two numbers.
    [[nodiscard]] double value_lower() const { return -best_vs_p0; }
    [[nodiscard]] double value_upper() const { return best_vs_p1; }
};

inline Exploitability exploitability(const Game& game, const Strategy& strategy) {
    Exploitability e;
    e.value = expected_value(game, strategy);
    e.best_vs_p1 = best_response_value(game, strategy, 0);
    e.best_vs_p0 = best_response_value(game, strategy, 1);
    return e;
}

// Take player `who`'s decisions from `source` and leave the rest of `base`.
inline Strategy with_player(const Game& game, Strategy base, const Strategy& source, int who) {
    for (const auto& infoset : game.infosets)
        if (infoset.player == who)
            for (std::int32_t a = 0; a < infoset.action_count; ++a)
                base[static_cast<std::size_t>(infoset.offset + a)] =
                    source[static_cast<std::size_t>(infoset.offset + a)];
    return base;
}

// Probability that player `who`'s own choices lead to each of its infosets.
// (Perfect recall makes this the same for every history in an infoset.)
inline std::vector<double> own_reach(const Game& game, const Strategy& strategy, int who) {
    std::vector<double> reach(game.infosets.size(), 0.0);
    std::function<void(std::int32_t, double)> walk = [&](std::int32_t index, double mine) {
        const Node& node = game.nodes[static_cast<std::size_t>(index)];
        if (node.type == NodeType::Terminal) return;
        if (node.type == NodeType::Decision && node.player == who) {
            const auto& infoset = game.infosets[static_cast<std::size_t>(node.infoset)];
            reach[static_cast<std::size_t>(node.infoset)] = mine;
            for (std::int32_t a = 0; a < node.child_count; ++a)
                walk(game.child(node, a), mine * strategy[static_cast<std::size_t>(infoset.offset + a)]);
            return;
        }
        for (std::int32_t a = 0; a < node.child_count; ++a) walk(game.child(node, a), mine);
    };
    walk(0, 1.0);
    return reach;
}

// Player `who` flips a coin once, before the hand: with probability `share`
// play `b` for the whole hand, otherwise `a`. Returns the single behavior
// strategy that is equivalent to that coin flip (Kuhn's theorem).
inline Strategy mix(const Game& game, const Strategy& a, const Strategy& b, double share, int who) {
    const auto reach_a = own_reach(game, a, who);
    const auto reach_b = own_reach(game, b, who);
    Strategy out = a;
    for (std::size_t id = 0; id < game.infosets.size(); ++id) {
        const auto& infoset = game.infosets[id];
        if (infoset.player != who) continue;
        const double wa = (1.0 - share) * reach_a[id];
        const double wb = share * reach_b[id];
        if (wa + wb <= 0) continue;
        for (std::int32_t k = 0; k < infoset.action_count; ++k) {
            const auto i = static_cast<std::size_t>(infoset.offset + k);
            out[i] = (wa * a[i] + wb * b[i]) / (wa + wb);
        }
    }
    return out;
}

// Both seats in one profile: seat 0's decisions from `first`, seat 1's from `second`.
inline Strategy combine(const Game& game, const Strategy& first, const Strategy& second) {
    return with_player(game, first, second, 1);
}

// The restricted Nash response game of Johanson, Zinkevich and Bowling (2007).
// Before the hand a hidden coin decides who player `me` faces: with
// probability `confidence` an opponent locked into `model`, otherwise an
// opponent free to play anything. `me` cannot tell which, so its infosets are
// shared between the two copies of the tree. An equilibrium of this game is
// the most profitable strategy against the model among all strategies that are
// at most a given amount exploitable; sweeping `confidence` from 0 to 1 traces
// that whole trade-off, from the equilibrium to the pure best response.
//
// The result has the same infosets as `game`, so any solver runs on it
// unchanged and its strategy can be evaluated directly in the original game.
inline Game restricted_game(const Game& game, const Strategy& model, double confidence, int me) {
    if (confidence < 0 || confidence > 1) throw std::invalid_argument("confidence must be in [0, 1]");
    const auto n = static_cast<std::int32_t>(game.nodes.size());
    const auto c = static_cast<std::int32_t>(game.children.size());
    Game out;
    out.name = game.name + "-restricted";
    out.infosets = game.infosets;
    out.action_slots = game.action_slots;
    out.rank_names = game.rank_names;
    out.nodes.reserve(static_cast<std::size_t>(2 * n + 1));
    out.children.reserve(static_cast<std::size_t>(2 * c + 2));
    out.chance_prob.reserve(static_cast<std::size_t>(2 * c + 2));

    Node root;
    root.type = NodeType::Chance;
    root.first_child = 0;
    root.child_count = 2;
    out.nodes.push_back(root);
    out.children = {1, 1 + n};
    out.chance_prob = {confidence, 1.0 - confidence};

    for (int copy = 0; copy < 2; ++copy) {
        const std::int32_t node_shift = 1 + copy * n;
        const std::int32_t child_shift = 2 + copy * c;
        for (const Node& original : game.nodes) {
            Node node = original;
            node.first_child = original.first_child + child_shift;
            // In the first copy the opponent has no choices left: its decisions
            // become chance moves with the model's probabilities.
            if (copy == 0 && original.type == NodeType::Decision && original.player != me) {
                node.type = NodeType::Chance;
                node.player = -1;
                node.infoset = -1;
            }
            out.nodes.push_back(node);
        }
        for (std::int32_t i = 0; i < c; ++i) {
            out.children.push_back(game.children[static_cast<std::size_t>(i)] + node_shift);
            out.chance_prob.push_back(game.chance_prob[static_cast<std::size_t>(i)]);
        }
        if (copy == 0)
            for (const Node& original : game.nodes) {
                if (original.type != NodeType::Decision || original.player == me) continue;
                const auto& infoset = game.infosets[static_cast<std::size_t>(original.infoset)];
                for (std::int32_t a = 0; a < original.child_count; ++a)
                    out.chance_prob[static_cast<std::size_t>(child_shift + original.first_child + a)] =
                        model[static_cast<std::size_t>(infoset.offset + a)];
            }
    }
    return out;
}

// Simple opponent types. Each is a complete strategy for both seats.
inline Strategy archetype(const Game& game, const std::string& kind) {
    Strategy s(static_cast<std::size_t>(game.action_slots), 0.0);
    const int top = static_cast<int>(game.rank_names.size()) - 1;
    for (const auto& infoset : game.infosets) {
        const auto slot = [&](char action) -> double& {
            return s[static_cast<std::size_t>(infoset.offset) + infoset.actions.find(action)];
        };
        const bool has = infoset.actions.find('b') != std::string::npos ||
                         infoset.actions.find('r') != std::string::npos;
        const char aggressive = infoset.actions[0] == 'k' ? 'b' : 'r';
        const char passive = infoset.actions[0] == 'k' ? 'k' : 'c';
        const bool pair = infoset.public_rank >= 0 && infoset.public_rank == infoset.private_rank;
        const bool strong = pair || infoset.private_rank == top;
        if (kind == "random") {
            for (std::int32_t a = 0; a < infoset.action_count; ++a)
                s[static_cast<std::size_t>(infoset.offset + a)] = 1.0 / infoset.action_count;
        } else if (kind == "station") {
            slot(passive) = 1.0; // Never folds, never bets.
        } else if (kind == "maniac") {
            slot(has ? aggressive : passive) = 1.0; // Bets and raises whenever allowed.
        } else if (kind == "rock") {
            // Never bluffs: puts money in only with the top card or a pair.
            if (strong) slot(has ? aggressive : passive) = 1.0;
            else slot(infoset.actions[0] == 'k' ? 'k' : 'f') = 1.0;
        } else {
            throw std::invalid_argument("unknown opponent type '" + kind + "'");
        }
    }
    return s;
}

} // namespace zerosum
