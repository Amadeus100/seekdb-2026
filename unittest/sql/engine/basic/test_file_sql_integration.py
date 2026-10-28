"""Native file SQL integration tests against a disposable seekdb instance.

Uses SEEKDB_HOST/PORT/USER/PASSWORD. The configured secure_file_priv must be
a local writable test directory; all fixtures are in a fresh child directory.
"""
import datetime
import json
import os
from pathlib import Path
import shutil
import tempfile
import subprocess
import sys
import unittest
import pymysql


class FileSQLTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = pymysql.connect(host=os.getenv('SEEKDB_HOST', '127.0.0.1'),
            port=int(os.getenv('SEEKDB_PORT', '2884')), user=os.getenv('SEEKDB_USER', 'root'),
            password=os.getenv('SEEKDB_PASSWORD', ''), database='test', autocommit=True,
            charset='utf8mb4', read_timeout=60)
        with cls.db.cursor() as c:
            c.execute('SET SESSION ob_query_timeout=120000000')
            c.execute('SELECT @@secure_file_priv')
            root = c.fetchone()[0]
            if not root or root.upper() == 'NULL':
                raise unittest.SkipTest('requires a local secure_file_priv test directory')
            cls.root = Path(root).resolve(strict=True)
            cls.dir = Path(tempfile.mkdtemp(prefix='seekdb-file-sql-test-', dir=cls.root)).resolve()
            c.execute('SET SESSION enable_file_sql=ON')
        (cls.dir / 'left.csv').write_text('id,name,score,active,day,empty\n1,Alice,98.5,true,2024-02-29,\n2,"Bob, B",87,false,,\n3,"line\nthree",90.25,true,2024-03-01,\n', encoding='utf-8')
        (cls.dir / 'right.jsonl').write_text('\n'.join(json.dumps(x) for x in [
            {'id':1,'label':'one'}, {'id':3,'label':'three'}, {'id':4,'extra':False}]) + '\n', encoding='utf-8')

    @classmethod
    def tearDownClass(cls):
        cls.db.close()
        if cls.root in cls.dir.parents and cls.dir.name.startswith('seekdb-file-sql-test-'):
            shutil.rmtree(cls.dir)

    def query(self, sql, args=None):
        with self.db.cursor() as c:
            c.execute(sql, args)
            return c.fetchall()

    def scan(self, name, kind='csv'):
        return 'read_' + kind + '(' + self.db.escape(str(self.dir / name).replace('\\','/')) + ')'

    def test_native_filter_group_join_and_rescan(self):
        left = self.scan('left.csv')
        right = self.scan('right.jsonl', 'ndjson')
        self.assertEqual(self.query(f'SELECT id,name FROM {left} f WHERE score>90 ORDER BY id DESC LIMIT 1'), ((3,'line\nthree'),))
        self.assertEqual(self.query(f'SELECT active,COUNT(*),SUM(id) FROM {left} GROUP BY active HAVING COUNT(*)>1'), ((1,2,4),))
        self.assertEqual(self.query(f'SELECT a.id,b.label FROM {left} a JOIN {right} b ON a.id=b.id ORDER BY a.id'), ((1,'one'),(3,'three')))
        self.assertEqual(self.query(f'SELECT a.id,b.label FROM {left} a LEFT JOIN {right} b ON a.id=b.id ORDER BY a.id'), ((1,'one'),(2,None),(3,'three')))
        self.assertEqual(self.query(f'SELECT a.id,b.id FROM {left} a JOIN {left} b ON a.id=b.id ORDER BY a.id'), ((1,1),(2,2),(3,3)))
        plan = '\n'.join(r[0] for r in self.query(f'EXPLAIN SELECT a.id FROM {left} a JOIN {right} b ON a.id=b.id'))
        self.assertIn('FUNCTION_TABLE', plan)
        self.assertIn('JOIN', plan)

    def test_schema_and_full_inference_after_changes(self):
        rows = self.query('DESCRIBE SELECT * FROM ' + self.scan('left.csv'))
        self.assertEqual([r[0:3] for r in rows], [('id','BIGINT','NO'),('name','VARCHAR','NO'),
            ('score','DOUBLE','NO'),('active','BOOLEAN','NO'),('day','DATE','YES'),('empty','NULL','YES')])
        p = self.dir / 'changed.csv'
        p.write_text('n\n1\n', encoding='utf-8')
        self.assertEqual(self.query('SELECT * FROM ' + self.scan(p.name)), ((1,),))
        p.write_text('n,new_field\n9223372036854775807,9\n1.5,10\n', encoding='utf-8')
        self.assertEqual(self.query('SELECT * FROM ' + self.scan(p.name)), (('9223372036854775807',9),('1.5',10)))
        self.assertEqual(self.query('SELECT COUNT(*),COUNT(empty) FROM ' + self.scan('left.csv')), ((3,0),))

    def test_parquet_multi_group_nullable_projection(self):
        try:
            import pyarrow
        except ImportError:
            self.skipTest('optional Parquet fixture generation requires pyarrow')
        generator = Path(__file__).with_name('generate_file_scan_fixture.py')
        subprocess.run([sys.executable, '-X', 'utf8', str(generator), str(self.dir)], check=True, capture_output=True)
        for name in ['snappy_v1.parquet','snappy_v2.parquet','plain_v2.parquet']:
            with self.subTest(name=name):
                table = self.scan(name,'parquet')
                self.assertEqual(self.query(f'SELECT COUNT(*),MIN(id),MAX(id),SUM(id),COUNT(nullable) FROM {table}'), ((25,0,24,300,16),))
                self.assertEqual(self.query(f'SELECT id,flag,name,day,`time`,`unsigned` FROM {table} WHERE id=8'),
                    ((8,0,'unicode-你好0',datetime.date(2024,2,29),datetime.datetime(1969,12,31,23,59,59,999999),'18446744073709551615'),))

    def test_datetime_fractional_precision(self):
        p = self.dir / 'times.csv'
        p.write_text('t\n2026-08-01 10:11:12\n2026-08-03 12:13:14\n', encoding='utf-8')
        with self.db.cursor() as c:
            c.execute('SELECT t FROM ' + self.scan(p.name) + ' ORDER BY t')
            self.assertEqual(c.description[0][5], 0)
            self.assertEqual(c.fetchall(), ((datetime.datetime(2026,8,1,10,11,12),),
                                            (datetime.datetime(2026,8,3,12,13,14),)))
        p.write_text('t\n2026-08-01 10:11:12.123456\n2026-08-03 12:13:14\n', encoding='utf-8')
        with self.db.cursor() as c:
            c.execute('SELECT t FROM ' + self.scan(p.name) + ' ORDER BY t')
            self.assertEqual(c.description[0][5], 6)
            self.assertEqual(c.fetchall()[0][0].microsecond, 123456)

    def test_csv_options_and_json_types(self):
        p = self.dir / 'custom.csv'
        p.write_bytes(b'\xef\xbb\xbf1;\'a;\\\'b\';NULL\r\n2;\'\';\'NULL\'\r\n')
        sql = self.scan(p.name)[:-1] + ",header=false,delimiter=';',quote=CHAR(39))"
        # Options are constant literals; expression-valued options are rejected.
        with self.assertRaises(pymysql.MySQLError):
            self.query('SELECT * FROM ' + sql)
        sql = self.scan(p.name)[:-1] + ",header=false,delimiter=';',quote=" + self.db.escape("'") + ',escape=' + self.db.escape('\\') + ",nullstr='NULL')"
        self.assertEqual(self.query('SELECT * FROM ' + sql), ((1,"a;'b",None),(2,'','NULL')))
        p = self.dir / 'types.jsonl'
        p.write_text('{"quoted":"123","n":null}\n{"quoted":123,"flag":true}\n', encoding='utf-8')
        self.assertEqual(self.query('SELECT quoted,n,flag FROM ' + self.scan(p.name,'json')), (('123',None,None),('123',None,1)))

    def test_copy_roundtrip_and_no_overwrite(self):
        out = self.dir / 'export.csv'
        sql = 'COPY (SELECT id,name,score,day,empty FROM ' + self.scan('left.csv') + ' ORDER BY id) TO %s (FORMAT CSV,HEADER,NULLSTR \'NULL\')'
        self.query(sql, (str(out).replace('\\','/'),))
        original = out.read_bytes()
        result = self.query('SELECT * FROM ' + self.scan(out.name)[:-1] + ",nullstr='NULL') ORDER BY id")
        self.assertEqual(result, ((1,'Alice',98.5,datetime.date(2024,2,29),None),
             (2,'Bob, B',87.0,None,None),(3,'line\nthree',90.25,datetime.date(2024,3,1),None)))
        with self.assertRaises(pymysql.MySQLError):
            self.query(sql, (str(out).replace('\\','/'),))
        self.assertEqual(out.read_bytes(), original)
        self.assertFalse(list(self.dir.glob('.seekdb-copy-*.tmp')))

    def test_copy_table_and_runtime_failure_cleanup(self):
        table = '`file_copy_' + self.dir.name[-8:].replace('-','_') + '`'
        self.query(f'CREATE TABLE {table}(id INT PRIMARY KEY, value TEXT)')
        try:
            self.query(f'INSERT INTO {table} VALUES(1,\'1\'),(2,\'invalid-json\')')
            destination = str(self.dir / 'table.csv').replace('\\','/')
            self.query(f'COPY {table} TO %s (HEADER)', (destination,))
            self.assertEqual(self.query('SELECT * FROM ' + self.scan('table.csv') + ' ORDER BY id'), ((1,'1'),(2,'invalid-json')))
            failed = self.dir / 'query-failed.csv'
            with self.assertRaises(pymysql.MySQLError):
                self.query(f'COPY (SELECT CAST(value AS JSON) FROM {table} ORDER BY id) TO %s (HEADER)', (str(failed).replace('\\','/'),))
            self.assertFalse(failed.exists())
            self.assertFalse(list(self.dir.glob('.seekdb-copy-*.tmp')))
        finally:
            self.query(f'DROP TABLE {table}')

    def test_bounds_bad_data_and_switch(self):
        for name, data, kind in [('bad.csv','a,b\n1\n','csv'), ('nested.jsonl','{"a":[1]}\n','json'),
                                 ('bad.jsonl','{"a":1}\nnot-json\n','json')]:
            (self.dir / name).write_text(data, encoding='utf-8')
            with self.subTest(name=name), self.assertRaises(pymysql.MySQLError):
                self.query('SELECT * FROM ' + self.scan(name,kind))
        with self.assertRaises(pymysql.MySQLError):
            self.query("SELECT * FROM read_csv('https://example.com/data.csv')")
        with tempfile.TemporaryDirectory(prefix='seekdb-outside-', dir=self.root.parent) as outside:
            outside_file = Path(outside) / 'outside.csv'
            outside_file.write_text('value\n123\n', encoding='utf-8')
            with self.assertRaises(pymysql.MySQLError):
                self.query('SELECT * FROM read_csv(%s)', (str(outside_file),))
            traversed = self.root / '..' / Path(outside).name / outside_file.name
            with self.assertRaises(pymysql.MySQLError):
                self.query('SELECT * FROM read_csv(%s)', (str(traversed),))
            outside_export = Path(outside) / 'export.csv'
            with self.assertRaises(pymysql.MySQLError):
                self.query('COPY (SELECT 1 AS value) TO %s (FORMAT CSV)', (str(outside_export),))
            self.assertFalse(outside_export.exists())
            link = self.dir / 'outside-link.csv'
            try:
                link.symlink_to(outside_file)
            except (OSError, NotImplementedError):
                pass  # Creating symlinks may require privileges on Windows.
            else:
                try:
                    with self.assertRaises(pymysql.MySQLError):
                        self.query('SELECT * FROM read_csv(%s)', (str(link),))
                finally:
                    link.unlink()
        self.query('SET SESSION enable_file_sql=OFF')
        try:
            with self.assertRaises(pymysql.MySQLError):
                self.query('SELECT * FROM ' + self.scan('left.csv'))
        finally:
            self.query('SET SESSION enable_file_sql=ON')


if __name__ == '__main__':
    unittest.main(verbosity=2)
