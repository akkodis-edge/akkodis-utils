import os
from ctypes import *

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
        ('str', c_char_p),
        ('mint', c_int)]

class LibOwlFilter(Structure):
    _fields_ = [
        ('type', c_int),
        ('op', c_int),
        ('data', LibOwlFilterData)]

class LibOwlOptionData(Union):
    _fields_ = [
        ('mdouble', c_double)]

class LibOwlOption(Structure):
    _fields_ = [
        ('type', c_int),
        ('data', LibOwlOptionData)]

SENSOR_TEMPERATURE = 0
SENSOR_VOLTAGE = 1
SENSOR_CURRENT = 2
SENSOR_RATIO = 3
SENSOR_COUNTER = 4
LIBOWL_OP_GREATER_THAN = 0
LIBOWL_OP_GREATER_EQUAL = 1
LIBOWL_OP_LESS_THAN = 2
LIBOWL_OP_LESS_EQUAL = 3
LIBOWL_OP_EQUAL = 4
LIBOWL_OP_NOT_EQUAL = 5
LIBOWL_OP_IN = 6

sensor_type_name = {
    SENSOR_TEMPERATURE: 'TEMP',
    SENSOR_VOLTAGE: 'VOLTAGE',
    SENSOR_CURRENT: 'CURRENT',
    SENSOR_RATIO: 'RATIO',
    SENSOR_COUNTER: 'COUNTER'
}

class Filters():
    def __init__(self):
        self.filters = []
    def __check_op(op):
        if not isinstance(op, int):
            raise TypeError('Op must be of type int')
        if op < LIBOWL_OP_GREATER_THAN or op > LIBOWL_OP_IN:
            raise ValueError('Op invalid value')
    def name(self, op, name):
        if not isinstance(name, str):
            raise TypeError('Name must be of type str')
        Filters.__check_op(op)
        self.filters.append(('name', op, name))
    def epoch(self, op, epoch):
        if not isinstance(epoch, float):
            raise TypeError('epoch must be of type float')
        Filters.__check_op(op)
        self.filters.append(('epoch', op, epoch))
    def index(self, op, index):
        if not isinstance(index, int):
            raise TypeError('Index must be of type int')
        Filters.__check_op(op)
        self.filters.append(('index', op, index))
    def type(self, op, type):
        if not isinstance(type, int):
            raise TypeError('Type must be of type int')
        if type < SENSOR_TEMPERATURE or type > SENSOR_COUNTER:
            raise ValueError('Type invalid value')
        Filters.__check_op(op)
        self.filters.append(('type', op, type))
    def build(self, lib):
        c_filter_array_type = LibOwlFilter * len(self.filters)
        c_filter_array = c_filter_array_type()
        for i, (typename, op, value) in enumerate(self.filters):
            c_array_ref = byref(c_filter_array[i])
            c_op = c_int(op)
            if typename == 'name':
                ret = lib.libowl_filter_name(c_array_ref, c_op, c_char_p(value.encode('utf-8')))
            elif typename == 'epoch':
                ret = lib.libowl_filter_epoch(c_array_ref, c_op, c_double(value))
            elif typename == 'index':
                ret = lib.libowl_filter_index(c_array_ref, c_op, c_int(value))
            elif typename == 'type':
                ret = lib.libowl_filter_type(c_array_ref, c_op, c_int(value))
            else:
                raise RuntimeError('Unknown filter type: {}'.format(typename))
            if (ret != 0):
                raise OSError(ret, os.strerror(ret), 'Failed creating filter: {}'.format(typename))
        return c_filter_array

class Options():
    def __init__(self):
        self.__des = False
        self.__avg = False
        self.__min = False
        self.__max = False
        self.__interval = None
    def descending(self):
        self.__des = True
    def min(self):
        if sum([self.__avg, self.__max]) > 0:
            raise ValueError('Options avg, min and max are mutually exclusive')
        self.__min = True
    def max(self):
        if sum([self.__avg, self.__min]) > 0:
            raise ValueError('Options avg, min and max are mutually exclusive')
        self.__max = True
    def avg(self):
        if sum([self.__min, self.__max]) > 0:
            raise ValueError('Options avg, min and max are mutually exclusive')
        self.__avg = True
    def interval(self, interval):
        if not isinstance(interval, float):
            raise TypeError('Interval must be of type float')
        if interval < 0.001:
            raise ValueError('Interval must be >= 0.001')
        self.__interval = interval
    def build(self, lib):
        count = sum([self.__min, self.__max, self.__avg, self.__des])
        if self.__interval != None:
            count += 1
        c_option_array_type = LibOwlOption * count
        c_option_array = c_option_array_type()
        c_index = 0
        if self.__des:
            ret = lib.libowl_option_descending(byref(c_option_array[c_index]))
            if ret != 0:
                 raise OSError(ret, os.strerror(ret), 'Failed creating option: descending')
            c_index += 1
        if self.__min:
            ret = lib.libowl_option_min(byref(c_option_array[c_index]))
            if ret != 0:
                 raise OSError(ret, os.strerror(ret), 'Failed creating option: min')
            c_index += 1
        if self.__max:
            ret = lib.libowl_option_max(byref(c_option_array[c_index]))
            if ret != 0:
                 raise OSError(ret, os.strerror(ret), 'Failed creating option: max')
            c_index += 1
        if self.__avg:
            ret = lib.libowl_option_avg(byref(c_option_array[c_index]))
            if ret != 0:
                 raise OSError(ret, os.strerror(ret), 'Failed creating option: average')
            c_index += 1
        if self.__interval != None:
            ret = lib.libowl_option_interval(byref(c_option_array[c_index]), c_double(self.__interval))
            if ret != 0:
                raise OSError(ret, os.strerror(ret), 'Failed creating option: interval')
            c_index += 1
        return c_option_array

class LibOwl:
    def __init__(self, path):
        self.owl = None
        self.lib = cdll.LoadLibrary("libowl.so.1")
        self.owl = c_void_p()
        ret = self.lib.libowl_open(byref(self.owl), c_char_p(path.encode('utf-8')), 0)
        if ret != 0:
            raise OSError(ret, os.strerror(ret), 'Failed opening db')
    def __del__(self):
        if (self.owl):
            self.lib.libowl_close(self.owl)
    def read(self, filters, options, limit):
        c_filters_array = filters.build(self.lib)
        c_filters_size = c_size_t(len(c_filters_array))
        c_options_array = options.build(self.lib)
        c_options_size = c_size_t(len(c_options_array))
        # create data array
        c_data_array_type = LibOwlSensorData * limit
        c_data_array = c_data_array_type()
        c_data_size = c_size_t(len(c_data_array))
        # work
        out = []
        processed_data = 0
        try:
            ret = self.lib.libowl_read(self.owl, byref(c_options_array), c_options_size,
                                                byref(c_filters_array), c_filters_size,
                                                byref(c_data_array), c_data_size)
            if ret < 0:
                raise OSError(ret, os.strerror(ret), 'Failed reading db')
            if ret > 0:
                processed_data = ret
        finally:
            for data in c_data_array[:processed_data]:
                out.append((data.name.decode('utf-8'), data.index, data.epoch, data.type, data.value))
                self.lib.libowl_sensor_data_free(byref(data))
        return out
