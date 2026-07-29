#!/bin/sh
#
# Standalone SQLite runtime self-test for the LXP Linux personality on target.
# Run inside the target shell, or pipe it to `ssh target sh -s`.
#
# Extension groups intentionally use fresh sqlite3 processes: combining them
# exceeds the configured FDPIC process region even though every group passes
# independently. The suite uses the default rollback journal. WAL currently
# wedges the guest, and VACUUM currently exhausts the process region even for
# an 8 KiB database; those remain failing probes rather than required passes.

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
SELECT 'scalar-pass',
	json_extract('{"n":42}', '$.n'),
	json_array_length('[1,2,3]'),
	sqrt(81.0),
	abs(sin(0.0)) < 0.000001;
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
WITH RECURSIVE fib(n, a, b) AS (
	VALUES(0, 0, 1)
	UNION ALL SELECT n + 1, b, a + b FROM fib WHERE n < 20
)
SELECT 'cte-pass', a FROM fib WHERE n = 20;
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
WITH input(v) AS (VALUES(30), (10), (20)),
ranked AS (
	SELECT v, row_number() OVER (ORDER BY v) AS rn FROM input
)
SELECT 'window-pass', SUM(v * rn) FROM ranked;
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
CREATE TABLE indexed(id INTEGER PRIMARY KEY, key TEXT NOT NULL, value INTEGER);
CREATE INDEX indexed_key ON indexed(key);
INSERT INTO indexed(key, value) VALUES('a', 1), ('b', 2), ('c', 3);
EXPLAIN QUERY PLAN SELECT value FROM indexed WHERE key = 'b';
SELECT 'index-pass', value FROM indexed WHERE key = 'b';
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
CREATE VIRTUAL TABLE docs USING fts4(body);
INSERT INTO docs VALUES
	('embedded sqlite runtime test'),
	('sqlite full text search'),
	('unrelated row');
SELECT 'fts4-pass', COUNT(*) FROM docs WHERE docs MATCH 'sqlite';
SQL

sqlite3 :memory: <<'SQL' || exit 1
.bail on
CREATE VIRTUAL TABLE boxes USING rtree(id, x1, x2, y1, y2);
INSERT INTO boxes VALUES
	(1, 0, 10, 0, 10),
	(2, 20, 30, 20, 30),
	(3, 5, 15, 5, 15);
SELECT 'rtree-pass', COUNT(*) FROM boxes
WHERE x2 >= 8 AND x1 <= 12 AND y2 >= 8 AND y1 <= 12;
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
