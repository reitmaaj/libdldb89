# Stories

## S1: persistent program

AS a Datalog user
I WANT to load facts and rules from DLX and query the derived relations after
reopening the process
SO THAT my inference is durable without an external engine.

## S2: safe mutation

AS a database operator
I WANT every mutation to either commit a complete fixed point or leave the
previous generation untouched
SO THAT concurrent readers never observe a partial or stale derivation.

## S3: typed values

AS a user
I WANT `42` and `"42"` to be distinct values
SO THAT type confusion cannot silently change query results.

## S4: faithful export

AS a user
I WANT `dump` to reproduce a reloadable program of asserted facts and rules
SO THAT I can move or version a database as canonical DLX.
