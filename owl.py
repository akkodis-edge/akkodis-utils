#!/usr/bin/env python3

import sys
import time
import random
import sqlite3
import argparse

def init_db(db, cursor, sensors):
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS
            category_type(
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                UNIQUE(name)
            )
    ''')
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS
            sensors(
                id INTEGER PRIMARY KEY,
                type_id INTEGER NOT NULL,
                name TEXT NOT NULL,
                FOREIGN KEY(type_id) REFERENCES category_type(id),
                UNIQUE(type_id, name)
            )
    ''')
    # Note: data(id) can be used to determine if time in data(epoch)
    # has run backwards.
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS
            data(
                id INTEGER PRIMARY KEY,
                sensor_id INTEGER NOT NULL,
                value INTEGER NOT NULL,
                epoch INTEGER NOT NULL,
                FOREIGN KEY(sensor_id) REFERENCES sensors(id)
            )
    ''')

    for sensor in sensors:
        cursor.execute('''
            INSERT OR IGNORE INTO category_type(name) VALUES (?)
        ''', (sensor['type'],))
        cursor.execute('''
            INSERT OR IGNORE INTO sensors(name, type_id) VALUES
                (?, (SELECT id from category_type WHERE name=(?)))
        ''', (sensor['name'], sensor['type']))

    db.commit()

def add_value(db, cursor, type, name, value):
    cursor.execute('''
        INSERT INTO data(sensor_id, value, epoch) VALUES (
            (SELECT id from sensors WHERE type_id=(SELECT id from category_type WHERE name=(?)) AND name=(?)),
            ?, unixepoch('now'))
        ''', (type, name, value))

def get_values(db, cursor, id):
    # Read data since id
    res = cursor.execute('''
        SELECT
            A.id,
            (SELECT name from category_type WHERE id = S.type_id),
            S.name,
            A.value,
            A.epoch
        FROM data as A
        INNER JOIN sensors AS S on S.id = A.sensor_id
        WHERE A.id >= (?)
        ORDER BY A.id ASC
        LIMIT 100
    ''', (id,))
    return res.fetchall()

def temperature():
    return random.randrange(18, 22, 1)

uptime_stored = -1
def uptime():
    global uptime_stored
    uptime_stored += 1
    return uptime_stored

def main():
    parser = argparse.ArgumentParser(description='Logger separated in daemon writer and client reader(s)')
    parser.add_argument('--db', required=True, help='Path to database file')
    parser.add_argument('--debug', action='store_true', help='Debug output')
    args = parser.parse_args()

    # This should be a config file provided on cmdline
    sensors = [
        {'type': 'temperature', 'name': 'cpu', 'func': temperature},
        {'type': 'monotonic', 'name': 'uptime', 'func': uptime},
    ]

    # Open db
    db = sqlite3.connect(args.db)
    if args.debug:
        db.set_trace_callback(print)
    cursor = db.cursor()

    # Initialize db
    init_db(db, cursor, sensors)

    next_id = 0
    while True:
        # Retrieve data
        for id, type, name, value, epoch in get_values(db, cursor, next_id):
            date = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(epoch))
            print('[{}] {}.{} = {}'.format(date, type, name, value))
            if id >= next_id:
                next_id = id + 1

        # Add data
        for sensor in sensors:
            add_value(db, cursor, sensor['type'], sensor['name'], sensor['func']())
        db.commit()

        time.sleep(1)

    sys.exit(1)

if __name__ == '__main__':
    main()
