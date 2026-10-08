# ZeroSum project entry

Use these bullets after you have solved Kuhn poker by hand, run `make test`,
and can explain why the average strategy converges and the current one does
not. The project is ready to publish; no repository URL exists until you push
it (see `mac-and-github.md`). Do not describe it as a poker bot or as evidence
about real markets.

**ZeroSum | Game-Theoretic Solvers and Exploitation Study**
C++20, Python, SciPy, counterfactual regret minimization, linear programming

- Implemented five regret-minimization solvers (CFR, CFR+, Linear, Discounted,
  Monte Carlo) for imperfect-information poker in C++20; each solver's bounds
  contain the exact game value from an independent sequence-form linear program.
- Measured work to equal accuracy on Leduc hold'em: Discounted CFR converged in
  321 iterations against 842,438 for the original algorithm, about 2,000x fewer
  node visits.
- Quantified the cost of unexploitable play: a CFR+ equilibrium captured 9% to
  43% of the profit available against four weak opponent types; a restricted
  Nash response recovered half the remainder with 8x to 87x less risk than
  mixing in the best response.

Shorter two-bullet version:

- Built five CFR-family equilibrium solvers for imperfect-information poker in
  C++20 and verified them against an independent linear program; Discounted CFR
  needed about 2,000x fewer node visits than the original for equal accuracy.
- Showed a CFR+ equilibrium captured only 9% to 43% of the profit available
  against four weak opponent types, and that a restricted Nash response
  recovered half the remainder with 8x to 87x less risk than mixing in the best
  response.

Where it fits: first or second project on a quantitative trading resume,
beside QuoteLab or GammaScalp. It is weaker evidence for a general software
role than QuorumLog or CacheForge.

Each number above comes from `results/bench.csv`, `results/exploit.csv`,
`results/frontier.csv`, or `results/lp_check.csv`. Know the qualifiers: the
2,000x is for reaching 1 milli-ante on Leduc hold'em; 9% to 43% is the CFR+
equilibrium (the linear program's equilibrium captures 5% to 41%); and the risk
comparison is against a coin-flip mix of equilibrium and best response earning
the same profit (8.4x random, 10.9x rock, 16.1x maniac, 86.6x calling station).
