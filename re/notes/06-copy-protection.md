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

## Open
- How the question is chosen (RNG seed source: BIOS ticks / DOS time?) and how input is compared.
  Find the routine that reads the offset table.
- What happens on a wrong answer (the manual suggests the race runs with restrictions or the game
  quits).

## For the port
The player supplies their own game files, so an **optional "skip manual check"** setting is
reasonable. It would be off in strict Classic mode. Implement it as a code hook on the
quiz routine once that routine is identified, never by patching the player's files.
