/*
 * Worstfish - a UCI chess engine that always plays the worst move.
 *
 * Worstfish runs Stockfish at full strength as an oracle. On every move it asks
 * Stockfish to evaluate every legal move (MultiPV), then plays the one Stockfish
 * rates lowest. It is, as far as Stockfish can tell, the worst possible move.
 *
 * Usage:
 *   worstfish [path-to-stockfish]
 *
 * The Stockfish command can also be set with the WORSTFISH_ENGINE environment
 * variable. If neither is given, "stockfish" is looked up on the PATH.
 *
 * Copyright (C) 2026 The Worstfish authors
 * Licensed under the GNU General Public License v3.0 or later. See LICENSE.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORSTFISH_VERSION "1.0"
#define MAXMOVES 256
#define LINEMAX 65536

static int to_engine = -1, from_engine = -1;
static pid_t engine_pid = -1;

static char position[LINEMAX] = "position startpos";
static char moves[MAXMOVES][8];
static long scores[MAXMOVES];
static int nmoves = 0;
static int searching = 0;
static long default_movetime = 1000;   /* used when the GUI gives no clock */

/* ------------------------------------------------------------------ I/O */

static void write_all(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void out(const char *s) { write_all(1, s); }
static void eng(const char *s) { write_all(to_engine, s); }

static void start_engine(const char *cmd) {
    int in[2], outp[2];
    if (pipe(in) || pipe(outp)) {
        out("info string worstfish: cannot create pipes\n");
        _exit(1);
    }
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(in[0], 0);
        dup2(outp[1], 1);
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    close(in[0]);
    close(outp[1]);
    to_engine = in[1];
    from_engine = outp[0];
    eng("uci\n"
        "setoption name MultiPV value 500\n"
        "setoption name Skill Level value 20\n"
        "setoption name UCI_LimitStrength value false\n"
        "isready\n");
}

/* --------------------------------------------------------------- parsing */

/* Return a pointer just past "key " if the word key appears in line. */
static const char *after(const char *line, const char *key) {
    size_t k = strlen(key);
    const char *p = line;
    while ((p = strstr(p, key)) != NULL) {
        int start_ok = (p == line) || p[-1] == ' ';
        int end_ok = p[k] == ' ';
        if (start_ok && end_ok) return p + k + 1;
        p += k;
    }
    return NULL;
}

static long num_after(const char *line, const char *key, long def) {
    const char *p = after(line, key);
    return p ? strtol(p, NULL, 10) : def;
}

/* Is it white to move in the stored position? */
static int white_to_move(void) {
    const char *fen = after(position, "fen");
    int white = 1;
    if (fen) {
        /* second FEN field is the side to move */
        const char *sp = strchr(fen, ' ');
        if (sp) white = sp[1] != 'b';
    }
    const char *mv = after(position, "moves");
    if (mv) {
        int count = 0, in_word = 0;
        for (; *mv; mv++) {
            if (*mv != ' ' && !in_word) { count++; in_word = 1; }
            else if (*mv == ' ') in_word = 0;
        }
        if (count % 2) white = !white;
    }
    return white;
}

/* Decide how long Stockfish may think, from the GUI's "go" command. */
static long think_time(const char *go) {
    long movetime = num_after(go, "movetime", -1);
    if (movetime > 0) return movetime;
    int white = white_to_move();
    long left = num_after(go, white ? "wtime" : "btime", -1);
    long inc = num_after(go, white ? "winc" : "binc", 0);
    long togo = num_after(go, "movestogo", 0);
    if (left < 0) return default_movetime;     /* infinite, depth, nodes... */
    long divisor = togo > 0 ? togo + 2 : 30;
    long t = left / divisor + inc * 3 / 4;
    long safety = left / 10;                  /* never spend the last 10% */
    if (t > left - safety) t = left - safety;
    if (t < 20) t = 20;
    return t;
}

static void record_info(const char *line) {
    if (strstr(line, "lowerbound") || strstr(line, "upperbound")) return;
    const char *sc = after(line, "score");
    const char *pv = after(line, "pv");
    if (!sc || !pv) return;

    long val;
    if (strncmp(sc, "cp ", 3) == 0) {
        val = strtol(sc + 3, NULL, 10);
    } else if (strncmp(sc, "mate ", 5) == 0) {
        long m = strtol(sc + 5, NULL, 10);
        /* Being mated sooner is worse; mating sooner is better. */
        val = (m > 0) ? 100000 - m : -100000 - m;
    } else {
        return;
    }

    char mv[8];
    int i = 0;
    while (pv[i] && pv[i] != ' ' && i < 7) { mv[i] = pv[i]; i++; }
    mv[i] = 0;
    if (i < 4) return;

    for (int j = 0; j < nmoves; j++) {
        if (strcmp(moves[j], mv) == 0) { scores[j] = val; return; }
    }
    if (nmoves < MAXMOVES) {
        memcpy(moves[nmoves], mv, sizeof mv);
        scores[nmoves] = val;
        nmoves++;
    }
}

/* ---------------------------------------------------------- engine side */

static void handle_engine_line(char *line) {
    if (strncmp(line, "info ", 5) == 0) {
        if (searching) record_info(line);
        return;
    }
    if (strncmp(line, "bestmove", 8) != 0 || !searching) return;
    searching = 0;
    if (nmoves == 0) {                 /* no legal moves, or no info: pass it on */
        out(line);
        out("\n");
        return;
    }
    int worst = 0;
    for (int j = 1; j < nmoves; j++)
        if (scores[j] < scores[worst]) worst = j;
    char buf[64];
    snprintf(buf, sizeof buf, "info string worstfish: %d moves rated, worst scored %ld\n",
             nmoves, scores[worst]);
    out(buf);
    snprintf(buf, sizeof buf, "bestmove %.7s\n", moves[worst]);
    out(buf);
}

/* ------------------------------------------------------------- GUI side */

static void handle_gui_line(char *line) {
    if (strcmp(line, "uci") == 0) {
        out("id name Worstfish " WORSTFISH_VERSION "\n"
            "id author The Worstfish authors\n"
            "option name Threads type spin default 1 min 1 max 1024\n"
            "option name Hash type spin default 16 min 1 max 33554432\n"
            "option name DefaultMoveTime type spin default 1000 min 10 max 600000\n"
            "uciok\n");
    } else if (strcmp(line, "isready") == 0) {
        out("readyok\n");
    } else if (strncmp(line, "setoption", 9) == 0) {
        const char *name = after(line, "name");
        if (!name) return;
        if (strncmp(name, "Threads ", 8) == 0 || strncmp(name, "Hash ", 5) == 0) {
            eng(line);                 /* pass hardware settings through */
            eng("\n");
        } else if (strncmp(name, "DefaultMoveTime ", 16) == 0) {
            long v = num_after(line, "value", default_movetime);
            if (v >= 10) default_movetime = v;
        }
        /* Anything that would weaken Stockfish (Skill Level etc.) is ignored. */
    } else if (strcmp(line, "ucinewgame") == 0) {
        eng("ucinewgame\n");
    } else if (strncmp(line, "position", 8) == 0) {
        snprintf(position, sizeof position, "%s", line);
    } else if (strncmp(line, "go", 2) == 0 && (line[2] == 0 || line[2] == ' ')) {
        static char buf[LINEMAX + 64];
        nmoves = 0;
        searching = 1;
        snprintf(buf, sizeof buf, "%s\ngo movetime %ld\n", position, think_time(line));
        eng(buf);
    } else if (strcmp(line, "stop") == 0) {
        eng("stop\n");
    } else if (strcmp(line, "quit") == 0) {
        eng("quit\n");
        waitpid(engine_pid, NULL, 0);
        _exit(0);
    }
}

/* ------------------------------------------------------------- main loop */

typedef struct { char buf[LINEMAX]; size_t len; } linebuf;

static int pump(int fd, linebuf *lb, void (*fn)(char *)) {
    char tmp[8192];
    ssize_t n = read(fd, tmp, sizeof tmp);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
        char c = tmp[i];
        if (c == '\n') {
            lb->buf[lb->len] = 0;
            if (lb->len && lb->buf[lb->len - 1] == '\r') lb->buf[lb->len - 1] = 0;
            fn(lb->buf);
            lb->len = 0;
        } else if (lb->len < LINEMAX - 1) {
            lb->buf[lb->len++] = c;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    const char *cmd = argc > 1 ? argv[1] : getenv("WORSTFISH_ENGINE");
    if (!cmd || !*cmd) cmd = "stockfish";
    const char *mt = getenv("WORSTFISH_MOVETIME");
    if (mt && *mt && strtol(mt, NULL, 10) >= 10) default_movetime = strtol(mt, NULL, 10);
    start_engine(cmd);

    static linebuf gui, en;
    struct pollfd fds[2] = {{0, POLLIN, 0}, {from_engine, POLLIN, 0}};
    for (;;) {
        if (poll(fds, 2, -1) < 0) continue;
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (!pump(from_engine, &en, handle_engine_line)) {
                out("info string worstfish: Stockfish exited\n");
                break;
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            if (!pump(0, &gui, handle_gui_line)) { eng("quit\n"); break; }
        }
    }
    if (engine_pid > 0) kill(engine_pid, SIGTERM);
    return 0;
}
