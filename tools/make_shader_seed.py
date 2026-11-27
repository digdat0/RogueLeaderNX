"""Makes the starter shader list the releases ship (initial_pipeline_cache.db, next to the
program): the pipelines a player's own cache holds, so a new install's first start compiles them
all before the game begins ("PREPARING SHADERS") instead of while it plays. Aurora merges the file
into the player's cache at every start (pipeline_cache.cpp seed_pipeline_cache): existing players
gain the ones for places they haven't been yet.

It holds only Aurora's pipeline settings (TEV stages, blend and depth modes, vertex layouts) - no
textures, models or game code. Ubershader rows are left out (derived from the others when
ubershaders are on), and so is anything not of the newest config version (Aurora drops those).

    python tools/make_shader_seed.py [<pipeline_cache.db>] [-o build/initial_pipeline_cache.db]

Default input: the portable user folder's cache (build/windows/user, a release folder's user) if
given, else %APPDATA%/Red Two/pipeline_cache.db. Read through SQLite (its write-ahead log too),
never modified."""
import argparse
import os
import sqlite3
import struct

UBER_MARKER = 0x52424555  # "UBER" in PipelineConfig::msaaSamples (Aurora lib/gx/uber.hpp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache', nargs='?', default=os.path.join(os.environ.get('APPDATA', ''), 'Red Two', 'pipeline_cache.db'))
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'initial_pipeline_cache.db'))
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        os.remove(out)
    src = sqlite3.connect('file:' + os.path.abspath(args.cache).replace('\\', '/') + '?mode=ro', uri=True)
    dst = sqlite3.connect(out)
    src.backup(dst)  # (a consistent copy, the WAL included)
    src.close()
    newest = dict(dst.execute('SELECT type, MAX(config_version) FROM pipeline_cache GROUP BY type'))
    drop = []
    for rowid, typ, version, config in dst.execute('SELECT rowid, type, config_version, config FROM pipeline_cache'):
        uber = typ == 1 and len(config) >= 8 and struct.unpack_from('<I', config, 4)[0] == UBER_MARKER
        if uber or version != newest[typ]:
            drop.append((rowid,))
    dst.executemany('DELETE FROM pipeline_cache WHERE rowid = ?', drop)
    dst.commit()
    dst.execute('PRAGMA journal_mode = DELETE')
    dst.execute('VACUUM')
    kept = dict(dst.execute('SELECT type, COUNT(*) FROM pipeline_cache GROUP BY type'))
    dst.close()
    print(f'{out}: {sum(kept.values())} pipelines {kept} ({len(drop)} left out), {os.path.getsize(out) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
