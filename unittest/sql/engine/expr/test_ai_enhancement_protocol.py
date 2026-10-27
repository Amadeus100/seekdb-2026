"""AI SQL protocol regression against an actual seekdb and the local fixture.

SEEKDB_HOST/SEEKDB_PORT/SEEKDB_USER/SEEKDB_PASSWORD select a disposable test
instance. Requires pymysql. Run as a script from any working directory.
"""
import importlib.util
import json
import os
from pathlib import Path
import threading
import unittest
import uuid
import pymysql

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('enhancement_mock', ROOT /
    'tools/deploy/mysql_test/test_suite/ai_function/include/enhancement_mock.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class AIProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = fixture.ThreadingHTTPServer(('127.0.0.1', 0), fixture.Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.db = pymysql.connect(host=os.getenv('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.getenv('SEEKDB_PORT', '2884')), user=os.getenv('SEEKDB_USER', 'root'),
            password=os.getenv('SEEKDB_PASSWORD', ''), database='test', autocommit=True,
            charset='utf8mb4', read_timeout=120)
        cls.prefix = 'ai_protocol_' + uuid.uuid4().hex[:8]
        cls.models = []

    @classmethod
    def tearDownClass(cls):
        with cls.db.cursor() as c:
            c.execute('DROP TABLE IF EXISTS `' + cls.prefix + '`')
            for model in cls.models:
                c.execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)', (model + '_ep',))
                c.execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)', (model,))
        cls.db.close()
        cls.server.shutdown()
        cls.server.server_close()

    def query(self, sql, args=None):
        with self.db.cursor() as c:
            c.execute(sql, args)
            return c.fetchall()

    def model(self, kind, path, provider='openai'):
        name = self.prefix + '_' + str(len(self.models))
        self.query('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                   (name, json.dumps({'type': kind, 'model_name': 'fixture-model'})))
        self.query('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
            (name + '_ep', json.dumps({'ai_model_name': name, 'provider': provider,
             'access_key': '' if provider.lower() == 'ollama' else 'fixture-key',
             'url': f'http://127.0.0.1:{self.server.server_port}/{path}'})))
        self.models.append(name)
        return name

    def clear(self):
        with fixture.lock:
            fixture.events.clear()

    def requests(self):
        with fixture.lock:
            return [dict(e['body']) for e in fixture.events]

    def test_completion_prompt_and_longtext(self):
        model = self.model('completion', 'v1/chat/completions')
        self.assertEqual(self.query("SELECT AI_COMPLETE(%s, AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1,true), JSON_OBJECT('k',2)), JSON_OBJECT('temperature',0.2,'max_tokens',16))", (model,))[0][0],
                         'reply:[1, true]/[1, true]/{"k": 2}')
        body = self.requests()[-1]
        self.assertAlmostEqual(body['temperature'], 0.2)
        self.assertEqual(body['max_tokens'], 16)
        text = '你好\\"\n' * 15000
        self.query('CREATE TABLE `' + self.prefix + '` (id INT PRIMARY KEY, body LONGTEXT)')
        try:
            self.query('INSERT INTO `' + self.prefix + '` VALUES(1,%s)', (text,))
            self.assertEqual(self.query('SELECT AI_COMPLETE(%s,body,NULL) FROM `' + self.prefix + '`', (model,))[0][0], 'reply:' + text)
        finally:
            self.query('DROP TABLE `' + self.prefix + '`')
        for template in ['{3}', '{-1}', '{99999999999999999999999999}']:
            with self.subTest(template=template), self.assertRaises(pymysql.MySQLError):
                self.query('SELECT AI_COMPLETE(%s,AI_PROMPT(%s,\'a\'))', (model, template))
        self.assertEqual(self.query("SELECT AI_COMPLETE(%s,AI_PROMPT('literal {name}: {0}',AI_PROMPT('<{0}>','你好')))", (model,))[0][0], 'reply:literal {name}: <你好>')
        bad = self.model('completion', 'empty/v1/chat/completions')
        with self.assertRaises(pymysql.MySQLError):
            self.query('SELECT AI_COMPLETE(%s,\'hello\')', (bad,))

    def test_embed_grouping_null_filter_and_limit(self):
        models = [self.model('dense_embedding', 'v1/embeddings'),
                  self.model('dense_embedding', 'v1/embeddings')]
        self.query('CREATE TABLE `' + self.prefix + '` (id INT PRIMARY KEY, body LONGTEXT, model VARCHAR(100), dim INT)')
        rows = [(i, None if i % 7 == 0 else 'item-' + str(i), models[i % 2], 4 if i % 3 else 8) for i in range(270)]
        try:
            with self.db.cursor() as c:
                c.executemany('INSERT INTO `' + self.prefix + '` VALUES(%s,%s,%s,%s)', rows)
            self.clear()
            result = self.query('SELECT id,AI_EMBED(model,body,dim) FROM `' + self.prefix + '` WHERE id>=10 AND id<260 ORDER BY id')
            self.assertEqual(len(result), 250)
            for i, value in result:
                if rows[i][1] is None:
                    self.assertIsNone(value)
                else:
                    vector = json.loads(value)
                    self.assertEqual(len(vector), rows[i][3])
                    self.assertEqual(vector[0], float(sum(rows[i][1].encode()) % 997))
            requests = self.requests()
            observed = [s for r in requests for s in r['input']]
            self.assertCountEqual(observed, [r[1] for r in rows[10:260] if r[1] is not None])
            self.assertTrue(any(len(r['input']) > 1 for r in requests), requests)
            self.assertTrue(all(len(r['input']) <= 128 for r in requests))
            self.clear()
            result = self.query('SELECT id,AI_EMBED(model,body,dim) FROM `' + self.prefix + '` WHERE id>=10 LIMIT 3')
            expected = [rows[i][1] for i, _ in result if rows[i][1] is not None]
            self.assertCountEqual([s for r in self.requests() for s in r['input']], expected)
            for dim in [0, -1, 1.5, None]:
                with self.subTest(dim=dim), self.assertRaises(pymysql.MySQLError):
                    self.query('SELECT AI_EMBED(%s,\'bad\',%s)', (models[0], dim))
        finally:
            self.query('DROP TABLE `' + self.prefix + '`')

    def test_embed_response_failures(self):
        self.query('CREATE TABLE `' + self.prefix + '` (id INT PRIMARY KEY, body TEXT)')
        try:
            self.query('INSERT INTO `' + self.prefix + '` VALUES(1,\'one\'),(2,\'two\')')
            for path in ['bad-dimension', 'bad-element', 'duplicate-index', 'fail']:
                model = self.model('dense_embedding', path + '/v1/embeddings')
                with self.subTest(path=path), self.assertRaises(pymysql.MySQLError):
                    self.query('SELECT AI_EMBED(%s,body,4) FROM `' + self.prefix + '`', (model,))
        finally:
            self.query('DROP TABLE `' + self.prefix + '`')

    def test_rerank_global_order_and_ids(self):
        model = self.model('rerank', 'v1/rerank', 'siliconflow')
        docs = [{'id': str(i), 'body': ('apple' if i % 3 == 0 else 'pear')} for i in range(65)]
        result = json.loads(self.query('SELECT AI_RERANK(%s,\'apple\',%s,\'body\')', (model, json.dumps(docs)))[0][0])
        self.assertEqual([d['id'] for d in result], [str(i) for i in range(65) if i % 3 == 0] + [str(i) for i in range(65) if i % 3 != 0])
        self.assertEqual(len(result), 65)
        self.assertEqual(result[0]['_model_score'], 1)
        result = json.loads(self.query('SELECT AI_RERANK(%s,\'apple\',%s,NULL)', (model, json.dumps(['pear','apple','pear'])))[0][0])
        self.assertEqual([d['index'] for d in result], [1,0,2])

    def test_ollama_alter_and_native_protocol(self):
        model = self.model('completion', 'api/generate', 'ollama')
        for provider in ['ollama', 'Ollama', 'OLLAMA']:
            self.query('CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)', (model + '_ep', json.dumps({'provider': provider})))
            self.assertEqual(self.query('SELECT AI_COMPLETE(%s,\'hello\',NULL)', (model,))[0][0], 'reply:hello')
            self.assertIs(self.requests()[-1]['stream'], False)
        model = self.model('dense_embedding', 'api/embed', 'OLLAMA')
        self.assertEqual(len(json.loads(self.query('SELECT AI_EMBED(%s,\'hello\',4)', (model,))[0][0])), 4)


if __name__ == '__main__':
    unittest.main(verbosity=2)
