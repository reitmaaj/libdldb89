# BDD scenarios

## SCENARIO D1: recursive materialization

GIVEN `parent("alice","bob")`, `parent("bob","carol")` and the transitive
closure rules
WHEN loaded and queried for `ancestor("alice", X)`
THEN two rows (`bob`, `carol`) are returned.

## SCENARIO D2: typed scalars

GIVEN facts `n(42)` and `n("42")`
WHEN `?- n(X).` is queried
THEN two rows are returned, and `?- n(42).`/`?- n("42").` are both true while
`?- n(43).` is false.

## SCENARIO D3: canonical dump

GIVEN a loaded database
WHEN dumped
THEN canonical fact and rule clauses appear, sorted by bytes, and excluded
derived facts are absent.

## SCENARIO D4: persistence

GIVEN a committed database
WHEN closed and reopened
THEN the same queries succeed and the generation is preserved.

## SCENARIO D5: rule replacement

GIVEN a transitive-closure database
WHEN rules are replaced with only the direct rule
THEN previously derived transitive tuples disappear after evaluation.

## SCENARIO D6: atomic rejection

GIVEN a loaded database
WHEN an invalid DLX program is loaded
THEN `DLDB89_EPROGRAM` is returned, the generation is unchanged, and prior data
still queries.
