# MyPostgreSQLite

A diskbacked SQL query engine. The interactive shell connects the SQL front end,
optimizer, execution engine, and storage engine.

## Build and run

```sh
make -j4
./build/vql --db ./db --frames 32
```

`--db` just defaults to `db` and the directory and catalog are created if needed
Tables and statistics stay available even when you restart the program
`--frames` defaults to 32 and has to be at least 4

## Shell commands

```sql
CREATE TABLE R (id INT, label VARCHAR(40));
INSERT INTO R VALUES (1, 'first');
LOAD R FROM 'data/r.csv';
ANALYZE R;
SELECT * FROM R ORDER BY id;
EXPLAIN SELECT id FROM R WHERE id > 10;
EXPLAIN ANALYZE SELECT id FROM R WHERE id > 10;
SET frames = 4;
HELP;
QUIT;
-- THIS IS A COMMENT
```

CSV files have no header and one record per line
Quote fields that contain commas with double quotes

Invalid fields stop loading and report the line and column in an error
Earlier valid rows remain loaded since loading isnt a transaction guy

Paths with spaces must be quoted and paths are relative to the current directory

## Measurements

All the queries report result row count, page reads/writes, elapsed milliseconds, estimated root rows, and actual root rows

- Cached pages can produce zero disk reads. And planinng I/O is excluded.
- Time measurements dont count parsing, analysis, and optimization

## Shell Stuff and Things

The shell also accepts redirected input with no prompt
Any command error makes that run return status 1 but if you have later commands they will still run
Incomplete commands at EOF are errors

## Tests and standalone tools
