# Own the project in an interview

Before you present this, solve Kuhn poker on paper once (question 3), run
`make test`, and change one thing: a new opponent type in `archetype()`, or a
different bet size in `leduc_rules()`. Trading interviewers like this topic
and will push on it. Say plainly which parts you can derive and which you
implemented from a paper.

## A 45-second explanation

"I wanted to understand the gap between unexploitable play and profitable play.
I wrote five regret-minimization solvers in C++ for small poker games and
checked them against a linear program that solves the same games exactly.
Then I measured two things. First, how much work each algorithm needs:
discounted CFR reached the same accuracy as the original with about two
thousand times fewer node visits. Second, what equilibrium play leaves behind:
against four simple opponent types it won every time but collected only 9 to 43
percent of what was available. Taking all of it means playing a strategy that
is itself badly exploitable, so I implemented restricted Nash response, which
got half of the extra profit with about a tenth of the exposure that mixing in
the best response needs for the same profit."

## Questions you should be able to answer

**1. What is a Nash equilibrium in a two-player zero-sum game, and why is it
the right target?**

A pair of strategies where neither player can gain by changing theirs alone.
In zero-sum games it has a stronger meaning: your equilibrium strategy
guarantees you at least the game's value no matter what the opponent does.
That guarantee is why it is the default against an unknown opponent. It is not
a claim that it wins the most.

**2. What is exploitability and how do you compute it?**

Fix my strategy and let the opponent pick the best possible reply. The amount
that reply wins beyond the game value is my exploitability. I average it over
both seats so the game value cancels: `(best reply vs my seat-0 play + best
reply vs my seat-1 play) / 2`. It is zero exactly at equilibrium.

Computing the best reply is the subtle part. The replying player must choose
one action per information set, not per node, because they cannot see my card.
So `respond()` in `analysis.hpp` groups the nodes of each information set,
weights each by the probability that chance and my strategy bring the game
there, and picks the action with the best total.

**3. Solve Kuhn poker.**

Cards J, Q, K; each player antes 1; one bet of 1 is allowed.

Second player's indifference conditions give a unique answer: bluff with the J
one third of the time after a check, and call a bet with the Q one third of the
time. First player has a family: bet the J with probability a (any a from 0 to
1/3), bet the K with probability 3a, never bet the Q, and call with the Q with
probability a + 1/3. The value is −1/18 to the first player.

Why 3 to 1? When I bet, the opponent holding a Q must be indifferent between
calling and folding. Calling risks 1 to win 3 (the pot of 2 plus my bet), so
they need me to be bluffing one time in four: K bets to J bets is 3 to 1.

Why does the second player call with the Q one third of the time? To make my J
indifferent between bluffing and checking. Checking the J always loses the
ante. Compared with that, a bluff wins 2 more when it works and loses 1 more
when it is called, so it has to work one time in three. Half the time the
opponent holds the K and always calls, so the Q must fold two times in three,
which is calling one time in three. `tests/test_core.cpp` checks that every
member of this family has exploitability zero and that each solver's output
satisfies these relations.

**4. Explain CFR in two sentences.**

At every information set, keep a running total of how much better each action
would have done than the strategy you actually played, weighted by how likely
the other side and chance were to bring the game there. Play each action in
proportion to its positive total; the average of the strategies you played
converges to an equilibrium.

**5. Why "counterfactual"? Why weight by the opponent's reach and not yours?**

The regret at an information set asks "if I had tried to get here, how would
each action have done?" My own probability of getting here is something I
control, so it is left out; the opponent's and chance's are not. Leaving my own
reach out is what lets the regrets at different information sets be minimized
independently and still bound the regret for the whole game.

**6. Why does the average strategy converge and not the last one?**

Regret matching guarantees that average regret goes to zero, like 1/sqrt(T).
A standard result says that if both players' average regret is at most
epsilon, the average strategies form a 2-epsilon equilibrium. The current
strategy can keep cycling. Rock-paper-scissors shows it: the current strategy
chases the last mistake while the average sits at one third each.

**7. Why is CFR+ so much faster than CFR?**

Plain CFR lets regret go deeply negative for an action that has been bad for a
long time. When that action becomes good, it takes many iterations to climb
back to zero before it is played again. CFR+ floors regret at zero, so the
action is picked up immediately. It also updates players in turn and weights
later iterations more in the average, since early strategies are poor.
Discounted CFR does the same job more gently: it shrinks old negative regret
by half each iteration instead of erasing it.

Be honest about the limit of this answer: the worst-case bound for all of
them is still 1/sqrt(T). The speedup is empirical. On Leduc I measured 321
iterations against 842,438 for the same accuracy.

**8. Your sampled solver was the slowest. Is it broken?**

No, and here is how I checked: it converges (the tests require it to get
under 5 mA on Kuhn and to improve fivefold on Leduc), its value bracket always
contains the LP value, and it is reproducible from its seed. It loses here
because Leduc's entire tree is 1,936 nodes. A full traversal is cheap and
exact; a sampled one is cheap and noisy. Sampling is for trees too big to
walk, which I did not build. I would rather report that than leave it out.

**9. How do you know the solvers are right?**

Three independent checks.
- A sequence-form linear program in Python, sharing no code with the C++
  solvers, gives the exact value. Every solver's bracket contains it.
- The LP's strategy, written to a file and evaluated by the C++ best-response
  code, has exploitability below 10^-9 mA. So the evaluator and the LP agree.
- The rules themselves are written twice: C++ deals ranks with probabilities,
  the Python reference deals cards from a deck. Same information sets, same
  value.

A bug would have to be made the same way in two languages and two algorithms.

**10. What is the sequence form, briefly?**

A strategy written as the probability of each sequence of my own actions,
rather than each action at each information set. With perfect recall, those
probabilities are tied together by linear constraints ("what flows into an
information set flows out"), and expected payoff is `x' A y`. The best reply to
a fixed x is then a linear program, and taking its dual lets one LP choose x
and price the opponent's reply together. The size is linear in the game tree,
unlike the normal form, which is exponential.

**11. Equilibrium won against every weak opponent. Why not just play it?**

Because it only collected 9 to 43 percent of what was there. Equilibrium play
makes the opponent indifferent in many spots, which means it also does not
punish their mistakes there. Against the "rock," who only puts money in with a
King or a pair, the right adjustment is obvious to a human (fold when they bet,
bet whenever they check) and worth ten times the equilibrium profit.

**12. So why not play the best response?**

It is brittle. The best response to the rock loses 963 mA per hand to a
perfect opponent, more than the 700 it was making. It is only right if the
model of the opponent is exactly right and they never adjust.

**13. What is a restricted Nash response?**

Imagine a coin is flipped before the hand. With probability p the opponent is
the model; otherwise the opponent can do anything. I do not see the coin.
The equilibrium of that game is my best strategy against the model among all
strategies with at most a certain exploitability. p = 0 gives the plain
equilibrium, p = 1 the best response, and in between traces the whole
trade-off.

I implemented it as a game transformation (`restricted_game()`): two copies of
the tree under a new chance node, with the opponent's decisions replaced by
chance moves in the copy where they follow the model, and my information sets
shared across both copies. Any solver then runs on it unchanged.

**14. Why does it beat simply mixing equilibrium and best response?**

The mix plays the entire best response some fraction of the time, including
the parts of it that risk a lot to gain a little. The restricted response
chooses, decision by decision, where deviating pays most per unit of exposure.
Against the rock at p = 0.2 it earned 381 mA with 40 mA of exposure; the mix
needed 444 mA of exposure for the same profit.

**15. Two equilibria earned different amounts against the rock. How?**

Equilibria of a zero-sum game all have the same value against a best-responding
opponent. Against a flawed opponent they can differ, because they differ in
spots the equilibrium opponent makes irrelevant. The LP returns a corner
solution and CFR+ returns something more mixed; against the rock they earned
33 and 66 mA. It is a reminder that "the equilibrium" is not one strategy.

**16. How is any of this like trading?**

Carefully: it is an analogy, not a model of a market. A market maker quoting
both sides against unknown counterparties faces the same choice between a
strategy that cannot be picked off and one that leans on a belief about who
is on the other side. The restricted-response idea, "be right if my read is
right, lose a bounded amount if it is wrong," is how I would think about sizing
any view. I would not claim more than that.

**17. What would you do next?**

Estimate the opponent from a finite sample of hands and see how many hands it
takes before exploiting beats equilibrium; add a game with bet sizing so the
action space is not two or three choices; replace the explicit tree with a
vectorized public-state solver.

## Numbers to know

| Fact | Value |
| --- | --- |
| Kuhn value to first player | −1/18 ≈ −0.0556 antes |
| Leduc value to first player | −0.085606 antes |
| Leduc information sets | 288 with suits merged, 936 without |
| Work to 1 mA on Leduc: CFR / DCFR | 2,368,783,718 / 1,175,470 node visits (about 2,000×) |
| CFR+ equilibrium's share of the maximum profit | 9% (rock) to 43% (calling station); the LP equilibrium, 5% to 41% |
| Rock: best response profit / its own exploitability | 700 / 963 mA per hand |
| Rock: restricted response at p = 0.2 | 381 mA profit, 40 mA exposure (the equal-profit mix needs 444 mA) |
| Risk ratio, mix vs restricted response, at half the extra profit | 8.4× random, 10.9× rock, 16.1× maniac, 86.6× station |
| Tests | 24 C++ cases, 15 Python tests |

## What not to claim

- Not a poker bot, and not evidence about real poker or real markets.
- Not a new algorithm. Every method here is from a published paper; the
  contribution is a clean implementation, an independent check, and the
  measurements.
- The 2,000× is for one game and one accuracy target, and the exact count moves
  by several percent with floating-point details. It will be different on other
  games.
