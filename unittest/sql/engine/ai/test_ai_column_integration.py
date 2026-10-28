#!/usr/bin/env python3
"""Native SQL assertions for async AI columns; requires a running seekdb.

Uses the competition HTTP fixture, not a replacement SQL engine. Set
SEEKDB_TEST_PORT (default 2884) and optionally SEEKDB_TEST_PASSWORD.
"""
import importlib.util
import json
import os
from pathlib import Path
import time
import unittest
import uuid

import pymysql

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = ROOT / "tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py"
spec = importlib.util.spec_from_file_location("ai_column_mock", FIXTURE)
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)


class AIColumnIntegration(unittest.TestCase):
    @classmethod
    def connect(cls):
        return pymysql.connect(host="127.0.0.1", port=int(os.environ.get("SEEKDB_TEST_PORT", "2884")),
                               user="root", password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                               charset="utf8mb4", autocommit=True)

    @classmethod
    def setUpClass(cls):
        deadline = time.monotonic() + 30
        while True:
            try:
                cls.connection = cls.connect()
                break
            except pymysql.OperationalError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        cls.token = "aci_" + uuid.uuid4().hex[:8]
        cls.database = cls.token
        cls.mock_port = 19125
        mock.start_server(cls.mock_port)
        cls.models = {kind: cls.token + "_" + kind for kind in ("text", "fail", "embed")}
        with cls.connection.cursor() as q:
            q.execute("CREATE DATABASE " + cls.database)
            q.execute("USE " + cls.database)
            for kind, name in cls.models.items():
                model = {"type": "dense_embedding" if kind == "embed" else "completion", "model_name": "mock-" + kind}
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)", (name, json.dumps(model)))
                path = "/v1/embeddings" if kind == "embed" else "/v1/always-fail/completions" if kind == "fail" else "/v1/chat/completions"
                endpoint = {"ai_model_name": name, "url": "http://127.0.0.1:%d%s" % (cls.mock_port, path),
                            "access_key": "integration-key", "provider": "openai", "request_model_name": "mock-" + kind}
                q.execute("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)", (name + "_ep", json.dumps(endpoint)))

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.connection.cursor() as q:
                q.execute("ROLLBACK")
                q.execute("DROP DATABASE IF EXISTS " + cls.database)
                for name in cls.models.values():
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)", (name + "_ep",))
                    q.execute("CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)", (name,))
        finally:
            cls.connection.close()
            mock.stop_server(cls.mock_port)

    def sql(self, statement, args=None, connection=None):
        with (connection or self.connection).cursor() as q:
            q.execute(statement, args)
            return q.fetchall() if q.description else ()

    def scalar(self, statement, args=None, connection=None):
        return self.sql(statement, args, connection)[0][0]

    def wait(self, condition, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.scalar(condition):
                return
            time.sleep(0.05)
        self.fail("timed out: " + condition + "\ntasks=" + repr(self.sql(
            "SELECT table_id,column_id,state,retry_count,last_error FROM oceanbase.__all_ai_column_task")))

    def error(self, number, statement):
        with self.assertRaises(pymysql.MySQLError) as error:
            self.sql(statement)
        self.assertEqual(error.exception.args[0], number)

    def task_count(self, table):
        return self.scalar("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id="
                           "(SELECT table_id FROM oceanbase.__all_table WHERE database_id="
                           "(SELECT database_id FROM oceanbase.__all_database WHERE database_name=%s) AND table_name=%s)",
                           (self.database, table))

    def create(self, table, extra="", result=None):
        result = result or "tags JSON AI COLUMN(AI_COMPLETE('%s', CONCAT('Classify: ',content)))" % self.models["text"]
        self.sql("CREATE TABLE %s (id BIGINT PRIMARY KEY, content VARCHAR(200), title VARCHAR(40)%s, %s)" %
                 (table, extra, result))

    def test_01_ddl_and_assignment(self):
        self.create("ddl")
        rows = self.sql("SHOW AI COLUMNS FROM ddl")
        self.assertEqual(rows[0][0:2], ("tags", self.models["text"]))
        self.assertIn("AI COLUMN", self.sql("SHOW CREATE TABLE ddl")[0][1])
        self.assertEqual([r[0] for r in self.sql("DESCRIBE ddl")], ["id", "content", "title", "tags"])
        self.error(1235, "INSERT INTO ddl(id,content,tags) VALUES(1,'x','{}')")
        self.error(1235, "UPDATE ddl SET tags='{}'")
        # seekdb can persist an invalid trigger definition; executing it must
        # still reject the attempted direct AI-column assignment.
        self.sql("CREATE TRIGGER user_write_ai BEFORE INSERT ON ddl FOR EACH ROW SET NEW.tags='{}'")
        with self.assertRaises(pymysql.MySQLError):
            self.sql("INSERT INTO ddl(id,content) VALUES(7,'cannot-write-ai')")
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM ddl"), 0)
        self.sql("DROP TRIGGER user_write_ai")
        self.sql("ALTER TABLE ddl ADD score BIGINT AI COLUMN(AI_COMPLETE('%s',content))" % self.models["text"])
        self.assertEqual(len(self.sql("SHOW AI COLUMNS FROM ddl")), 2)
        self.sql("ALTER TABLE ddl MODIFY score BIGINT AI COLUMN(AI_COMPLETE('%s',CONCAT('Score: ',content)))" % self.models["text"])
        self.assertIn("Score:", self.sql("SHOW CREATE TABLE ddl")[0][1])
        self.error(7600, "CREATE TABLE bad_dimension(id INT, content TEXT, v VECTOR(4) AI COLUMN(AI_EMBED('%s',content,3)))" % self.models["embed"])
        self.error(1235, "CREATE TABLE bad_dependency(id INT, content TEXT, a TEXT AI COLUMN(AI_COMPLETE('%s',content)), b TEXT AI COLUMN(AI_COMPLETE('%s',a)))" % (self.models["text"], self.models["text"]))
        self.sql("DROP TABLE ddl")

    def test_02_transaction_group_and_dml(self):
        self.create("transactions", result="tags JSON AI COLUMN(AI_COMPLETE('%s',CONCAT('Classify: ',content))), "
                    "score BIGINT AI COLUMN(AI_COMPLETE('%s',CONCAT('Score: ',content)))" % (self.models["text"], self.models["text"]))
        before = len(mock.read_events(self.mock_port))
        self.sql("START TRANSACTION")
        self.sql("INSERT INTO transactions(id,content,title) VALUES(1,'rollback-value','old')")
        self.assertEqual(self.sql("SELECT tags,score FROM transactions"), ((None, None),))
        self.assertEqual(self.task_count("transactions"), 2)
        observer = self.connect()
        try:
            self.assertEqual(self.scalar("SELECT COUNT(*) FROM %s.transactions" % self.database, connection=observer), 0)
            time.sleep(0.3)
            self.assertEqual(len(mock.read_events(self.mock_port)), before)
        finally:
            observer.close()
        self.sql("ROLLBACK")
        self.assertEqual(self.task_count("transactions"), 0)
        self.sql("INSERT INTO transactions(id,content,title) VALUES(1,'initial','old')")
        deadline = time.monotonic() + 20
        while True:
            tags, score = self.sql("SELECT tags,score FROM transactions WHERE id=1")[0]
            self.assertEqual(tags is None, score is None, "partial group publication")
            if tags is not None:
                self.assertEqual(score, 7)
                self.assertEqual(json.loads(tags)["input"], "Classify: initial")
                break
            if time.monotonic() > deadline:
                self.fail("group did not finish; tasks=" + repr(self.sql("SELECT * FROM oceanbase.__all_ai_column_task")))
            time.sleep(0.02)
        self.wait("SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task")
        before = len(mock.read_events(self.mock_port))
        self.sql("UPDATE transactions SET title='new' WHERE id=1")
        time.sleep(0.3)
        self.assertEqual(len(mock.read_events(self.mock_port)), before)
        self.sql("START TRANSACTION")
        self.sql("UPDATE transactions SET content='rollback-value' WHERE id=1")
        self.assertEqual(self.sql("SELECT tags,score FROM transactions"), ((None, None),))
        self.assertEqual(self.task_count("transactions"), 2)
        self.sql("ROLLBACK")
        self.assertEqual(self.task_count("transactions"), 0)
        self.assertEqual(self.scalar("SELECT content FROM transactions"), "initial")
        self.sql("START TRANSACTION")
        self.sql("UPDATE transactions SET id=2,content='changed' WHERE id=1")
        self.assertEqual(self.scalar("SELECT COUNT(DISTINCT request_id) FROM oceanbase.__all_ai_column_task"), 1)
        self.assertEqual(self.scalar("SELECT MIN(rowkey_predicate) FROM oceanbase.__all_ai_column_task"), "`id` = 2")
        self.sql("COMMIT")
        self.wait("SELECT score=7 AND tags IS NOT NULL FROM transactions WHERE id=2")
        self.assertEqual(json.loads(self.scalar("SELECT tags FROM transactions"))["input"], "Classify: changed")
        self.sql("START TRANSACTION")
        self.sql("INSERT INTO transactions(id,content) VALUES(10,'batch-a'),(11,'batch-b')")
        self.assertEqual(self.task_count("transactions"), 4)
        self.sql("DELETE FROM transactions WHERE id IN (10,11)")
        self.assertEqual(self.task_count("transactions"), 0)
        self.sql("COMMIT")
        self.sql("DROP TABLE transactions")
        events = mock.read_events(self.mock_port)[before:]
        self.assertFalse(any("rollback-value" in e["user"] for e in events))

    def test_03_stale_requests_and_delete(self):
        self.create("stale")
        before = len(mock.read_events(self.mock_port))
        self.sql("INSERT INTO stale(id,content) VALUES(1,'slow-old')")
        time.sleep(0.4)
        self.sql("UPDATE stale SET content='fast-new' WHERE id=1")
        self.wait("SELECT tags IS NOT NULL FROM stale WHERE id=1")
        self.assertEqual(json.loads(self.scalar("SELECT tags FROM stale"))["input"], "Classify: fast-new")
        time.sleep(1.3)
        self.assertEqual(json.loads(self.scalar("SELECT tags FROM stale"))["input"], "Classify: fast-new")
        events = mock.read_events(self.mock_port)[before:]
        self.assertTrue(any("slow-old" in e["user"] for e in events))
        self.sql("UPDATE stale SET content='slow-old-delete' WHERE id=1")
        time.sleep(0.3)
        self.sql("DELETE FROM stale WHERE id=1")
        time.sleep(1.3)
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM stale"), 0)
        self.assertEqual(self.task_count("stale"), 0)
        self.sql("DROP TABLE stale")

    def test_04_retry_and_strict(self):
        self.create("retry_docs", extra=", status VARCHAR(20), category VARCHAR(20)",
                    result="result VARCHAR(400) AI COLUMN(AI_COMPLETE('%s',content)) WITH(max_retries=0), "
                    "label VARCHAR(400) AI COLUMN(AI_COMPLETE('%s',CONCAT('Label: ',content))) WITH(max_retries=0)" %
                    (self.models["text"], self.models["text"]))
        self.sql("INSERT INTO retry_docs(id,content,status,category) VALUES(1,'row-one','active','news'),(2,'row-two','inactive','sports')")
        self.wait("SELECT COUNT(*)=2 FROM retry_docs WHERE result IS NOT NULL AND label IS NOT NULL")
        self.sql("PREPARE strict_probe FROM 'SELECT result FROM retry_docs WHERE id=2'")
        mock.stop_server(self.mock_port)
        self.sql("ALTER TABLE retry_docs RETRY AI COLUMN result WHERE id=1")
        self.sql("SET SESSION ai_column_strict_read=ON")
        self.assertEqual(self.scalar("SELECT COUNT(*) FROM retry_docs WHERE label IS NOT NULL"), 2)
        self.assertEqual(self.scalar("SELECT COUNT(id) FROM retry_docs"), 2)
        self.error(11120, "EXECUTE strict_probe")
        self.sql("DEALLOCATE PREPARE strict_probe")
        for sql in ("SELECT result FROM retry_docs WHERE id=2", "SELECT * FROM retry_docs WHERE id=2",
                    "SELECT id FROM retry_docs WHERE result IS NULL",
                    "UPDATE retry_docs SET category=category WHERE result IS NULL",
                    "DELETE FROM retry_docs WHERE result IS NULL"):
            self.error(11120, sql)
        self.sql("SET SESSION ai_column_strict_read=OFF")
        self.wait("SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task")
        self.assertIsNone(self.scalar("SELECT result FROM retry_docs WHERE id=1"))
        self.sql("ALTER TABLE retry_docs RETRY AI COLUMN result WHERE status='FAILED' AND category='news'")
        mock.start_server(self.mock_port)
        self.sql("ALTER TABLE retry_docs RETRY AI COLUMN result")
        self.wait("SELECT COUNT(*)=2 FROM retry_docs WHERE result IS NOT NULL")
        self.sql("SET SESSION ai_column_strict_read=ON")
        self.assertEqual(self.scalar("SELECT COUNT(result) FROM retry_docs"), 2)
        self.sql("SET SESSION ai_column_strict_read=OFF")
        self.sql("ALTER TABLE retry_docs RETRY AI COLUMN result WHERE status='READY' AND id=999")
        self.assertEqual(self.task_count("retry_docs"), 0)
        mock.stop_server(self.mock_port)
        self.sql("ALTER TABLE retry_docs RETRY AI COLUMN result WHERE `status`='active'")
        self.assertEqual(self.sql("SELECT id,result IS NULL,label IS NOT NULL FROM retry_docs ORDER BY id"), ((1, 1, 1), (2, 0, 1)))
        self.error(1235, "ALTER TABLE retry_docs RETRY AI COLUMN content")
        self.error(1235, "ALTER TABLE retry_docs RETRY AI COLUMN result, ADD other INT")
        self.sql("DROP TABLE retry_docs")
        mock.start_server(self.mock_port)
        self.create("auto_retry", result="result TEXT AI COLUMN(AI_COMPLETE('%s',content)) WITH(max_retries=2)" % self.models["text"])
        self.sql("INSERT INTO auto_retry(id,content) VALUES(1,'retry-once')")
        self.wait("SELECT result IS NOT NULL FROM auto_retry")
        retry_events = [e for e in mock.read_events(self.mock_port) if e["user"] == "retry-once"]
        self.assertEqual([e["status"] for e in retry_events], [400, 200])
        self.sql("DROP TABLE auto_retry")

    def test_05_vector_and_rollback(self):
        self.create("vectors", result="emb VECTOR(4) AI COLUMN(AI_EMBED('%s',content))" % self.models["embed"])
        self.sql("INSERT INTO vectors(id,content) VALUES(1,'vec-zero'),(2,'vec-ten'),(3,'vec-twenty')")
        self.wait("SELECT COUNT(*)=3 FROM vectors WHERE emb IS NOT NULL")
        self.sql("CREATE VECTOR INDEX idx_aci_emb ON vectors(emb) WITH(distance=l2,type=hnsw,lib=vsag,m=10,ef_construction=12,ef_search=40)")
        ann = "SELECT id FROM vectors ORDER BY l2_distance(emb,[0,0,0,0]) APPROXIMATE LIMIT 3"
        self.assertEqual(self.sql(ann), ((1,), (2,), (3,)))
        self.sql("START TRANSACTION")
        self.sql("UPDATE vectors SET content='vec-zero' WHERE id=2")
        self.assertIsNone(self.scalar("SELECT emb FROM vectors WHERE id=2"))
        self.assertEqual({r[0] for r in self.sql(ann)}, {1, 3})
        self.sql("ROLLBACK")
        self.assertEqual(self.scalar("SELECT emb=[10,0,0,0] FROM vectors WHERE id=2"), 1)
        self.assertEqual(self.task_count("vectors"), 0)
        self.sql("UPDATE vectors SET content='vec-zero' WHERE id=2")
        self.wait("SELECT emb=[0,0,0,0] FROM vectors WHERE id=2")
        self.assertEqual({r[0] for r in self.sql(ann + " OFFSET 0")[:2]}, {1, 2})
        self.sql("DROP TABLE vectors")

    def test_06_row_identity_and_definition_changes(self):
        model = self.models["text"]
        self.sql("CREATE TABLE heap_docs(content VARCHAR(200), result TEXT AI COLUMN(AI_COMPLETE('%s',content)))" % model)
        self.sql("INSERT INTO heap_docs(content) VALUES('heap-one'),('heap-two')")
        self.wait("SELECT COUNT(*)=2 FROM heap_docs WHERE result IS NOT NULL")
        self.sql("UPDATE heap_docs SET content='heap-changed' WHERE content='heap-one'")
        self.wait("SELECT result='mock:heap-changed' FROM heap_docs WHERE content='heap-changed'")
        self.sql("DROP TABLE heap_docs")
        self.sql("CREATE TABLE composite_docs(tenant VARCHAR(30), id INT, content VARCHAR(200), PRIMARY KEY(tenant,id), "
                 "result TEXT AI COLUMN(AI_COMPLETE('%s',content)))" % model)
        self.sql("INSERT INTO composite_docs(tenant,id,content) VALUES(%s,1,'CaseSensitive')", ("队伍'\\A",))
        self.wait("SELECT result='mock:CaseSensitive' FROM composite_docs")
        self.sql("UPDATE composite_docs SET tenant=%s,content='casesensitive'", ("队伍'\\B",))
        self.wait("SELECT CAST(result AS BINARY)=CAST('mock:casesensitive' AS BINARY) FROM composite_docs")
        self.sql("DROP TABLE composite_docs")
        self.sql("CREATE TABLE alter_existing(id INT PRIMARY KEY, content VARCHAR(200))")
        self.sql("INSERT INTO alter_existing VALUES(1,'old-row')")
        self.sql("ALTER TABLE alter_existing ADD result TEXT AI COLUMN(AI_COMPLETE('%s',content))" % model)
        self.wait("SELECT result='mock:old-row' FROM alter_existing")
        before = len(mock.read_events(self.mock_port))
        self.sql("ALTER TABLE alter_existing ADD label TEXT AI COLUMN(AI_COMPLETE('%s',CONCAT('Label: ',content)))" % model)
        self.wait("SELECT label='mock:Label: old-row' FROM alter_existing")
        events = mock.read_events(self.mock_port)[before:]
        self.assertEqual([e["user"] for e in events], ["Label: old-row"])
        self.sql("ALTER TABLE alter_existing MODIFY result TEXT AI COLUMN(AI_COMPLETE('%s',CONCAT('new:',content)))" % model)
        self.wait("SELECT result='mock:new:old-row' FROM alter_existing")
        self.sql("DROP TABLE alter_existing")

    def test_07_group_final_failure(self):
        self.create("mixed_results", result="tags JSON AI COLUMN(AI_COMPLETE('%s',CONCAT('Classify: ',content))), "
                    "broken TEXT AI COLUMN(AI_COMPLETE('%s',content)) WITH(max_retries=1)" % (self.models["text"], self.models["fail"]))
        self.sql("INSERT INTO mixed_results(id,content) VALUES(1,'mixed-success-failure')")
        deadline = time.monotonic() + 20
        while True:
            tags, broken = self.sql("SELECT tags,broken FROM mixed_results")[0]
            self.assertIsNone(broken)
            if tags is not None:
                self.assertEqual(json.loads(tags)["input"], "Classify: mixed-success-failure")
                break
            if time.monotonic() > deadline:
                self.fail("mixed terminal group not published")
            time.sleep(0.02)
        self.wait("SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task")
        failed = [e for e in mock.read_events(self.mock_port) if "always-fail" in e["path"] and e["user"] == "mixed-success-failure"]
        self.assertEqual(len(failed), 2)
        self.sql("SET SESSION ai_column_strict_read=ON")
        self.assertIsNotNone(self.scalar("SELECT tags FROM mixed_results"))
        self.error(11120, "SELECT broken FROM mixed_results")
        self.sql("SET SESSION ai_column_strict_read=OFF")
        self.sql("ALTER TABLE mixed_results RETRY AI COLUMN broken WHERE status='FAILED'")
        self.wait("SELECT COUNT(*)=0 FROM oceanbase.__all_ai_column_task")
        self.sql("DROP TABLE mixed_results")


if __name__ == "__main__":
    unittest.main(verbosity=2)
