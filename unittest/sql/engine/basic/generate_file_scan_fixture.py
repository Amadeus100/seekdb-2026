"""Generate optional Parquet scanner fixtures; requires pyarrow for tests only."""
import datetime
import pathlib
import sys
import pyarrow as pa
import pyarrow.parquet as pq

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
table = pa.table({
    'id': pa.array(range(25), type=pa.int64()),
    'nullable': pa.array([None if n % 3 == 0 else n * 10 for n in range(25)], type=pa.int32()),
    'flag': pa.array([bool(n % 2) for n in range(25)]),
    'name': pa.array(['unicode-你好' + str(n % 4) for n in range(25)]),
    'day': pa.array([datetime.date(2024, 2, 29)] * 25, type=pa.date32()),
    'time': pa.array([-1000] * 25, type=pa.timestamp('ns')),
    'unsigned': pa.array([2 ** 64 - 1] * 25, type=pa.uint64()),
})
for name, compression, page_version, dictionary in [
    ('snappy_v1.parquet', 'snappy', '1.0', True),
    ('snappy_v2.parquet', 'snappy', '2.0', True),
    ('plain_v2.parquet', None, '2.0', False),
]:
    pq.write_table(table, root / name, compression=compression,
                   data_page_version=page_version, use_dictionary=dictionary,
                   row_group_size=7, version='2.6')
    print(root / name)
