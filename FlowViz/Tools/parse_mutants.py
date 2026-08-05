#!/usr/bin/env python3
"""Parse a mutant file into TSV records for mutate.sh.

Reads the mutant file named on argv, writes one TSV line per record to
STDOUT. Writing to stdout rather than a path is the point: mutate.sh used a
fixed /tmp filename for this, and two campaigns running at once overwrote each
other's mutant list. That is not hypothetical -- a payload campaign was seen
scoring a mutant belonging to an entirely different source file, printed under
that other campaign's name. It surfaced as a harmless SKIP only because the
foreign pattern did not match; a pattern that did match would have been
applied, built, and scored under the wrong mutant's name.

Format: records separated by a line of `%%`, fields within a record by a line
of `--`:

    name
    --
    text to find
    --
    text to replace it with
    %%
    ...

Each field is JSON-encoded on output so that newlines, tabs and quotes survive
the trip through `read`.
"""

import json
import sys


def parse(text):
    """Yield (name, from, to) triples. Raises ValueError on a malformed record."""
    for record in text.split('\n%%\n'):
        if not record.strip():
            continue
        parts = record.split('\n--\n')
        if len(parts) != 3:
            raise ValueError(
                "a record must have exactly 3 fields separated by '--', got "
                "%d:\n%s" % (len(parts), record[:200]))
        name, frm, to = (p.strip('\n') for p in parts)
        # Only the name is stripped of surrounding whitespace. from/to are
        # source text whose leading indentation is load-bearing -- stripping it
        # would make every mutant match the wrong indentation level, or nothing
        # at all.
        yield name.strip(), frm, to


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: parse_mutants.py <mutants-file>\n")
        return 2
    with open(argv[1], encoding='utf-8') as handle:
        text = handle.read()
    try:
        records = list(parse(text))
    except ValueError as error:
        sys.stderr.write("malformed mutant file %s: %s\n" % (argv[1], error))
        return 2
    if not records:
        sys.stderr.write("no mutants parsed from %s\n" % argv[1])
        return 2
    for record in records:
        sys.stdout.write('\t'.join(json.dumps(field) for field in record) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
