"""Parser for capture schedules (see race.schedule for the format)."""
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Action:
    at: float | None      # seconds after the latest sync; None = immediately
    verb: str
    args: list[str]
    ours: float | None    # vette_run emulated seconds for the same event, if given
    line: int


VERBS = {'sync': 2, 'key': None, 'down': 1, 'up': 1, 'shot': 1, 'patch': 3}


def parse(path):
    actions = []
    for n, raw in enumerate(Path(path).read_text().splitlines(), 1):
        fields = raw.split('#', 1)[0].split()
        if not fields:
            continue
        ours = [f for f in fields if f.startswith('ours=')]
        fields = [f for f in fields if not f.startswith('ours=')]
        if len(fields) < 2 or fields[1] not in VERBS:
            raise ValueError(f'{path}:{n}: expected "<t> <{"|".join(VERBS)}> ...": {raw.strip()}')
        verb, args = fields[1], fields[2:]
        want = VERBS[verb]
        if (want is not None and len(args) != want) or (want is None and not args):
            raise ValueError(f'{path}:{n}: wrong number of arguments for {verb}: {raw.strip()}')
        actions.append(Action(None if fields[0] == '-' else float(fields[0]), verb, args,
                              float(ours[0][5:]) if ours else None, n))
    return actions
