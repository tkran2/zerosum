#pragma once

#include "zerosum/game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace zerosum {

// splitmix64: small, fast, and identical on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    double uniform() { return static_cast<double>(next() >> 11U) * (1.0 / 9007199254740992.0); }

private:
    std::uint64_t state_;
};

enum class Algorithm {
    Cfr,     // Zinkevich et al. (2007): the original, simultaneous updates.
    CfrPlus, // Tammelin (2014): regrets floored at zero, alternating updates, linear averaging.
    Linear,  // Brown and Sandholm (2019): iteration t weighted by t.
    Dcfr,    // Brown and Sandholm (2019): discounting with alpha 1.5, beta 0, gamma 2.
    Mccfr,   // Lanctot et al. (2009): external sampling.
};

inline const char* algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::Cfr: return "cfr";
    case Algorithm::CfrPlus: return "cfr+";
    case Algorithm::Linear: return "lcfr";
    case Algorithm::Dcfr: return "dcfr";
    case Algorithm::Mccfr: return "mccfr";
    }
    return "?";
}

inline Algorithm parse_algorithm(const std::string& name) {
    for (const auto algorithm : {Algorithm::Cfr, Algorithm::CfrPlus, Algorithm::Linear,
                                 Algorithm::Dcfr, Algorithm::Mccfr})
        if (name == algorithm_name(algorithm)) return algorithm;
    throw std::invalid_argument("unknown algorithm '" + name + "' (cfr, cfr+, lcfr, dcfr, mccfr)");
}

// Counterfactual regret minimization. Each iteration plays the game against
// itself, and at every decision asks: how much better would each action have
// done than what I actually play? Those regrets accumulate, and the next
// strategy plays each action in proportion to its positive regret. The
// AVERAGE strategy over all iterations converges to a Nash equilibrium.
class Solver {
public:
    Solver(const Game& game, Algorithm algorithm, std::uint64_t seed = 1)
        : game_(game), algorithm_(algorithm), rng_(seed),
          regret_(static_cast<std::size_t>(game.action_slots), 0.0),
          strategy_sum_(static_cast<std::size_t>(game.action_slots), 0.0),
          current_(uniform_strategy(game)) {}

    void iterate() {
        ++iteration_;
        const double t = static_cast<double>(iteration_);
        switch (algorithm_) {
        case Algorithm::Cfr:
            // Both players are updated against the same frozen strategy.
            refresh_current();
            weight_ = 1.0;
            walk(0, 0, 1.0, 1.0);
            walk(0, 1, 1.0, 1.0);
            break;
        case Algorithm::CfrPlus:
            weight_ = t;
            for (int player = 0; player < 2; ++player) {
                refresh_current();
                walk(0, player, 1.0, 1.0);
                // Regret-matching+: never carry a negative regret forward.
                for (const auto& infoset : game_.infosets)
                    if (infoset.player == player)
                        for (std::int32_t a = 0; a < infoset.action_count; ++a) {
                            auto& r = regret_[static_cast<std::size_t>(infoset.offset + a)];
                            if (r < 0) r = 0;
                        }
            }
            break;
        case Algorithm::Linear:
        case Algorithm::Dcfr: {
            weight_ = 1.0;
            for (int player = 0; player < 2; ++player) {
                refresh_current();
                walk(0, player, 1.0, 1.0);
            }
            // Shrink what has accumulated so that later iterations count more.
            const bool dcfr = algorithm_ == Algorithm::Dcfr;
            // t^1.5 as t * sqrt(t): sqrt is exactly rounded everywhere, pow is not,
            // and results here should not depend on the platform's math library.
            const double t15 = t * std::sqrt(t);
            const double positive = dcfr ? t15 / (t15 + 1.0) : t / (t + 1.0);
            const double negative = dcfr ? 0.5 : t / (t + 1.0);
            const double average = dcfr ? (t / (t + 1.0)) * (t / (t + 1.0)) : t / (t + 1.0);
            for (auto& r : regret_) r *= r > 0 ? positive : negative;
            for (auto& s : strategy_sum_) s *= average;
            break;
        }
        case Algorithm::Mccfr:
            for (int player = 0; player < 2; ++player) sample(0, player);
            break;
        }
    }

    // The equilibrium approximation: each action's share of the accumulated play.
    [[nodiscard]] Strategy average_strategy() const {
        Strategy out(strategy_sum_.size());
        for (const auto& infoset : game_.infosets) {
            double total = 0;
            for (std::int32_t a = 0; a < infoset.action_count; ++a)
                total += strategy_sum_[static_cast<std::size_t>(infoset.offset + a)];
            for (std::int32_t a = 0; a < infoset.action_count; ++a) {
                const auto i = static_cast<std::size_t>(infoset.offset + a);
                out[i] = total > 0 ? strategy_sum_[i] / total : 1.0 / infoset.action_count;
            }
        }
        return out;
    }

    [[nodiscard]] std::uint64_t iteration() const { return iteration_; }
    [[nodiscard]] std::uint64_t nodes_touched() const { return touched_; }
    [[nodiscard]] Algorithm algorithm() const { return algorithm_; }

private:
    // Regret matching for one infoset, written into `out`.
    void match(const Infoset& infoset, double* out) const {
        double total = 0;
        for (std::int32_t a = 0; a < infoset.action_count; ++a) {
            const double r = regret_[static_cast<std::size_t>(infoset.offset + a)];
            out[a] = r > 0 ? r : 0;
            total += out[a];
        }
        for (std::int32_t a = 0; a < infoset.action_count; ++a)
            out[a] = total > 0 ? out[a] / total : 1.0 / infoset.action_count;
    }

    void refresh_current() {
        for (const auto& infoset : game_.infosets)
            match(infoset, current_.data() + infoset.offset);
    }

    // Full-tree traversal that updates `me`. reach_me is the probability that
    // my own choices lead here; reach_other is the same for the opponent and
    // chance together. Returns the node's value to `me` under current play.
    double walk(std::int32_t index, int me, double reach_me, double reach_other) {
        ++touched_;
        const Node& node = game_.nodes[static_cast<std::size_t>(index)];
        if (node.type == NodeType::Terminal) return me == 0 ? node.utility : -node.utility;
        if (reach_me == 0 && reach_other == 0) return 0; // Nothing here can change anything.
        if (node.type == NodeType::Chance) {
            double value = 0;
            for (std::int32_t a = 0; a < node.child_count; ++a) {
                const double p = game_.probability(node, a);
                value += p * walk(game_.child(node, a), me, reach_me, reach_other * p);
            }
            return value;
        }
        const Infoset& infoset = game_.infosets[static_cast<std::size_t>(node.infoset)];
        const double* sigma = current_.data() + infoset.offset;
        if (node.player != me) {
            double value = 0;
            for (std::int32_t a = 0; a < node.child_count; ++a)
                value += sigma[a] * walk(game_.child(node, a), me, reach_me, reach_other * sigma[a]);
            return value;
        }
        double action_value[4];
        double value = 0;
        for (std::int32_t a = 0; a < node.child_count; ++a) {
            action_value[a] = walk(game_.child(node, a), me, reach_me * sigma[a], reach_other);
            value += sigma[a] * action_value[a];
        }
        for (std::int32_t a = 0; a < node.child_count; ++a) {
            const auto i = static_cast<std::size_t>(infoset.offset + a);
            // Counterfactual regret: weighted by how likely the OTHER side makes this spot.
            regret_[i] += reach_other * (action_value[a] - value);
            strategy_sum_[i] += weight_ * reach_me * sigma[a];
        }
        return value;
    }

    // External sampling: one sampled deal and one sampled opponent line, but
    // every one of my own actions. Cheap iterations, noisy regrets.
    double sample(std::int32_t index, int me) {
        ++touched_;
        const Node& node = game_.nodes[static_cast<std::size_t>(index)];
        if (node.type == NodeType::Terminal) return me == 0 ? node.utility : -node.utility;
        if (node.type == NodeType::Chance) {
            double roll = rng_.uniform();
            std::int32_t pick = node.child_count - 1;
            for (std::int32_t a = 0; a < node.child_count; ++a) {
                roll -= game_.probability(node, a);
                if (roll < 0) {
                    pick = a;
                    break;
                }
            }
            return sample(game_.child(node, pick), me);
        }
        const Infoset& infoset = game_.infosets[static_cast<std::size_t>(node.infoset)];
        double sigma[4];
        match(infoset, sigma);
        if (node.player != me) {
            for (std::int32_t a = 0; a < node.child_count; ++a)
                strategy_sum_[static_cast<std::size_t>(infoset.offset + a)] += sigma[a];
            double roll = rng_.uniform();
            std::int32_t pick = node.child_count - 1;
            for (std::int32_t a = 0; a < node.child_count; ++a) {
                roll -= sigma[a];
                if (roll < 0) {
                    pick = a;
                    break;
                }
            }
            return sample(game_.child(node, pick), me);
        }
        double action_value[4];
        double value = 0;
        for (std::int32_t a = 0; a < node.child_count; ++a) {
            action_value[a] = sample(game_.child(node, a), me);
            value += sigma[a] * action_value[a];
        }
        for (std::int32_t a = 0; a < node.child_count; ++a)
            regret_[static_cast<std::size_t>(infoset.offset + a)] += action_value[a] - value;
        return value;
    }

    const Game& game_;
    Algorithm algorithm_;
    Rng rng_;
    std::vector<double> regret_;
    std::vector<double> strategy_sum_;
    Strategy current_;
    double weight_{1.0};
    std::uint64_t iteration_{};
    std::uint64_t touched_{};
};

} // namespace zerosum
