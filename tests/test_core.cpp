// Tests for the game builder, the solvers, and the exploitation analysis.
// No third-party test framework is required.

#include "zerosum/analysis.hpp"
#include "zerosum/cfr.hpp"
#include "zerosum/game.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {

using namespace zerosum;

std::size_t assertions = 0;
std::size_t cases = 0;

void check(bool condition, const char* expression, int line) {
    ++assertions;
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

template <typename F>
void test(const char* name, F&& body) {
    try {
        body();
        ++cases;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(name) + ": " + error.what());
    }
}

template <typename F>
bool throws(F&& body) {
    try {
        body();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }

// The value of Kuhn poker to the first player, from Kuhn (1950).
constexpr double kKuhnValue = -1.0 / 18.0;
// The value of Leduc hold'em to the first player. This constant comes from the
// linear program in scripts/lp_oracle.py, not from any solver in this repo.
constexpr double kLeducValue = -0.08560642407;

double& at(const Game& game, Strategy& strategy, const std::string& key, char action) {
    for (const auto& infoset : game.infosets)
        if (infoset.key == key)
            return strategy[static_cast<std::size_t>(infoset.offset) + infoset.actions.find(action)];
    throw std::runtime_error("no infoset " + key);
}

double get(const Game& game, const Strategy& strategy, const std::string& key, char action) {
    Strategy copy = strategy;
    return at(game, copy, key, action);
}

void set(const Game& game, Strategy& strategy, const std::string& key, char action, double p) {
    // Two-action infosets only: the other action takes the remainder.
    for (const auto& infoset : game.infosets) {
        if (infoset.key != key) continue;
        if (infoset.action_count != 2) throw std::runtime_error("set() needs two actions");
        const auto chosen = infoset.actions.find(action);
        strategy[static_cast<std::size_t>(infoset.offset) + chosen] = p;
        strategy[static_cast<std::size_t>(infoset.offset) + 1 - chosen] = 1.0 - p;
        return;
    }
    throw std::runtime_error("no infoset " + key);
}

// Every equilibrium of Kuhn poker, indexed by how often the first player
// bluffs with the Jack (alpha, anywhere from 0 to 1/3).
Strategy kuhn_equilibrium(const Game& game, double alpha) {
    Strategy s = uniform_strategy(game);
    set(game, s, "0:J:-:", 'b', alpha);                  // Bluff the worst card sometimes,
    set(game, s, "0:Q:-:", 'b', 0.0);                    // never bet the middle card,
    set(game, s, "0:K:-:", 'b', 3 * alpha);              // and value-bet three times as often as bluffing.
    set(game, s, "0:J:-:kb", 'c', 0.0);
    set(game, s, "0:Q:-:kb", 'c', alpha + 1.0 / 3.0);
    set(game, s, "0:K:-:kb", 'c', 1.0);
    set(game, s, "1:J:-:k", 'b', 1.0 / 3.0);
    set(game, s, "1:Q:-:k", 'b', 0.0);
    set(game, s, "1:K:-:k", 'b', 1.0);
    set(game, s, "1:J:-:b", 'c', 0.0);
    set(game, s, "1:Q:-:b", 'c', 1.0 / 3.0);
    set(game, s, "1:K:-:b", 'c', 1.0);
    return s;
}

Strategy random_strategy(const Game& game, Rng& rng) {
    Strategy s(static_cast<std::size_t>(game.action_slots));
    for (const auto& infoset : game.infosets) {
        double total = 0;
        for (std::int32_t a = 0; a < infoset.action_count; ++a) {
            const double weight = -std::log(1.0 - rng.uniform()) + 1e-9;
            s[static_cast<std::size_t>(infoset.offset + a)] = weight;
            total += weight;
        }
        for (std::int32_t a = 0; a < infoset.action_count; ++a)
            s[static_cast<std::size_t>(infoset.offset + a)] /= total;
    }
    return s;
}

Strategy solve(const Game& game, Algorithm algorithm, std::uint64_t iterations, std::uint64_t seed = 1) {
    Solver solver(game, algorithm, seed);
    for (std::uint64_t i = 0; i < iterations; ++i) solver.iterate();
    return solver.average_strategy();
}

double gap_ma(const Game& game, const Strategy& strategy) {
    return 1000 * exploitability(game, strategy).exploitability();
}

double profit(const Game& game, const Strategy& mine, const Strategy& opponent) {
    return 0.5 * (expected_value(game, with_player(game, opponent, mine, 0)) -
                  expected_value(game, with_player(game, mine, opponent, 0)));
}

// ---------------------------------------------------------------------------

void game_tests() {
    const Game kuhn = make_game("kuhn");
    const Game leduc = make_game("leduc");

    test("Kuhn poker has the textbook tree", [&] {
        CHECK(kuhn.nodes.size() == 55);
        CHECK(kuhn.infosets.size() == 12);
        CHECK(kuhn.action_slots == 24);
        std::size_t terminals = 0;
        for (const auto& node : kuhn.nodes)
            if (node.type == NodeType::Terminal) {
                ++terminals;
                CHECK(std::abs(node.utility) == 1 || std::abs(node.utility) == 2);
            }
        CHECK(terminals == 30);
        const Node& root = kuhn.nodes[0];
        CHECK(root.type == NodeType::Chance && root.child_count == 6);
        for (std::int32_t a = 0; a < 6; ++a) CHECK(near(kuhn.probability(root, a), 1.0 / 6.0, 1e-15));
        CHECK(kuhn.rank_names == std::vector<std::string>({"J", "Q", "K"}));
    });

    test("Kuhn payoffs match a hand-written enumeration", [&] {
        // Always check, always call when bet into: only showdowns for one ante.
        // The first player wins three of the six deals, so the value is zero.
        Strategy passive(static_cast<std::size_t>(kuhn.action_slots), 0.0);
        for (const auto& infoset : kuhn.infosets)
            passive[static_cast<std::size_t>(infoset.offset) + infoset.actions.find(infoset.actions[0] == 'k' ? 'k' : 'c')] = 1;
        CHECK(near(expected_value(kuhn, passive), 0.0, 1e-15));
        // Both always bet or call: every hand is a showdown for two.
        Strategy aggressive(static_cast<std::size_t>(kuhn.action_slots), 0.0);
        for (const auto& infoset : kuhn.infosets)
            aggressive[static_cast<std::size_t>(infoset.offset) + infoset.actions.find(infoset.actions[0] == 'k' ? 'b' : 'c')] = 1;
        CHECK(near(expected_value(kuhn, aggressive), 0.0, 1e-15));
        // First player always bets, second always folds: first wins one ante.
        Strategy steal = aggressive;
        for (const auto& infoset : kuhn.infosets)
            if (infoset.player == 1 && infoset.actions == "fc") {
                steal[static_cast<std::size_t>(infoset.offset)] = 1;
                steal[static_cast<std::size_t>(infoset.offset) + 1] = 0;
            }
        CHECK(near(expected_value(kuhn, steal), 1.0, 1e-15));
        // Uniform play, worked out by hand over the 6 deals and 5 betting lines:
        // the first player averages +1/8 ante per hand.
        CHECK(near(expected_value(kuhn, uniform_strategy(kuhn)), 0.125, 1e-15));
    });

    test("Leduc hold'em has 288 information sets", [&] {
        CHECK(leduc.infosets.size() == 288);
        CHECK(leduc.nodes.size() == 1936);
        std::size_t first = 0;
        for (const auto& infoset : leduc.infosets) first += infoset.player == 0;
        CHECK(first == 144);
        // 1 ante + two bets of 2 + two bets of 4 is the most one player can lose.
        for (const auto& node : leduc.nodes)
            if (node.type == NodeType::Terminal) CHECK(std::abs(node.utility) <= 13 && node.utility == std::floor(node.utility));
        CHECK(leduc.rank_names == std::vector<std::string>({"J", "Q", "K"}));
    });

    test("chance probabilities sum to one everywhere", [&] {
        for (const char* name : {"kuhn", "leduc", "leduc5", "leduc13"}) {
            const Game game = make_game(name);
            for (const auto& node : game.nodes) {
                if (node.type != NodeType::Chance) continue;
                double total = 0;
                for (std::int32_t a = 0; a < node.child_count; ++a) total += game.probability(node, a);
                CHECK(near(total, 1.0, 1e-12));
            }
        }
    });

    test("a pair beats a high card, and equal hands split", [&] {
        // Find showdowns after check-check, reveal, check-check and compare by key.
        std::map<std::string, double> seen;
        std::function<void(std::int32_t, std::string, std::string)> walk =
            [&](std::int32_t index, std::string cards, std::string line) {
                const Node& node = leduc.nodes[static_cast<std::size_t>(index)];
                if (node.type == NodeType::Terminal) {
                    if (line == "kk/kk") seen[cards] = node.utility;
                    return;
                }
                if (node.type == NodeType::Chance) {
                    for (std::int32_t a = 0; a < node.child_count; ++a) walk(leduc.child(node, a), cards, line);
                    return;
                }
                const auto& infoset = leduc.infosets[static_cast<std::size_t>(node.infoset)];
                // Record each player's card the first time they act, and the public card on the turn.
                std::string next = cards;
                if (infoset.history.empty() || infoset.history == "k")
                    if (next.size() == static_cast<std::size_t>(infoset.player))
                        next += leduc.rank_names[static_cast<std::size_t>(infoset.private_rank)];
                if (infoset.public_rank >= 0 && next.size() == 2)
                    next += leduc.rank_names[static_cast<std::size_t>(infoset.public_rank)];
                for (std::int32_t a = 0; a < node.child_count; ++a)
                    walk(leduc.child(node, a), next, infoset.history + infoset.actions[static_cast<std::size_t>(a)]);
            };
        walk(0, "", "");
        CHECK(seen.at("JKJ") == 1);   // First player pairs the board: beats a King.
        CHECK(seen.at("KJJ") == -1);  // Second player pairs.
        CHECK(seen.at("KQJ") == 1);   // No pair: high card wins.
        CHECK(seen.at("QKJ") == -1);
        CHECK(seen.at("QQK") == 0);   // Same rank: split.
        CHECK(seen.count("JJJ") == 0); // Only two Jacks in the deck.
    });

    test("every infoset is consistent and every player has perfect recall", [&] {
        for (const char* name : {"kuhn", "leduc", "leduc6"}) {
            const Game game = make_game(name);
            // The sequence of (infoset, action) pairs the acting player has taken
            // must be the same at every node of an infoset.
            std::map<std::int32_t, std::vector<std::int32_t>> memory;
            std::int32_t offset = 0;
            for (const auto& infoset : game.infosets) {
                CHECK(infoset.offset == offset);
                CHECK(infoset.action_count == static_cast<std::int32_t>(infoset.actions.size()));
                CHECK(infoset.action_count >= 2 && infoset.action_count <= 3);
                offset += infoset.action_count;
            }
            CHECK(offset == game.action_slots);
            std::size_t visited = 0;
            std::function<void(std::int32_t, std::vector<std::int32_t>, std::vector<std::int32_t>)> walk =
                [&](std::int32_t index, std::vector<std::int32_t> own0, std::vector<std::int32_t> own1) {
                    ++visited;
                    const Node& node = game.nodes[static_cast<std::size_t>(index)];
                    if (node.type == NodeType::Decision) {
                        const auto& infoset = game.infosets[static_cast<std::size_t>(node.infoset)];
                        CHECK(infoset.player == node.player);
                        CHECK(infoset.action_count == node.child_count);
                        auto& own = node.player == 0 ? own0 : own1;
                        const auto [found, fresh] = memory.emplace(node.infoset, own);
                        if (!fresh) CHECK(found->second == own);
                        for (std::int32_t a = 0; a < node.child_count; ++a) {
                            auto next0 = own0;
                            auto next1 = own1;
                            (node.player == 0 ? next0 : next1).push_back(infoset.offset + a);
                            walk(game.child(node, a), next0, next1);
                        }
                        return;
                    }
                    for (std::int32_t a = 0; a < node.child_count; ++a) walk(game.child(node, a), own0, own1);
                };
            walk(0, {}, {});
            CHECK(visited == game.nodes.size()); // A tree: every node reached exactly once.
            CHECK(memory.size() == game.infosets.size());
        }
    });

    test("larger decks scale the game, and bad names are rejected", [&] {
        CHECK(make_game("leduc6").infosets.size() == 1116);
        CHECK(make_game("leduc13").infosets.size() == 5148);
        CHECK(make_game("leduc13").rank_names.back() == "A");
        CHECK(throws([] { make_game("holdem"); }));
        CHECK(throws([] { make_game("leduc1"); }));
        CHECK(throws([] { make_game("leduc99"); }));
        CHECK(throws([] { parse_algorithm("simplex"); }));
        CHECK(parse_algorithm("cfr+") == Algorithm::CfrPlus);
    });
}

void analysis_tests() {
    const Game kuhn = make_game("kuhn");
    const Game leduc = make_game("leduc");

    test("every known Kuhn equilibrium has zero exploitability", [&] {
        for (const double alpha : {0.0, 0.1, 1.0 / 6.0, 0.25, 1.0 / 3.0}) {
            const auto e = exploitability(kuhn, kuhn_equilibrium(kuhn, alpha));
            CHECK(near(e.value, kKuhnValue, 1e-15));
            CHECK(near(e.exploitability(), 0.0, 1e-15));
            CHECK(near(e.value_lower(), kKuhnValue, 1e-15) && near(e.value_upper(), kKuhnValue, 1e-15));
        }
    });

    test("leaving the equilibrium family is punished by the amount theory predicts", [&] {
        // Bluffing the Jack more than a third of the time is no longer covered
        // by enough value bets with the King.
        auto greedy = kuhn_equilibrium(kuhn, 1.0 / 3.0);
        set(kuhn, greedy, "0:J:-:", 'b', 1.0);
        const auto e = exploitability(kuhn, greedy);
        CHECK(e.best_vs_p0 > -kKuhnValue + 0.01);
        CHECK(near(e.best_vs_p1, kKuhnValue, 1e-15)); // The second player's part is unchanged.
        // The second player's best reply: call every bet with the Queen.
        Strategy reply;
        best_response_value(kuhn, greedy, 1, &reply);
        CHECK(get(kuhn, reply, "1:Q:-:b", 'c') == 1.0);
        CHECK(get(kuhn, reply, "1:J:-:b", 'f') == 1.0);
        CHECK(get(kuhn, reply, "1:K:-:b", 'c') == 1.0);
    });

    test("a best response is a pure strategy that achieves its stated value", [&] {
        Rng rng(11);
        for (int trial = 0; trial < 40; ++trial) {
            const Game& game = trial % 2 ? leduc : kuhn;
            const Strategy s = random_strategy(game, rng);
            for (int me = 0; me < 2; ++me) {
                Strategy reply;
                const double value = best_response_value(game, s, me, &reply);
                const double played = expected_value(game, reply);
                CHECK(near(value, me == 0 ? played : -played, 1e-12));
                // No strategy does better: in particular not the original.
                const double original = expected_value(game, s);
                CHECK(value >= (me == 0 ? original : -original) - 1e-12);
                // And not another random one.
                const Strategy other = with_player(game, s, random_strategy(game, rng), me);
                const double alternative = expected_value(game, other);
                CHECK(value >= (me == 0 ? alternative : -alternative) - 1e-12);
            }
            const auto e = exploitability(game, s);
            CHECK(e.exploitability() >= 0);
            CHECK(e.value_lower() <= e.value_upper());
            CHECK(e.value_lower() <= (game.name == "kuhn" ? kKuhnValue : kLeducValue) + 1e-9);
            CHECK(e.value_upper() >= (game.name == "kuhn" ? kKuhnValue : kLeducValue) - 1e-9);
        }
    });

    test("mixing two strategies before the hand mixes their payoffs exactly", [&] {
        // Kuhn's theorem: the coin flip has an equivalent behavior strategy.
        Rng rng(5);
        for (int trial = 0; trial < 30; ++trial) {
            const Game& game = trial % 2 ? leduc : kuhn;
            const Strategy a = random_strategy(game, rng);
            const Strategy b = random_strategy(game, rng);
            const Strategy opponent = random_strategy(game, rng);
            const double share = rng.uniform();
            for (int who = 0; who < 2; ++who) {
                const Strategy mixed = mix(game, a, b, share, who);
                const double va = expected_value(game, with_player(game, opponent, a, who));
                const double vb = expected_value(game, with_player(game, opponent, b, who));
                const double vm = expected_value(game, with_player(game, opponent, mixed, who));
                CHECK(near(vm, (1 - share) * va + share * vb, 1e-12));
                for (const auto& infoset : game.infosets) {
                    double total = 0;
                    for (std::int32_t k = 0; k < infoset.action_count; ++k)
                        total += mixed[static_cast<std::size_t>(infoset.offset + k)];
                    CHECK(near(total, 1.0, 1e-12));
                }
            }
            const Strategy none = mix(game, a, b, 0.0, 0);
            const Strategy all = mix(game, a, b, 1.0, 0);
            for (const auto& infoset : game.infosets)
                for (std::int32_t k = 0; k < infoset.action_count; ++k) {
                    const auto i = static_cast<std::size_t>(infoset.offset + k);
                    CHECK(near(none[i], a[i], 1e-12));
                    CHECK(near(all[i], infoset.player == 0 ? b[i] : a[i], 1e-12));
                }
        }
    });

    test("opponent types are valid strategies with the leaks their names promise", [&] {
        for (const char* name : {"kuhn", "leduc"}) {
            const Game game = make_game(name);
            for (const char* kind : {"random", "station", "maniac", "rock"}) {
                const Strategy s = archetype(game, kind);
                for (const auto& infoset : game.infosets) {
                    double total = 0;
                    for (std::int32_t a = 0; a < infoset.action_count; ++a)
                        total += s[static_cast<std::size_t>(infoset.offset + a)];
                    CHECK(near(total, 1.0, 1e-12));
                }
                CHECK(exploitability(game, s).exploitability() > 0.1);
            }
            const Strategy station = archetype(game, "station");
            const Strategy maniac = archetype(game, "maniac");
            const Strategy rock = archetype(game, "rock");
            const int top = static_cast<int>(game.rank_names.size()) - 1;
            for (const auto& infoset : game.infosets) {
                const auto probability = [&](const Strategy& s, char action) {
                    const auto position = infoset.actions.find(action);
                    return position == std::string::npos ? 0.0 : s[static_cast<std::size_t>(infoset.offset) + position];
                };
                CHECK(probability(station, 'f') == 0 && probability(station, 'b') == 0 && probability(station, 'r') == 0);
                CHECK(probability(maniac, 'f') == 0 && probability(maniac, 'k') == (infoset.actions == "k" ? 1 : 0));
                const bool strong = infoset.private_rank == top ||
                    (infoset.public_rank >= 0 && infoset.public_rank == infoset.private_rank);
                if (!strong) CHECK(probability(rock, 'b') == 0 && probability(rock, 'r') == 0 && probability(rock, 'c') == 0);
                else CHECK(probability(rock, 'f') == 0);
            }
            CHECK(throws([&] { archetype(game, "shark"); }));
        }
    });
}

void solver_tests() {
    const Game kuhn = make_game("kuhn");
    const Game leduc = make_game("leduc");

    test("every full-traversal algorithm solves Kuhn poker", [&] {
        struct Budget { Algorithm algorithm; std::uint64_t iterations; double ma; };
        for (const auto& b : {Budget{Algorithm::Cfr, 20000, 2.0}, Budget{Algorithm::Linear, 5000, 0.5},
                              Budget{Algorithm::Dcfr, 5000, 0.1}, Budget{Algorithm::CfrPlus, 5000, 0.1}}) {
            const Strategy s = solve(kuhn, b.algorithm, b.iterations);
            const auto e = exploitability(kuhn, s);
            CHECK(1000 * e.exploitability() < b.ma);
            CHECK(e.value_lower() <= kKuhnValue + 1e-12 && kKuhnValue <= e.value_upper() + 1e-12);
            CHECK(near(e.value, kKuhnValue, 2 * e.exploitability() + 1e-12));
        }
    });

    test("the solved Kuhn strategy is one of Kuhn's equilibria", [&] {
        const Strategy s = solve(kuhn, Algorithm::CfrPlus, 100000);
        CHECK(gap_ma(kuhn, s) < 0.01);
        const double alpha = get(kuhn, s, "0:J:-:", 'b');
        CHECK(alpha > -1e-9 && alpha < 1.0 / 3.0 + 1e-3);
        CHECK(near(get(kuhn, s, "0:K:-:", 'b'), 3 * alpha, 5e-3));
        CHECK(near(get(kuhn, s, "0:Q:-:", 'b'), 0.0, 1e-3));
        CHECK(near(get(kuhn, s, "0:Q:-:kb", 'c'), alpha + 1.0 / 3.0, 5e-3));
        // The second player's equilibrium strategy is unique.
        CHECK(near(get(kuhn, s, "1:J:-:k", 'b'), 1.0 / 3.0, 2e-3));
        CHECK(near(get(kuhn, s, "1:Q:-:b", 'c'), 1.0 / 3.0, 2e-3));
        CHECK(near(get(kuhn, s, "1:K:-:b", 'c'), 1.0, 1e-3));
        CHECK(near(get(kuhn, s, "1:J:-:b", 'f'), 1.0, 1e-3));
    });

    test("every full-traversal algorithm solves Leduc hold'em", [&] {
        struct Budget { Algorithm algorithm; std::uint64_t iterations; double ma; };
        for (const auto& b : {Budget{Algorithm::Cfr, 2000, 40.0}, Budget{Algorithm::Linear, 2000, 8.0},
                              Budget{Algorithm::Dcfr, 2000, 0.2}, Budget{Algorithm::CfrPlus, 2000, 0.3}}) {
            const Strategy s = solve(leduc, b.algorithm, b.iterations);
            const auto e = exploitability(leduc, s);
            CHECK(1000 * e.exploitability() < b.ma);
            CHECK(e.value_lower() <= kLeducValue && kLeducValue <= e.value_upper());
        }
    });

    test("more iterations give a tighter bracket around the true value", [&] {
        Solver solver(leduc, Algorithm::CfrPlus);
        double previous = 1e9;
        for (const std::uint64_t stop : {10u, 100u, 1000u, 5000u}) {
            while (solver.iteration() < stop) solver.iterate();
            const auto e = exploitability(leduc, solver.average_strategy());
            CHECK(e.exploitability() < previous);
            CHECK(e.value_lower() <= kLeducValue && kLeducValue <= e.value_upper());
            previous = e.exploitability();
        }
        CHECK(1000 * previous < 0.05);
        CHECK(solver.nodes_touched() > 5000u * leduc.nodes.size()); // Two walks per iteration, some pruned.
    });

    test("the newer algorithms need far less work than the original", [&] {
        const auto work = [&](Algorithm algorithm) {
            Solver solver(leduc, algorithm);
            while (gap_ma(leduc, solver.average_strategy()) > 5.0) {
                for (int i = 0; i < 20; ++i) solver.iterate();
                if (solver.iteration() > 100000) throw std::runtime_error("did not converge");
            }
            return solver.nodes_touched();
        };
        const auto plain = work(Algorithm::Cfr);
        CHECK(work(Algorithm::CfrPlus) * 10 < plain);
        CHECK(work(Algorithm::Dcfr) * 10 < plain);
        CHECK(work(Algorithm::Linear) * 2 < plain);
    });

    test("sampled CFR converges and is reproducible from its seed", [&] {
        const Strategy a = solve(kuhn, Algorithm::Mccfr, 200000, 7);
        const Strategy b = solve(kuhn, Algorithm::Mccfr, 200000, 7);
        const Strategy c = solve(kuhn, Algorithm::Mccfr, 200000, 8);
        CHECK(a == b);
        CHECK(a != c);
        CHECK(gap_ma(kuhn, a) < 5.0 && gap_ma(kuhn, c) < 5.0);
        const Strategy early = solve(leduc, Algorithm::Mccfr, 2000, 3);
        const Strategy late = solve(leduc, Algorithm::Mccfr, 400000, 3);
        CHECK(gap_ma(leduc, late) < 40.0);
        CHECK(gap_ma(leduc, late) * 5 < gap_ma(leduc, early));
        // One sampled iteration touches a small fraction of the tree.
        Solver solver(leduc, Algorithm::Mccfr, 3);
        for (int i = 0; i < 1000; ++i) solver.iterate();
        CHECK(solver.nodes_touched() < 1000 * leduc.nodes.size() / 10);
    });

    test("full-traversal solvers are deterministic", [&] {
        for (const auto algorithm : {Algorithm::Cfr, Algorithm::CfrPlus, Algorithm::Linear, Algorithm::Dcfr})
            CHECK(solve(leduc, algorithm, 50, 1) == solve(leduc, algorithm, 50, 99));
    });

    test("solving scales to a 13-rank deck", [&] {
        const Game big = make_game("leduc13");
        const Strategy s = solve(big, Algorithm::CfrPlus, 300);
        CHECK(gap_ma(big, s) < 10.0);
    });
}

void exploitation_tests() {
    const Game leduc = make_game("leduc");
    const Game kuhn = make_game("kuhn");
    const Strategy nash = solve(leduc, Algorithm::CfrPlus, 5000);
    const double nash_gap = exploitability(leduc, nash).exploitability();

    test("equilibrium play never loses, but leaves most of the profit behind", [&] {
        for (const char* kind : {"random", "station", "maniac", "rock"}) {
            const Strategy opponent = archetype(leduc, kind);
            const double earned = profit(leduc, nash, opponent);
            Strategy seat[2];
            double most = 0;
            for (int me = 0; me < 2; ++me) most += 0.5 * best_response_value(leduc, opponent, me, &seat[me]);
            CHECK(earned > 0.05);                     // Every one of these opponents pays.
            CHECK(earned >= -nash_gap);               // The guarantee, to within solver error.
            CHECK(earned < 0.5 * most);               // But less than half of what is there.
            // The maximum profit equals the opponent's exploitability.
            CHECK(near(most, exploitability(leduc, opponent).exploitability(), 1e-12));
            CHECK(near(profit(leduc, combine(leduc, seat[0], seat[1]), opponent), most, 1e-12));
        }
    });

    test("the restricted game has the documented shape", [&] {
        const Strategy model = archetype(kuhn, "rock");
        const Game restricted = restricted_game(kuhn, model, 0.25, 0);
        CHECK(restricted.nodes.size() == 2 * kuhn.nodes.size() + 1);
        CHECK(restricted.infosets.size() == kuhn.infosets.size());
        CHECK(restricted.action_slots == kuhn.action_slots);
        CHECK(near(restricted.probability(restricted.nodes[0], 0), 0.25, 0));
        std::size_t opponent_decisions = 0, original = 0;
        for (const auto& node : restricted.nodes) opponent_decisions += node.type == NodeType::Decision && node.player == 1;
        for (const auto& node : kuhn.nodes) original += node.type == NodeType::Decision && node.player == 1;
        CHECK(opponent_decisions == original); // Only the free copy still asks the opponent to choose.
        for (const auto& node : restricted.nodes) {
            if (node.type != NodeType::Chance) continue;
            double total = 0;
            for (std::int32_t a = 0; a < node.child_count; ++a) total += restricted.probability(node, a);
            CHECK(near(total, 1.0, 1e-12));
        }
        CHECK(throws([&] { restricted_game(kuhn, model, 1.5, 0); }));
        // Any strategy is worth the weighted average of its two matchups.
        Rng rng(3);
        for (int trial = 0; trial < 10; ++trial) {
            const Strategy s = random_strategy(kuhn, rng);
            const double versus_model = expected_value(kuhn, with_player(kuhn, model, s, 0));
            CHECK(near(expected_value(restricted, s), 0.25 * versus_model + 0.75 * expected_value(kuhn, s), 1e-12));
        }
    });

    test("restricted game: no confidence gives the equilibrium, full confidence the best response", [&] {
        const Strategy model = archetype(leduc, "rock");
        for (int me = 0; me < 2; ++me) {
            const double sign = me == 0 ? 1.0 : -1.0;
            const Game cautious = restricted_game(leduc, model, 0.0, me);
            const auto e0 = exploitability(cautious, solve(cautious, Algorithm::CfrPlus, 2000));
            CHECK(near(e0.value, kLeducValue, 1e-3));
            const Game trusting = restricted_game(leduc, model, 1.0, me);
            const auto e1 = exploitability(trusting, solve(trusting, Algorithm::CfrPlus, 2000));
            CHECK(near(sign * e1.value, best_response_value(leduc, model, me), 1e-3));
        }
    });

    test("restricted Nash response beats coin-flip mixing at equal risk", [&] {
        const Strategy model = archetype(leduc, "rock");
        Strategy counter_seat[2];
        for (int me = 0; me < 2; ++me)
            best_response_value(leduc, with_player(leduc, model, nash, me), me, &counter_seat[me]);
        const Strategy counter = combine(leduc, counter_seat[0], counter_seat[1]);

        double last_profit = -1, last_risk = -1;
        for (const double confidence : {0.0, 0.2, 0.5, 0.8}) {
            Strategy seat[2];
            for (int me = 0; me < 2; ++me) {
                const Game restricted = restricted_game(leduc, model, confidence, me);
                seat[me] = solve(restricted, Algorithm::CfrPlus, 3000);
            }
            const Strategy response = combine(leduc, seat[0], seat[1]);
            const double earned = profit(leduc, response, model);
            const double risk = exploitability(leduc, response).exploitability();
            // More confidence in the model: more profit, more exposure.
            CHECK(earned > last_profit - 1e-3 && risk > last_risk - 1e-3);
            last_profit = earned;
            last_risk = risk;
            if (confidence == 0.0) continue;
            // The coin-flip mixture that earns the same profit is linear in its
            // share, so that share can be solved for directly.
            const double base = profit(leduc, nash, model);
            const double top = profit(leduc, counter, model);
            const double share = (earned - base) / (top - base);
            CHECK(share > 0 && share < 1);
            const Strategy mixed = combine(leduc, mix(leduc, nash, counter, share, 0), mix(leduc, nash, counter, share, 1));
            CHECK(near(profit(leduc, mixed, model), earned, 1e-9));
            CHECK(risk < 0.6 * exploitability(leduc, mixed).exploitability());
        }
        CHECK(last_profit > 0.85 * profit(leduc, counter, model));
    });
}

} // namespace

int main() {
    try {
        game_tests();
        analysis_tests();
        solver_tests();
        exploitation_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED " << error.what() << '\n';
        return 1;
    }
    std::cout << "core tests passed: " << cases << " cases, " << assertions << " assertions\n";
    return 0;
}
