#!/usr/bin/env python3
"""Run competition mysqltests against an already running disposable seekdb."""

import base64
import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[2]
    deploy = root / 'tools' / 'deploy'
    mysqltest = root / 'deps' / '3rd' / 'u01' / 'obclient' / 'bin' / 'mysqltest'
    evidence = root / 'kernel-test-evidence' / 'mysqltest'
    evidence.mkdir(parents=True, exist_ok=True)
    if not mysqltest.is_file():
        raise SystemExit('mysqltest client not found')
    secure_root = Path(os.environ['SEEKDB_FILE_ROOT'])
    data = secure_root / 'data_for_mysqltest_file_sql'
    data.mkdir(parents=True, exist_ok=True)
    fixtures = deploy / 'mysql_test' / 'test_suite' / 'file_sql' / 'data'
    for name in ('basic.csv', 'basic.jsonl', 'invalid.txt'):
        (data / name).write_bytes((fixtures / name).read_bytes())
    (data / 'simple.parquet').write_bytes(base64.b64decode((fixtures / 'simple.parquet.b64').read_bytes()))
    # Provision locally instead of the upstream include's SSH/SCP setup.  The
    # SQL, expected output, and mysqltest comparison are otherwise unchanged.
    file_test = deploy / 'mysql_test/test_suite/file_sql/t/file_sql_basic.test'
    lines = file_test.read_text(encoding='utf-8').splitlines(keepends=True)
    local_file_test = evidence / 'file_sql_basic.local.test'
    local_file_test.write_text(''.join(line for line in lines if not line.startswith('--source mysql_test/test_suite/file_sql/include/')),
                               encoding='utf-8')
    cases = [('file_sql', 'file_sql_basic', local_file_test)] + [
        ('ai_column', name, deploy / 'mysql_test' / 'test_suite' / 'ai_column' / 't' / (name + '.test'))
        for name in ('ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
                     'ai_column_vector_index', 'ai_column_retry_strict')
    ]
    env = os.environ.copy()
    env.update(OBSERVER_DIR=str(secure_root), OBMYSQL_PORT='2884', OBMYSQL_MS0='127.0.0.1',
               OBMYSQL_PWD='', OBMYSQL_USR='root', IS_BUSINESS='0', TENANT='mysql')
    client_root = mysqltest.parent.parent
    env['PATH'] = str(mysqltest.parent) + os.pathsep + env.get('PATH', '')
    env['LD_LIBRARY_PATH'] = os.pathsep.join([str(client_root / 'lib'), str(client_root / 'lib64'),
                                             env.get('LD_LIBRARY_PATH', '')])
    failed = False
    for suite, name, test in cases:
        result = deploy / 'mysql_test' / 'test_suite' / suite / 'r' / 'mysql' / (name + '.result')
        logdir = evidence / name
        logdir.mkdir(exist_ok=True)
        command = [str(mysqltest), '--host=127.0.0.1', '--port=2884', '--user=root', '--database=test',
                   '--silent', '--test-file=' + str(test), '--result-file=' + str(result),
                   '--logdir=' + str(logdir), '--tmpdir=' + str(logdir), '--tail-lines=20']
        try:
            proc = subprocess.run(command, cwd=deploy, env=env, capture_output=True, text=True, timeout=300)
            output, code = proc.stdout + proc.stderr, proc.returncode
        except subprocess.TimeoutExpired as exc:
            output = str(exc)
            code = 124
        (logdir / 'client.log').write_text(output, encoding='utf-8')
        print(f'{suite}.{name}: exit={code}', flush=True)
        if code:
            failed = True
            print(output[-4000:], flush=True)
        if suite == 'ai_column':
            for port in range(19120, 19125):
                subprocess.run([sys.executable, 'mysql_test/test_suite/ai_column/include/ai_column_mock.py',
                                'stop', str(port)], cwd=deploy, env=env, capture_output=True)
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
