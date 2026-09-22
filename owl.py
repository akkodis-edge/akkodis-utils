#!/usr/bin/env python3

import sys
import time
import random
import sqlite3
import argparse
import os
from ctypes import *
from time import strftime
from datetime import datetime, timezone

class LibOwlSensorData(Structure):
    _fields_ = [
        ('name', c_char_p),
        ('index', c_int64),
        ('epoch', c_double),
        ('type', c_int),
        ('value', c_int)]

class LibOwlFilterData(Union):
    _fields_ = [
        ('mdouble', c_double),
        ('mi64', c_int64),
        ('str', c_char_p)]

class LibOwlFilter(Structure):
    _fields_ = [
        ('type', c_int),
        ('op', c_int),
        ('data', LibOwlFilterData)]

SENSOR_TEMPERATURE = 0
LIBOWL_OP_GREATER_THAN = 0
LIBOWL_OP_GREATER_EQUAL = 1
LIBOWL_OP_LESS_THAN = 2
LIBOWL_OP_LESS_EQUAL = 3
LIBOWL_OP_EQUAL = 4

class LibOwl:
    def __init__(self, path):
        self.lib = cdll.LoadLibrary("libowl.so")
        self.owl = c_void_p()
        ret = self.lib.libowl_open(byref(self.owl), c_char_p(path.encode('utf-8')), 0)
        if ret != 0:
            raise OSError(ret, os.strerror(ret), 'Failed opening db')

    def __del__(self):
        if (self.owl):
            self.lib.libowl_close(self.owl)
    def read(self, limit, after=None, before=None):
        # create filters
        filters = []
        if after != None:
            filters.append((self.lib.libowl_filter_epoch, c_int(LIBOWL_OP_GREATER_THAN), c_double(after)))
        if before != None:
            filters.append((self.lib.libowl_filter_epoch, c_int(LIBOWL_OP_LESS_THAN), c_double(before)))
        c_filter_array_type = LibOwlFilter * len(filters)
        c_filter_array = c_filter_array_type()
        for index, (func, op, value) in enumerate(filters):
            ret = func(byref(c_filter_array[index]), op, value)
            if (ret != 0):
                raise OSError(ret, os.strerror(ret), 'Failed creating filter')
        # create data array
        c_data_array_type = LibOwlSensorData * limit
        c_data_array = c_data_array_type()
        # work
        out = []
        processed_data = 0
        try:
            ret = self.lib.libowl_read(self.owl, c_int(0), byref(c_filter_array), c_size_t(len(c_filter_array)),
                                                byref(c_data_array), c_size_t(len(c_data_array)))
            if ret < 0:
                raise OSError(ret, os.strerror(ret), 'Failed reading db')
            if ret > 0:
                processed_data = ret
        finally:
            for data in c_data_array[:processed_data]:
                type = 'UNKNOWN'
                if data.type == SENSOR_TEMPERATURE:
                    type = 'TEMP'
                out.append((data.name.decode('utf-8'), data.epoch, type, data.value))
                self.lib.libowl_sensor_data_free(byref(data))
        return out

def main():
    parser = argparse.ArgumentParser(description='Logger separated in daemon writer and client reader(s)')
    parser.add_argument('--db', required=True, help='Path to database file')
    parser.add_argument('--debug', action='store_true', help='Debug output')
    args = parser.parse_args()

    db = LibOwl(args.db)

    next_epoch = 0.0
    while True:
        data = db.read(50, after=next_epoch)
        for name, epoch, type, value in data:
            next_epoch = epoch
            datestr = datetime.fromtimestamp(epoch, timezone.utc)
            print('[{}] ({}) {}: {}'.format(datestr, type, name, value))

        if not data:
            time.sleep(0.1)

    sys.exit(1)

if __name__ == '__main__':
    main()
