#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace zerosum {

enum class NodeType : std::uint8_t { Chance, Decision, Terminal };

// One history (a full sequence of deals and actions so far).
struct Node {
    NodeType type{NodeType::Terminal};
    std::int8_t player{-1};      // Who acts, for a decision node.
    std::int32_t infoset{-1};    // What that player knows, for a decision node.
    std::int32_t first_child{};  // Index into Game::children.
    std::int32_t child_count{};
    double utility{};            // Terminal payoff to player 0, in antes. Player 1 gets the negative.
};

// Everything one player cannot tell apart: same private card, same public
// card, same betting. The player must act the same way at every node in it.
struct Infoset {
    std::int8_t player{};
    std::int32_t action_count{};
    std::int32_t offset{};    // Start of this infoset's slice in per-action arrays.
    std::string key;          // Human-readable, e.g. "0:K:-:kb".
    std::string actions;      // One letter per action: k check, b bet, f fold, c call, r raise.
    std::int8_t private_rank{};
    std::int8_t public_rank{-1};
    std::string history;
};

// A two-player zero-sum game stored as an explicit tree.
struct Game {
    std::string name;
    std::vector<Node> nodes;             // nodes[0] is the root.
    std::vector<std::int32_t> children;  // Child node of action a: children[first_child + a].
    std::vector<double> chance_prob;     // Parallel to children; meaningful under chance nodes.
    std::vector<Infoset> infosets;
    std::int32_t action_slots{};         // Total actions over all infosets.
    std::vector<std::string> rank_names;

    [[nodiscard]] std::int32_t child(const Node& node, std::int32_t action) const {
        return children[static_cast<std::size_t>(node.first_child + action)];
    }
    [[nodiscard]] double probability(const Node& node, std::int32_t action) const {
        return chance_prob[static_cast<std::size_t>(node.first_child + action)];
    }
};

// A behavior strategy for both players: one probability per (infoset, action),
// laid out by Infoset::offset.
using Strategy = std::vector<double>;

inline Strategy uniform_strategy(const Game& game) {
    Strategy s(static_cast<std::size_t>(game.action_slots));
    for (const auto& infoset : game.infosets)
        for (std::int32_t a = 0; a < infoset.action_count; ++a)
            s[static_cast<std::size_t>(infoset.offset + a)] = 1.0 / infoset.action_count;
    return s;
}

// The family of small poker games used here. Each player antes, is dealt one
// private card, and bets over one or two rounds; a public card is revealed
// before the second round.
struct PokerRules {
    std::string name;
    int ranks{3};          // Distinct card ranks.
    int copies{1};         // Cards per rank in the deck.
    int rounds{1};
    std::array<int, 2> bet{1, 1}; // Bet size in each round.
    int max_raises{1};     // Bets plus raises allowed per round.
    int ante{1};
};

// Kuhn (1950): three cards, one round, one bet. Solvable by hand.
inline PokerRules kuhn_rules() { return {"kuhn", 3, 1, 1, {1, 1}, 1, 1}; }

// Leduc hold'em (Southey et al., 2005): two suits, two rounds, bets of 2 then
// 4, at most two bets per round, and a public card that can pair a hand.
inline PokerRules leduc_rules(int ranks = 3) {
    return {ranks == 3 ? "leduc" : "leduc" + std::to_string(ranks), ranks, 2, 2, {2, 4}, 2, 1};
}

namespace detail {

class PokerBuilder {
public:
    explicit PokerBuilder(const PokerRules& rules) : rules_(rules) {
        if (rules.ranks < 2 || rules.ranks > 13 || rules.copies < 1 || rules.copies > 4 ||
            rules.rounds < 1 || rules.rounds > 2 || rules.max_raises < 1 || rules.ante < 1)
            throw std::invalid_argument("unsupported poker rules");
        if (rules.ranks * rules.copies < 2 + (rules.rounds - 1))
            throw std::invalid_argument("deck is too small");
        game_.name = rules.name;
        // Three-rank games use J, Q, K as in the papers that introduced them;
        // larger decks take the top N ranks of a standard deck.
        static const char* thirteen[] = {"2", "3", "4", "5", "6", "7", "8", "9", "T", "J", "Q", "K", "A"};
        const int first = rules.ranks == 3 ? 9 : 13 - rules.ranks;
        for (int r = 0; r < rules.ranks; ++r) game_.rank_names.push_back(thirteen[first + r]);
    }

    Game build() {
        const int deck = rules_.ranks * rules_.copies;
        const int root = add_node(NodeType::Chance);
        std::vector<int> kids;
        std::vector<double> probs;
        for (int a = 0; a < rules_.ranks; ++a) {
            for (int b = 0; b < rules_.ranks; ++b) {
                const int left = rules_.copies - (a == b ? 1 : 0);
                if (left <= 0) continue;
                State state;
                state.rank = {a, b};
                state.paid = {rules_.ante, rules_.ante};
                kids.push_back(betting(state));
                probs.push_back(static_cast<double>(rules_.copies) / deck *
                                static_cast<double>(left) / (deck - 1));
            }
        }
        attach(root, kids, probs);
        return std::move(game_);
    }

private:
    struct State {
        std::array<int, 2> rank{};
        int public_rank{-1};
        int round{0};
        std::array<int, 2> paid{};
        int raises{0};       // Bets and raises so far this round.
        int to_act{0};
        bool checked{false}; // The first player checked this round.
        std::string history;
    };

    int add_node(NodeType type) {
        Node node;
        node.type = type;
        game_.nodes.push_back(node);
        return static_cast<int>(game_.nodes.size() - 1);
    }

    void attach(int node, const std::vector<int>& kids, const std::vector<double>& probs) {
        game_.nodes[static_cast<std::size_t>(node)].first_child = static_cast<std::int32_t>(game_.children.size());
        game_.nodes[static_cast<std::size_t>(node)].child_count = static_cast<std::int32_t>(kids.size());
        for (std::size_t i = 0; i < kids.size(); ++i) {
            game_.children.push_back(kids[i]);
            game_.chance_prob.push_back(probs.empty() ? 0.0 : probs[i]);
        }
    }

    int terminal(double utility) {
        const int node = add_node(NodeType::Terminal);
        game_.nodes[static_cast<std::size_t>(node)].utility = utility;
        return node;
    }

    // Winner takes what the loser paid in.
    int showdown(const State& s) {
        const auto strength = [&](int player) {
            const int rank = s.rank[static_cast<std::size_t>(player)];
            return (rank == s.public_rank ? 100 : 0) + rank; // A pair beats any high card.
        };
        const int a = strength(0);
        const int b = strength(1);
        if (a == b) return terminal(0.0);
        return terminal(a > b ? s.paid[1] : -s.paid[0]);
    }

    int end_of_round(State s) {
        if (s.round + 1 >= rules_.rounds) return showdown(s);
        // Reveal the public card.
        const int deck = rules_.ranks * rules_.copies;
        const int node = add_node(NodeType::Chance);
        std::vector<int> kids;
        std::vector<double> probs;
        for (int r = 0; r < rules_.ranks; ++r) {
            const int left = rules_.copies - (r == s.rank[0] ? 1 : 0) - (r == s.rank[1] ? 1 : 0);
            if (left <= 0) continue;
            State next = s;
            next.public_rank = r;
            next.round = s.round + 1;
            next.raises = 0;
            next.to_act = 0;
            next.checked = false;
            next.history += '/';
            kids.push_back(betting(next));
            probs.push_back(static_cast<double>(left) / (deck - 2));
        }
        attach(node, kids, probs);
        return node;
    }

    int betting(const State& s) {
        const auto me = static_cast<std::size_t>(s.to_act);
        const auto other = 1 - me;
        const int owed = s.paid[other] - s.paid[me];
        const int size = rules_.bet[static_cast<std::size_t>(s.round)];
        std::string actions;
        if (owed > 0) {
            actions = "fc";
            if (s.raises < rules_.max_raises) actions += 'r';
        } else {
            actions = "k";
            if (s.raises < rules_.max_raises) actions += 'b';
        }

        const int node = add_node(NodeType::Decision);
        const std::string public_name =
            s.public_rank < 0 ? "-" : game_.rank_names[static_cast<std::size_t>(s.public_rank)];
        const std::string key = std::to_string(s.to_act) + ":" +
            game_.rank_names[static_cast<std::size_t>(s.rank[me])] + ":" + public_name + ":" + s.history;
        auto found = ids_.find(key);
        if (found == ids_.end()) {
            Infoset infoset;
            infoset.player = static_cast<std::int8_t>(s.to_act);
            infoset.action_count = static_cast<std::int32_t>(actions.size());
            infoset.offset = game_.action_slots;
            infoset.key = key;
            infoset.actions = actions;
            infoset.private_rank = static_cast<std::int8_t>(s.rank[me]);
            infoset.public_rank = static_cast<std::int8_t>(s.public_rank);
            infoset.history = s.history;
            game_.action_slots += infoset.action_count;
            game_.infosets.push_back(infoset);
            found = ids_.emplace(key, static_cast<int>(game_.infosets.size() - 1)).first;
        }
        game_.nodes[static_cast<std::size_t>(node)].player = static_cast<std::int8_t>(s.to_act);
        game_.nodes[static_cast<std::size_t>(node)].infoset = found->second;

        std::vector<int> kids;
        for (const char action : actions) {
            State next = s;
            next.history += action;
            next.to_act = static_cast<int>(other);
            switch (action) {
            case 'f':
                kids.push_back(terminal(s.to_act == 0 ? -s.paid[0] : s.paid[1]));
                break;
            case 'c':
                next.paid[me] = s.paid[other];
                kids.push_back(end_of_round(next));
                break;
            case 'k':
                if (s.checked) {
                    kids.push_back(end_of_round(next));
                } else {
                    next.checked = true;
                    kids.push_back(betting(next));
                }
                break;
            default: // 'b' or 'r': match what is owed, then add one bet.
                next.paid[me] = s.paid[other] + size;
                next.raises = s.raises + 1;
                kids.push_back(betting(next));
                break;
            }
        }
        attach(node, kids, {});
        return node;
    }

    PokerRules rules_;
    Game game_;
    std::map<std::string, int> ids_;
};

} // namespace detail

inline Game build_poker(const PokerRules& rules) { return detail::PokerBuilder(rules).build(); }

// "kuhn", "leduc", or "leducN" for N ranks (4 to 13).
inline Game make_game(const std::string& name) {
    if (name == "kuhn") return build_poker(kuhn_rules());
    if (name == "leduc") return build_poker(leduc_rules(3));
    if (name.rfind("leduc", 0) == 0) {
        const int ranks = std::stoi(name.substr(5));
        return build_poker(leduc_rules(ranks));
    }
    throw std::invalid_argument("unknown game '" + name + "' (try kuhn, leduc, leduc5)");
}

} // namespace zerosum
