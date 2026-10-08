// zerosum: solve small poker games, measure how far a strategy is from
// equilibrium, and quantify what it costs to exploit a weak opponent.

#include "zerosum/analysis.hpp"
#include "zerosum/cfr.hpp"
#include "zerosum/game.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

using namespace zerosum;

void usage() {
    std::cout <<
        "Usage: zerosum COMMAND GAME [options]      GAME: kuhn, leduc, leduc5 ... leduc13\n"
        "  info GAME                        tree size and information-set count\n"
        "  solve GAME [--algo cfr+] [--iterations 1000] [--every 0 | --growing] [--seed 1]\n"
        "             [--save FILE] [--csv]  run a solver; report exploitability\n"
        "  eval GAME --strategy FILE        exploitability and value of a saved strategy\n"
        "  show GAME --strategy FILE        print the strategy, one information set per line\n"
        "  exploit GAME --strategy FILE [--csv]   equilibrium vs four weak opponents\n"
        "  frontier GAME --strategy FILE --opponent rock [--steps 20] [--iterations 1000]\n"
        "                                   profit against risk as play tilts toward the exploit,\n"
        "                                   by coin-flip mixing and by restricted Nash response\n"
        "  export GAME                      the game tree as JSON, for the linear-program check\n"
        "  bench GAME [--target 1.0] [--max-nodes 4e9] [--trials 3]\n"
        "                                   work each algorithm needs to reach a target accuracy\n"
        "  play GAME --strategy FILE [--seat 0] [--seed 1]   play against the strategy\n"
        "Algorithms: cfr, cfr+, lcfr, dcfr, mccfr. Exploitability is in milli-antes per hand.\n";
}

struct Args {
    std::string command;
    std::string game;
    std::string algo{"cfr+"};
    std::string strategy;
    std::string save;
    std::string opponent{"rock"};
    std::uint64_t iterations{1000};
    std::uint64_t every{0};
    std::uint64_t seed{1};
    std::uint64_t max_nodes{4'000'000'000};
    double target{1.0};
    int trials{3};
    int steps{20};
    int seat{0};
    bool csv{false};
    bool growing{false};
};

Args parse(int argc, char** argv) {
    Args args;
    if (argc < 2) throw std::invalid_argument("missing command");
    args.command = argv[1];
    if (args.command == "--help" || args.command == "help") return args;
    if (argc < 3) throw std::invalid_argument("missing game");
    args.game = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string name = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + name);
            return argv[++i];
        };
        if (name == "--algo") args.algo = value();
        else if (name == "--strategy") args.strategy = value();
        else if (name == "--save") args.save = value();
        else if (name == "--opponent") args.opponent = value();
        else if (name == "--iterations") args.iterations = std::stoull(value());
        else if (name == "--every") args.every = std::stoull(value());
        else if (name == "--seed") args.seed = std::stoull(value());
        else if (name == "--max-nodes") args.max_nodes = static_cast<std::uint64_t>(std::stod(value()));
        else if (name == "--target") args.target = std::stod(value());
        else if (name == "--trials") args.trials = std::stoi(value());
        else if (name == "--steps") args.steps = std::stoi(value());
        else if (name == "--seat") args.seat = std::stoi(value());
        else if (name == "--csv") args.csv = true;
        else if (name == "--growing") args.growing = true;
        else throw std::invalid_argument("unknown option: " + name);
    }
    return args;
}

void save_strategy(const Game& game, const Strategy& strategy, const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << "# zerosum strategy for " << game.name << "\n# infoset action probability\n";
    out << std::setprecision(17);
    for (const auto& infoset : game.infosets)
        for (std::int32_t a = 0; a < infoset.action_count; ++a)
            out << infoset.key << ' ' << infoset.actions[static_cast<std::size_t>(a)] << ' '
                << strategy[static_cast<std::size_t>(infoset.offset + a)] << '\n';
}

Strategy load_strategy(const Game& game, const std::string& path) {
    if (path.empty()) throw std::invalid_argument("--strategy FILE is required");
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::map<std::string, const Infoset*> by_key;
    for (const auto& infoset : game.infosets) by_key[infoset.key] = &infoset;
    Strategy strategy(static_cast<std::size_t>(game.action_slots), -1.0);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string key;
        char action = 0;
        double probability = 0;
        if (!(row >> key >> action >> probability)) throw std::runtime_error("bad strategy line: " + line);
        const auto found = by_key.find(key);
        if (found == by_key.end()) throw std::runtime_error("strategy is for a different game: " + key);
        const auto position = found->second->actions.find(action);
        if (position == std::string::npos || probability < -1e-9 || probability > 1 + 1e-9)
            throw std::runtime_error("bad strategy line: " + line);
        strategy[static_cast<std::size_t>(found->second->offset) + position] = probability;
    }
    for (const auto& infoset : game.infosets) {
        double total = 0;
        for (std::int32_t a = 0; a < infoset.action_count; ++a) {
            const double p = strategy[static_cast<std::size_t>(infoset.offset + a)];
            if (p < 0) throw std::runtime_error("strategy is missing " + infoset.key);
            total += p;
        }
        if (std::abs(total - 1.0) > 1e-6)
            throw std::runtime_error("probabilities at " + infoset.key + " do not sum to 1");
    }
    return strategy;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

int cmd_info(const Game& game) {
    std::size_t chance = 0, decision = 0, terminal = 0;
    for (const auto& node : game.nodes) {
        if (node.type == NodeType::Chance) ++chance;
        else if (node.type == NodeType::Decision) ++decision;
        else ++terminal;
    }
    std::size_t first = 0;
    for (const auto& infoset : game.infosets)
        if (infoset.player == 0) ++first;
    std::cout << "game " << game.name << '\n'
        << "nodes " << game.nodes.size() << " (chance " << chance << ", decision " << decision
        << ", terminal " << terminal << ")\n"
        << "information_sets " << game.infosets.size() << " (player 0: " << first
        << ", player 1: " << game.infosets.size() - first << ")\n"
        << "actions " << game.action_slots << '\n';
    return 0;
}

void report(const Game& game, const Solver& solver, double seconds, bool csv) {
    const auto e = exploitability(game, solver.average_strategy());
    if (csv) {
        std::cout << game.name << ',' << algorithm_name(solver.algorithm()) << ',' << solver.iteration()
            << ',' << solver.nodes_touched() << ',' << std::setprecision(6) << seconds << ','
            << std::setprecision(10) << 1000 * e.exploitability() << ',' << e.value_lower() << ','
            << e.value_upper() << '\n';
    } else {
        std::cout << "iteration " << std::setw(8) << solver.iteration()
            << "  exploitability " << std::fixed << std::setprecision(4) << std::setw(10)
            << 1000 * e.exploitability() << " mA/hand"
            << "  value in [" << std::setprecision(6) << e.value_lower() << ", " << e.value_upper()
            << "]  nodes " << solver.nodes_touched() << '\n' << std::defaultfloat;
    }
}

int cmd_solve(const Game& game, const Args& args) {
    Solver solver(game, parse_algorithm(args.algo), args.seed);
    if (args.csv)
        std::cout << "game,algorithm,iteration,nodes_touched,solve_seconds,exploitability_ma,"
                     "value_lower,value_upper\n";
    double solving = 0; // Exploitability checks are not counted as solve time.
    // Report every --every iterations, or with --growing on a schedule that
    // multiplies by about 1.3 each time (for log-scale plots), or once at the end.
    std::uint64_t next = args.growing ? 1 : args.every ? args.every : args.iterations;
    while (solver.iteration() < args.iterations) {
        const auto start = std::chrono::steady_clock::now();
        const auto stop = std::min<std::uint64_t>(next, args.iterations);
        while (solver.iteration() < stop) solver.iterate();
        solving += seconds_since(start);
        report(game, solver, solving, args.csv);
        if (args.growing) next = std::max(next + 1, next * 13 / 10);
        else next += args.every ? args.every : args.iterations;
    }
    if (!args.save.empty()) save_strategy(game, solver.average_strategy(), args.save);
    return 0;
}

int cmd_eval(const Game& game, const Args& args) {
    const auto e = exploitability(game, load_strategy(game, args.strategy));
    std::cout << std::setprecision(10)
        << "value_to_first_player " << e.value << '\n'
        << "best_response_vs_second_player " << e.best_vs_p1 << '\n'
        << "best_response_vs_first_player " << e.best_vs_p0 << '\n'
        << "exploitability_ma_per_hand " << 1000 * e.exploitability() << '\n'
        << "game_value_between " << e.value_lower() << ' ' << e.value_upper() << '\n';
    return 0;
}

int cmd_show(const Game& game, const Args& args) {
    const auto strategy = load_strategy(game, args.strategy);
    for (const auto& infoset : game.infosets) {
        std::cout << std::left << std::setw(16) << infoset.key << std::right << std::fixed << std::setprecision(3);
        for (std::int32_t a = 0; a < infoset.action_count; ++a)
            std::cout << "  " << infoset.actions[static_cast<std::size_t>(a)] << ' '
                << strategy[static_cast<std::size_t>(infoset.offset + a)];
        std::cout << '\n';
    }
    return 0;
}

// Average profit per hand of `mine` (a strategy for both seats) against
// `opponent`, taking each seat half the time.
double profit_against(const Game& game, const Strategy& mine, const Strategy& opponent) {
    return 0.5 * (expected_value(game, with_player(game, opponent, mine, 0)) -
                  expected_value(game, with_player(game, mine, opponent, 0)));
}

// The most profitable counter-strategy to `opponent` in both seats. Where the
// opponent never lets the game go there is nothing to gain, and the
// counter-strategy keeps `fallback` (the equilibrium) there.
Strategy best_counter(const Game& game, const Strategy& opponent, const Strategy& fallback) {
    Strategy seat[2];
    for (int me = 0; me < 2; ++me)
        best_response_value(game, with_player(game, opponent, fallback, me), me, &seat[me]);
    return combine(game, seat[0], seat[1]);
}

// What the equilibrium earns from each weak opponent, what the best possible
// counter-strategy would earn, and what that counter-strategy risks.
int cmd_exploit(const Game& game, const Args& args) {
    const auto nash = load_strategy(game, args.strategy);
    if (args.csv)
        std::cout << "opponent,opponent_exploitability_ma,nash_profit_ma,max_profit_ma,"
                     "max_exploit_own_exploitability_ma,nash_share_of_max\n";
    else
        std::cout << "Milli-antes per hand, averaged over both seats.\n"
            << std::left << std::setw(10) << "opponent" << std::right << std::setw(12) << "its leak"
            << std::setw(14) << "equilibrium" << std::setw(14) << "max exploit" << std::setw(20)
            << "max exploit's leak" << '\n';
    for (const std::string kind : {"random", "station", "maniac", "rock"}) {
        const auto opponent = archetype(game, kind);
        const auto counter = best_counter(game, opponent, nash);
        const double nash_profit = profit_against(game, nash, opponent);
        const double max_profit = profit_against(game, counter, opponent);
        const double leak = exploitability(game, opponent).exploitability();
        const double risk = exploitability(game, counter).exploitability();
        if (args.csv)
            std::cout << kind << ',' << std::setprecision(8) << 1000 * leak << ',' << 1000 * nash_profit
                << ',' << 1000 * max_profit << ',' << 1000 * risk << ',' << nash_profit / max_profit << '\n';
        else
            std::cout << std::left << std::setw(10) << kind << std::right << std::fixed
                << std::setprecision(1) << std::setw(12) << 1000 * leak << std::setw(14)
                << 1000 * nash_profit << std::setw(14) << 1000 * max_profit << std::setw(20)
                << 1000 * risk << '\n' << std::defaultfloat;
    }
    return 0;
}

// Profit against one opponent type versus the amount a perfect opponent could
// win back, traced two ways:
//   mix         flip a coin before the hand between equilibrium and the best
//               counter-strategy (the obvious way to "exploit a little");
//   restricted  the restricted Nash response, which is the most profit
//               available at each level of risk.
int cmd_frontier(const Game& game, const Args& args) {
    const auto nash = load_strategy(game, args.strategy);
    const auto opponent = archetype(game, args.opponent);
    const auto counter = best_counter(game, opponent, nash);
    std::cout << "opponent,method,setting,profit_ma,own_exploitability_ma\n";
    const auto row = [&](const char* method, double setting, const Strategy& mine) {
        std::cout << args.opponent << ',' << method << ',' << std::setprecision(6) << setting << ','
            << std::setprecision(8) << 1000 * profit_against(game, mine, opponent) << ','
            << 1000 * exploitability(game, mine).exploitability() << '\n';
    };
    for (int step = 0; step <= args.steps; ++step) {
        const double share = static_cast<double>(step) / args.steps;
        row("mix", share, combine(game, mix(game, nash, counter, share, 0), mix(game, nash, counter, share, 1)));
    }
    for (int step = 0; step <= args.steps; ++step) {
        const double confidence = static_cast<double>(step) / args.steps;
        Strategy seat[2];
        for (int me = 0; me < 2; ++me) {
            const Game restricted = restricted_game(game, opponent, confidence, me);
            Solver solver(restricted, Algorithm::CfrPlus, args.seed);
            for (std::uint64_t i = 0; i < args.iterations; ++i) solver.iterate();
            seat[me] = solver.average_strategy();
        }
        row("restricted", confidence, combine(game, seat[0], seat[1]));
    }
    return 0;
}

int cmd_export(const Game& game) {
    std::cout << std::setprecision(17) << "{\"name\":\"" << game.name << "\",\"nodes\":[";
    for (std::size_t i = 0; i < game.nodes.size(); ++i) {
        const auto& node = game.nodes[i];
        if (i) std::cout << ',';
        if (node.type == NodeType::Terminal) {
            std::cout << "{\"t\":\"T\",\"u\":" << node.utility << '}';
            continue;
        }
        std::cout << "{\"t\":\"" << (node.type == NodeType::Chance ? 'C' : 'D') << "\",\"c\":[";
        for (std::int32_t a = 0; a < node.child_count; ++a)
            std::cout << (a ? "," : "") << game.child(node, a);
        std::cout << ']';
        if (node.type == NodeType::Chance) {
            std::cout << ",\"p\":[";
            for (std::int32_t a = 0; a < node.child_count; ++a)
                std::cout << (a ? "," : "") << game.probability(node, a);
            std::cout << ']';
        } else {
            std::cout << ",\"i\":" << node.infoset;
        }
        std::cout << '}';
    }
    std::cout << "],\"infosets\":[";
    for (std::size_t i = 0; i < game.infosets.size(); ++i) {
        const auto& infoset = game.infosets[i];
        std::cout << (i ? "," : "") << "{\"player\":" << static_cast<int>(infoset.player)
            << ",\"key\":\"" << infoset.key << "\",\"actions\":\"" << infoset.actions << "\"}";
    }
    std::cout << "]}\n";
    return 0;
}

// Work needed to reach a target accuracy. Every algorithm gets the same budget
// of node visits, which is the fair unit: one sampled iteration touches a
// sliver of the tree and one full iteration touches most of it.
int cmd_bench(const Game& game, const Args& args) {
    std::cout << "game,algorithm,trial,target_ma,reached,iterations,nodes_touched,solve_seconds,"
                 "nodes_per_second,final_exploitability_ma\n";
    for (const auto algorithm : {Algorithm::Cfr, Algorithm::Linear, Algorithm::Dcfr,
                                 Algorithm::CfrPlus, Algorithm::Mccfr}) {
        for (int trial = 1; trial <= args.trials; ++trial) {
            Solver solver(game, algorithm, args.seed + static_cast<std::uint64_t>(trial));
            const bool sampled = algorithm == Algorithm::Mccfr;
            std::uint64_t check = sampled ? 2000 : 10;
            double solving = 0;
            double gap = 1e9;
            while (solver.nodes_touched() < args.max_nodes && gap > args.target) {
                const auto start = std::chrono::steady_clock::now();
                const auto stop = solver.iteration() + check;
                while (solver.iteration() < stop && solver.nodes_touched() < args.max_nodes) solver.iterate();
                solving += seconds_since(start);
                gap = 1000 * exploitability(game, solver.average_strategy()).exploitability();
                // Check about every 5% of progress so the stopping point is tight.
                check = std::max<std::uint64_t>(check, solver.iteration() / 20);
            }
            std::cout << game.name << ',' << algorithm_name(algorithm) << ',' << trial << ','
                << args.target << ',' << (gap <= args.target ? 1 : 0) << ',' << solver.iteration()
                << ',' << solver.nodes_touched() << ',' << std::setprecision(6) << solving << ','
                << std::setprecision(8) << static_cast<double>(solver.nodes_touched()) / solving << ','
                << gap << '\n';
            if (!sampled) break; // Deterministic algorithms need one trial.
        }
    }
    return 0;
}

// Text-mode heads-up play against a saved strategy.
int cmd_play(const Game& game, const Args& args) {
    const auto strategy = load_strategy(game, args.strategy);
    Rng rng(args.seed * 7919 + 13);
    const int human = args.seat;
    double total = 0;
    int hands = 0;
    std::cout << "You are player " << human << ". Actions: k check, b bet, f fold, c call, r raise, q quit.\n";
    while (true) {
        std::int32_t index = 0;
        bool quit = false;
        std::string shown_public;
        while (game.nodes[static_cast<std::size_t>(index)].type != NodeType::Terminal) {
            const Node& node = game.nodes[static_cast<std::size_t>(index)];
            if (node.type == NodeType::Chance) {
                double roll = rng.uniform();
                std::int32_t pick = node.child_count - 1;
                for (std::int32_t a = 0; a < node.child_count; ++a) {
                    roll -= game.probability(node, a);
                    if (roll < 0) {
                        pick = a;
                        break;
                    }
                }
                index = game.child(node, pick);
                continue;
            }
            const auto& infoset = game.infosets[static_cast<std::size_t>(node.infoset)];
            if (infoset.public_rank >= 0 && shown_public.empty()) {
                shown_public = game.rank_names[static_cast<std::size_t>(infoset.public_rank)];
                std::cout << "  public card: " << shown_public << '\n';
            }
            std::int32_t pick = -1;
            if (node.player == human) {
                while (pick < 0) {
                    std::cout << "  your card " << game.rank_names[static_cast<std::size_t>(infoset.private_rank)]
                        << ", betting so far \"" << infoset.history << "\"  [" << infoset.actions << "] > " << std::flush;
                    std::string input;
                    if (!std::getline(std::cin, input) || input == "q") {
                        quit = true;
                        break;
                    }
                    const auto position = input.size() == 1 ? infoset.actions.find(input[0]) : std::string::npos;
                    if (position != std::string::npos) pick = static_cast<std::int32_t>(position);
                }
                if (quit) break;
            } else {
                double roll = rng.uniform();
                pick = infoset.action_count - 1;
                for (std::int32_t a = 0; a < infoset.action_count; ++a) {
                    roll -= strategy[static_cast<std::size_t>(infoset.offset + a)];
                    if (roll < 0) {
                        pick = a;
                        break;
                    }
                }
                std::cout << "  opponent: " << infoset.actions[static_cast<std::size_t>(pick)] << '\n';
            }
            index = game.child(node, pick);
        }
        if (quit) break;
        const double won = (human == 0 ? 1 : -1) * game.nodes[static_cast<std::size_t>(index)].utility;
        total += won;
        ++hands;
        std::cout << "  result: " << std::showpos << won << std::noshowpos << " antes   (total "
            << std::showpos << total << std::noshowpos << " over " << hands << " hands)\n\n";
    }
    if (hands > 0)
        std::cout << "\nYou averaged " << std::showpos << total / hands << std::noshowpos
            << " antes per hand over " << hands << " hands.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse(argc, argv);
        if (args.command == "--help" || args.command == "help") {
            usage();
            return 0;
        }
        const auto game = make_game(args.game);
        if (args.command == "info") return cmd_info(game);
        if (args.command == "solve") return cmd_solve(game, args);
        if (args.command == "eval") return cmd_eval(game, args);
        if (args.command == "show") return cmd_show(game, args);
        if (args.command == "exploit") return cmd_exploit(game, args);
        if (args.command == "frontier") return cmd_frontier(game, args);
        if (args.command == "export") return cmd_export(game);
        if (args.command == "bench") return cmd_bench(game, args);
        if (args.command == "play") return cmd_play(game, args);
        throw std::invalid_argument("unknown command: " + args.command);
    } catch (const std::exception& error) {
        std::cerr << "zerosum: " << error.what() << "\n(run 'zerosum help' for usage)\n";
        return 2;
    }
}
