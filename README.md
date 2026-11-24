# Worstfish

**The chess engine that always plays the worst move.**

Worstfish uses full-strength Stockfish to find the worst possible move in every position, and plays it.

On every turn it asks Stockfish to evaluate **every legal move**, then plays the one Stockfish rates lowest. If a move hangs the queen, Worstfish finds it. If a move allows mate in one, Worstfish plays it. If it can somehow lose faster, it will.

```
1. e4 f5 2. exf5 g5 3. Qh5#   (Stockfish vs Worstfish)
```

## How it works

Worstfish is a small UCI engine that sits between your chess GUI and Stockfish:

1. The GUI sends Worstfish a position and a clock, like any other engine.
2. Worstfish passes the position to Stockfish with `MultiPV 500`, which makes Stockfish search and score every legal move instead of only the best one.
3. When Stockfish finishes, Worstfish takes the move with the **lowest** score and plays it.

Getting mated sooner counts as worse than getting mated later, so if Worstfish can walk into mate in one, it does.

Stockfish always runs at full strength. Worstfish ignores settings that would weaken it (such as `Skill Level`), because a weaker Stockfish would be worse at finding the worst move.

## Building

You need a C compiler and Stockfish.

```sh
make
```

That produces a single `worstfish` binary. It has no dependencies beyond the C standard library and POSIX.

## Running

Point Worstfish at your Stockfish binary:

```sh
./worstfish /path/to/stockfish
```

Or set `WORSTFISH_ENGINE`, or put `stockfish` on your `PATH` and run `./worstfish` with no arguments.

Add it to any UCI chess GUI (Cute Chess, Arena, Banksia, En Croissant, etc.) as you would any other engine. If your GUI won't pass arguments, use a small wrapper script:

```sh
#!/bin/sh
exec /path/to/worstfish /path/to/stockfish
```

## UCI options

| Option | Default | What it does |
|---|---|---|
| `Threads` | 1 | Passed to Stockfish. |
| `Hash` | 16 | Passed to Stockfish (MB). |
| `DefaultMoveTime` | 1000 | Thinking time in ms when the GUI gives no clock (for example `go infinite` or `go depth`). |

Worstfish follows the clock it is given: `movetime`, or `wtime`/`btime` with increments and `movestogo`. It never spends the last 10% of its remaining time.

## Strength

Worstfish loses almost every game against almost every opponent, including most beginners. Against Stockfish it usually lasts 3 to 12 moves. Two copies of Worstfish against each other tend to shuffle until a draw by repetition, because each side keeps refusing to win.

## Platforms

Worstfish is plain C and runs on Linux, macOS and other Unix-like systems. It has been tested with Stockfish 11 and Stockfish 16.

## License

GPL-3.0-or-later, the same license as Stockfish. See [LICENSE](LICENSE).

Worstfish does not include any Stockfish code. It runs Stockfish as a separate program. Stockfish is developed by the Stockfish team: https://stockfishchess.org
