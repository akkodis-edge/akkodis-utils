#!/usr/bin/env python3

import sys
import time
import argparse
from datetime import datetime, timezone
from libowl import LibOwl, sensor_type_name

def main():
    parser = argparse.ArgumentParser(description='Logger separated in daemon writer and client reader(s)')
    parser.add_argument('--db', required=True, help='Path to database file')
    parser.add_argument('--debug', action='store_true', help='Debug output')
    args = parser.parse_args()

    db = LibOwl(args.db)

    print('Available sensors in db:')
    for (name, type) in db.list_sensors():
        print('({}): {}'.format(sensor_type_name.get(type, 'UNKNOWN'), name))

    next_epoch = 0.0
    while True:
        data = db.read(50, after=next_epoch)
        for name, epoch, type, value in data:
            next_epoch = epoch
            datestr = datetime.fromtimestamp(epoch, timezone.utc)
            print('[{}] ({}) {}: {}'.format(datestr, sensor_type_name.get(type, 'UNKNOWN'), name, value))

        if not data:
            time.sleep(0.1)

    sys.exit(1)

if __name__ == '__main__':
    main()
