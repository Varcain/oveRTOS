#!/bin/sh
#
# Standalone SQLite runtime self-test for the LXP Linux personality on target.
# Run inside the target shell, or pipe it to `ssh target sh -s`.
#
# The FDPIC build intentionally omits WAL because the Linux personality does
# not provide file-backed shared mmap or POSIX record locks. Verify that a WAL
# request safely remains in rollback-journal mode. Extension groups run in one
# sqlite3 process and VACUUM is required to pass, guarding the bounded FDPIC
# memory configuration.

rm -f /tmp/sqlite-runtime-selftest.db \
	/tmp/sqlite-runtime-selftest.db-journal \
	/tmp/sqlite-runtime-selftest.db-wal \
	/tmp/sqlite-runtime-selftest.db-shm \
	/tmp/sqlite-runtime-selftest-aux.db

sqlite3 :memory: <<'SQL' || exit 1
.bail on
PRAGMA foreign_keys = ON;
CREATE TABLE parent(
	id INTEGER PRIMARY KEY,
	name TEXT NOT NULL UNIQUE,
	value INTEGER NOT NULL CHECK(value >= 0),
	data BLOB NOT NULL
) STRICT;
CREATE TABLE child(
	id INTEGER PRIMARY KEY,
	parent_id INTEGER NOT NULL REFERENCES parent(id) ON DELETE CASCADE,
	amount INTEGER NOT NULL
) STRICT;
CREATE TABLE audit(event TEXT NOT NULL) STRICT;
CREATE TRIGGER child_insert AFTER INSERT ON child BEGIN
	INSERT INTO audit VALUES('child:' || NEW.id);
END;
CREATE TEMP TABLE assertion(
	name TEXT PRIMARY KEY,
	ok INTEGER NOT NULL CHECK(ok = 1)
) STRICT;
BEGIN IMMEDIATE;
INSERT INTO parent VALUES(1, 'alpha', 10, X'00FF10');
INSERT INTO parent VALUES(2, 'beta', 20, X'DEADBEEF');
INSERT INTO child(parent_id, amount) VALUES(1, 3), (1, 4), (2, 5);
COMMIT;
INSERT INTO assertion SELECT 'rows', COUNT(*) = 3 FROM child;
INSERT INTO assertion SELECT 'trigger', COUNT(*) = 3 FROM audit;
INSERT INTO assertion SELECT 'join', SUM(amount) = 7
FROM child JOIN parent ON parent.id = child.parent_id
WHERE parent.name = 'alpha';
INSERT INTO assertion SELECT 'blob', hex(data) = '00FF10'
FROM parent WHERE id = 1;
INSERT INTO assertion SELECT 'types',
	typeof(id) = 'integer' AND typeof(name) = 'text'
	AND typeof(value) = 'integer' AND typeof(data) = 'blob'
FROM parent WHERE id = 2;
INSERT INTO assertion SELECT 'foreign-key-check', COUNT(*) = 0
FROM pragma_foreign_key_check;
INSERT OR IGNORE INTO parent VALUES(3, 'bad', -1, X'00');
INSERT INTO assertion SELECT 'check-constraint', COUNT(*) = 0
FROM parent WHERE id = 3;
INSERT OR IGNORE INTO parent VALUES(3, 'alpha', 30, X'00');
INSERT INTO assertion SELECT 'unique-constraint', COUNT(*) = 1
FROM parent WHERE name = 'alpha';
SAVEPOINT s;
UPDATE parent SET value = 999 WHERE id = 1;
ROLLBACK TO s;
RELEASE s;
INSERT INTO assertion SELECT 'savepoint', value = 10 FROM parent WHERE id = 1;
INSERT INTO parent VALUES(1, 'alpha', 0, X'00')
ON CONFLICT(id) DO UPDATE SET value = parent.value + 5;
INSERT INTO assertion SELECT 'upsert', value = 15 FROM parent WHERE id = 1;
DELETE FROM parent WHERE id = 2;
INSERT INTO assertion SELECT 'cascade', COUNT(*) = 0
FROM child WHERE parent_id = 2;
INSERT INTO assertion
SELECT 'integrity', COUNT(*) = 1 AND MIN(integrity_check) = 'ok'
FROM pragma_integrity_check;
SELECT 'core-pass', COUNT(*) FROM assertion;
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
CREATE TEMP TABLE assertion(
	name TEXT PRIMARY KEY,
	ok INTEGER NOT NULL CHECK(ok = 1)
) STRICT;
INSERT INTO assertion VALUES(
	'scalar',
	json_extract('{"n":42}', '$.n') = 42
	AND json_array_length('[1,2,3]') = 3
	AND sqrt(81.0) = 9.0
	AND abs(sin(0.0)) < 0.000001
);
WITH RECURSIVE fib(n, a, b) AS (
	VALUES(0, 0, 1)
	UNION ALL SELECT n + 1, b, a + b FROM fib WHERE n < 20
)
INSERT INTO assertion
SELECT 'cte', a = 6765 FROM fib WHERE n = 20;
WITH input(v) AS (VALUES(30), (10), (20)),
ranked AS (
	SELECT v, row_number() OVER (ORDER BY v) AS rn FROM input
)
INSERT INTO assertion
SELECT 'window', SUM(v * rn) = 140 FROM ranked;
CREATE TABLE indexed(id INTEGER PRIMARY KEY, key TEXT NOT NULL, value INTEGER);
CREATE INDEX indexed_key ON indexed(key);
INSERT INTO indexed(key, value) VALUES('a', 1), ('b', 2), ('c', 3);
INSERT INTO assertion
SELECT 'index', value = 2 FROM indexed WHERE key = 'b';
CREATE VIRTUAL TABLE docs USING fts4(body);
INSERT INTO docs VALUES
	('embedded sqlite runtime test'),
	('sqlite full text search'),
	('unrelated row');
INSERT INTO assertion
SELECT 'fts4', COUNT(*) = 2 FROM docs WHERE docs MATCH 'sqlite';
CREATE VIRTUAL TABLE boxes USING rtree(id, x1, x2, y1, y2);
INSERT INTO boxes VALUES
	(1, 0, 10, 0, 10),
	(2, 20, 30, 20, 30),
	(3, 5, 15, 5, 15);
INSERT INTO assertion
SELECT 'rtree', COUNT(*) = 2 FROM boxes
WHERE x2 >= 8 AND x1 <= 12 AND y2 >= 8 AND y1 <= 12;
SELECT 'combined-extension-pass', COUNT(*) FROM assertion;
SQL

sqlite3 /tmp/sqlite-runtime-selftest.db <<'SQL' || exit 1
.bail on
PRAGMA journal_mode = DELETE;
PRAGMA synchronous = FULL;
CREATE TABLE persisted(
	id INTEGER PRIMARY KEY,
	value TEXT NOT NULL
) STRICT;
INSERT INTO persisted(value) VALUES('first');
SELECT 'file-create-pass', COUNT(*) FROM persisted;
PRAGMA integrity_check;
SQL

wal_mode=$(
	sqlite3 -batch -noheader -list /tmp/sqlite-runtime-selftest.db \
		'PRAGMA journal_mode = WAL;'
) || exit 1
if [ "$wal_mode" != "delete" ]; then
	echo "unexpected FDPIC journal mode: $wal_mode" >&2
	exit 1
fi
echo wal-fallback-pass

sqlite3 /tmp/sqlite-runtime-selftest.db <<'SQL' || exit 1
.bail on
SELECT 'file-reopen-read-pass', COUNT(*), MIN(value), MAX(value)
FROM persisted;
INSERT INTO persisted(value) VALUES('second');
SELECT 'file-reopen-write-pass', COUNT(*) FROM persisted;
PRAGMA integrity_check;
SQL

sqlite3 /tmp/sqlite-runtime-selftest.db <<'SQL' || exit 1
.bail on
ATTACH DATABASE '/tmp/sqlite-runtime-selftest-aux.db' AS aux;
CREATE TABLE aux.kv(key TEXT PRIMARY KEY, value INTEGER NOT NULL) STRICT;
INSERT INTO aux.kv VALUES('answer', 42);
SELECT 'attach-pass', value FROM aux.kv WHERE key = 'answer';
DETACH DATABASE aux;
SQL

sqlite3 /tmp/sqlite-runtime-selftest.db <<'SQL' || exit 1
.bail on
CREATE TABLE vacuum_payload(
	id INTEGER PRIMARY KEY,
	value TEXT NOT NULL
) STRICT;
WITH RECURSIVE n(value) AS (
	VALUES(1)
	UNION ALL SELECT value + 1 FROM n WHERE value < 100
)
INSERT INTO vacuum_payload
SELECT value, printf('row-%04d', value) FROM n;
VACUUM;
SELECT 'vacuum-pass', COUNT(*) FROM vacuum_payload;
PRAGMA integrity_check;
SQL

sqlite3 /tmp/sqlite-runtime-selftest.db <<'SQL' || exit 1
.bail on
SELECT 'file-final-reopen-pass', COUNT(*), group_concat(value, ',')
FROM persisted;
PRAGMA integrity_check;
SQL

rm -f /tmp/sqlite-runtime-selftest.db \
	/tmp/sqlite-runtime-selftest.db-journal \
	/tmp/sqlite-runtime-selftest.db-wal \
	/tmp/sqlite-runtime-selftest.db-shm \
	/tmp/sqlite-runtime-selftest-aux.db

echo sqlite-runtime-selftest-pass
