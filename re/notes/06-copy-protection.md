# 06 — Copy protection: the garage manual quiz (DOS 1.1)

Before a race, the game asks a question answered from the printed manual, e.g. *"In inches, how
long is the wheelbase of the Twin Turbo? (page 42)"*. The answer is typed in a box. The question
and answer data live in segment `0ACB` (the data segment that also holds the `.bin` file names).

## Encoding (confirmed)
Every byte is stored **minus 14h**: decode with `b + 14h` over the whole byte range, digits
included. Codex's earlier decoder only decoded bytes 20h–6Ah, which garbled every digit. The
decoded text uses `FFh` as a terminator / drawing-command escape.

## Layout (confirmed by decoding the unpacked image)
| What | Where | Format |
|---|---|---|
| Question text | from `0ACB:6083` (linear `10D33`) | two lines per question, each `text FF 04 00 <row> 00 23` (row `i`/`s` = upper/lower line) |
| Question offset table | `0ACB:6F34` (linear `11BE4`) | 42 words (offsets into `0ACB`), terminated by `FFFF` |
| Answer table | `0ACB:6F94` (linear `11C44`) | 10-byte slots in question order: text, `FF`, zero padding |

The first answers, in order: `3285, BORE, 1953, NEW YORK, DAYTONA, 700, 1957, 1963, 1887, 1776,
1968, 1933, 1939, 1981, 4300, 3313, 46.7, 176.5, 96.2, 3500, …`

Verified at runtime in the host: with the fixed start date used by `vette_run` (1989-10-23
12:00), the first race asks the Twin Turbo wheelbase question, and typing `96.2` starts the race.

## The routine: `manual_quiz` 4160:0A40 (far) (confirmed)
- `start` calls it at 3009:00CF only while `DS:2AEC` = FFh ("not asked yet"), i.e. before the first race of a session.
- It saves AX BX CX DX DS DI ES SI BP, then calls `random` (3009:8DB5 → 8D64). That routine returns CX =
  rotr(PIT ch0 count + seed, 3) and stores the result as the new seed at `cs:8D62`.
- Question index = high word of CX·48 (MUL), at most 47, stored at `0ACB:6EC0`. Attempts at `0ACB:6EC2` = 0.
- It draws the box and question, then reads keys through 3FFC:003D: Enter checks, Esc redraws, letters are
  upper-cased, and the input is at most 10 characters.
- **The answer check is disabled in this v1.1 EXE.** At 0B03, `cmp byte [bx],FFh` is followed by an
  unconditional `jmp 0B22` (`EB 1A` at 0B06). That skips the original comparison loop at 0B08–0B20: it decodes
  each answer byte (+14h) and compares it with the input; the first failure re-asks (`inc [6EC2]`), and the
  second goes to 0B31, the "stolen Vette" path (notes 04). **Any answer is accepted.** It looks like a patched `je`.
  `Game/VETTE.EXE` is the unmodified file as distributed.
- Epilogue 0B22: restores the registers, sets `DS:2AEC = 1` (passed), RETF.
- Because the question comes from the live PIT count, it depends on exact timing. `vette_run`'s fixed
  schedule always asks question 18 (the Twin Turbo wheelbase).

## For the port: "skip manual check" (implemented)
`game::install_skip_manual_check` (`src/game/options.cpp`) hooks 4160:0A4E, right after the `random` call
returns. The RNG therefore still runs at its original moment and leaves the original seed. The hook stores
the question index, then runs the epilogue, so only the screen is removed. Verified headlessly: with and
without the skip, `cs:8D62`, `0ACB:6EC0` and `DS:2AEC` are identical at the race start (25857, 18, 1).
- The game (`vette2026`) skips by default; `--manual-check` shows the question.
- `vette_run` skips only with `--skip-manual-check`, so key scripts that type an answer keep working.
- The player's files are never modified.
