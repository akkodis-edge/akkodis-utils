#!/usr/bin/env python3

import sys
import os
import time
import argparse
import subprocess
import termios
import tty
import select
from datetime import datetime
from rich.live import Live
from rich.console import Console
from rich.layout import Layout
from rich.text import Text
from rich.table import Table
from rich import print
from libowl import LibOwl, sensor_type_name, Filters, Options
from libowl import LIBOWL_OP_GREATER_THAN, LIBOWL_OP_LESS_EQUAL, LIBOWL_OP_IN
from libowl import SENSOR_TEMPERATURE, SENSOR_RATIO, SENSOR_COUNTER

def plot_transform(type):
    if type == SENSOR_TEMPERATURE:
        return '/1000'
    elif type == SENSOR_RATIO:
        return '/10000'
    else:
        return ''

def plot_datapoints(datapoints, width, height, yformat):
    input = bytearray()
    datablock_names = []
    for name in datapoints:
        # get type from first reading, second tuple value
        type = datapoints[name][0][1]
        datablock_names.append((name, plot_transform(type)))
        input += f'$Data{len(datablock_names)} << EOD\n'.encode('utf-8')
        for (epoch, type, value) in datapoints[name]:
            input += f'{epoch} {value}\n'.encode('utf-8')
        input += b'EOD\n'
    input += \
f'''
set terminal dumb ansi256 {width},{height}
set xdata time
set timefmt "%s"
set key outside
set format x ""
set format y "{yformat}"
set xtics nomirror time
set ytics nomirror
'''.encode('utf-8')

    for index, (name, transform) in enumerate(datablock_names):
        prefix = 'plot' if index == 0 else ','
        input += f'{prefix} $Data{index+1} using 1:(column(2){transform}) with line title "{name}"'.encode('utf-8')
    input += b'\n'

    try:
        res = subprocess.run(['gnuplot'], check=True, input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return res.stdout
    except subprocess.CalledProcessError as e:
        print(e, e.stdout, e.stderr)

class Gnuplot:
    def __init__(self, datapoints, yformat=''):
        self.datapoints = datapoints
        self.prev_widht = 0
        self.prev_height = 0
        self.prev_plot = None
        self.yformat = yformat
    def __rich_console__(self, console, options):
        if self.prev_widht == options.max_width and self.prev_height == options.max_height and self.prev_plot != None:
            return self.prev_plot
        plot = plot_datapoints(self.datapoints, width=options.max_width, height=options.max_height, yformat=self.yformat)
        plot = plot.decode('utf-8').rstrip()
        self.prev_plot = Text.from_ansi(plot)
        self.prev_width = options.max_width
        self.prev_height = options.max_height
        return self.prev_plot

def read_datapoints(db, datapoints, points, types, time_from, time_to):
    '''
    Return True if data updated or false if not
    '''
    last_index = 0
    # Calculate interval per datapoint
    interval = (time_to - time_from) / points
    # Minimum supported interval is 1 ms
    if interval < 0.001:
        interval = 0.001
    options = Options()
    options.interval(interval)
    options.avg()
    updated = 0
    while True:
        filters = Filters()
        filters.index(LIBOWL_OP_GREATER_THAN, last_index)
        for type in types:
            filters.type(LIBOWL_OP_IN, type)
        filters.epoch(LIBOWL_OP_GREATER_THAN, time_from)
        filters.epoch(LIBOWL_OP_LESS_EQUAL, time_to)
        data = db.read(filters, options, 1000)
        updated += len(data)
        # no new data available
        if updated == 0 and len(data) == 0:
            return False
        # Add sensor values to datapoints
        for name, index, epoch, type, value in data:
            if name not in datapoints:
                datapoints[name] = []
            datapoints[name].append((epoch, type, value))
            if index > last_index:
                last_index = index
        # No more data to read
        if len(data) < 1000:
            break
    # Trim readings outside our scope
    to_delete = []
    for name in datapoints:
        # delete if empty
        if not datapoints[name]:
            to_delete(name)
            continue
        # limit to points number of values and sort by epoch
        datapoints[name] = sorted(datapoints[name][-points:], key=lambda tup: tup[0])
        # Remove if epoch of last reading is less than time_from
        if datapoints[name][-1][0] < time_from:
            to_delete(name)
            continue

    for name in to_delete:
        del datapoints[name]
    return True

def update_plot(root, datapoints, time_from, time_to, yformat):
    root['plot'].update(Gnuplot(datapoints, yformat))
    info_text = Table.grid(expand=True)
    info_text.add_column(justify='left')
    info_text.add_column(justify='right')
    info_text.add_row(
        Text(datetime.fromtimestamp(time_from).strftime('%Y-%m-%d %H:%M:%S.%f')),
        Text(datetime.fromtimestamp(time_to).strftime('%Y-%m-%d %H:%M:%S.%f')),
    )
    root['info'].update(info_text)

KEY_UP = 0
KEY_LEFT = 1
KEY_RIGHT = 2
KEY_DOWN = 3

class Input():
    def __init__(self):
        self.orig = None
        self.orig = termios.tcgetattr(sys.stdin)
        # disable buffering
        tty.setcbreak(sys.stdin, when=termios.TCSANOW)
        self.fds = select.poll()
        self.fds.register(sys.stdin.fileno(), select.POLLIN)
    def __del__(self):
        if self.orig != None:
            termios.tcsetattr(sys.stdin, termios.TCSANOW, self.orig)
    def __read_byte(self):
        if len(self.fds.poll(0)) < 1:
            return None
        value = sys.stdin.buffer.read1(1)
        if len(value) < 1:
            return None
        return value
    def getch(self):
        '''
        Extremely basic ANSI escape code parses to detect KEY_[UP,LEFT,RIGHT,DOWN].
        '''
        value = self.__read_byte()
        # check if escape sequence, if not, return value
        if value != b'\x1b':
            return value
        # check if control sequence introducer "[" and skip it
        value = self.__read_byte()
        if value == b'[':
            value = self.__read_byte()
        # check for key up down
        if value == b'A':
            return KEY_UP
        elif value == b'D':
            return KEY_LEFT
        elif value == b'C':
            return KEY_RIGHT
        elif value == b'B':
            return KEY_DOWN
        # return value for unknown
        return value

POS_MIN = 0
POS_YEAR = 0
POS_MONTH = 1
POS_WEEK = 2
POS_DAY = 3
POS_HOUR = 4
POS_MINUTE = 5
POS_MAX = 5

class Control:
    def __init__(self):
        self.input = Input()
        self.cursor_pos = POS_HOUR
        self.cursor_sel = POS_HOUR
    def process_input(self):
        while True:
            char = self.input.getch()
            if char == None:
                break
            elif char == KEY_UP:
                pass
            elif char == KEY_LEFT:
                if self.cursor_pos > POS_MIN:
                    self.cursor_pos -= 1
            elif char == KEY_RIGHT:
                if self.cursor_pos < POS_MAX:
                    self.cursor_pos += 1
            elif char == KEY_DOWN:
                pass
            elif char == b' ': # SPACE
                self.cursor_sel = self.cursor_pos
    def __generate_button(self, pos):
        if pos == POS_YEAR:
            name = ' YEAR '
        elif pos == POS_MONTH:
            name = ' MONTH '
        elif pos == POS_WEEK:
            name = ' WEEK '
        elif pos == POS_DAY:
            name = ' DAY '
        elif pos == POS_HOUR:
            name = ' HOUR '
        elif pos == POS_MINUTE:
            name = ' MINUTE '
        else:
            name = '  UNKNOWN  '
        if self.cursor_pos == pos and self.cursor_sel == pos:
            color = 'bold blue on magenta'
        elif self.cursor_pos == pos:
            color = 'bold magenta'
        elif self.cursor_sel == pos:
            color = 'blue on magenta'
        else:
            color = 'blue'
        return (name, color)
    def footer(self):
        grid = Table.grid(expand=True)
        grid.add_column(justify='left')
        grid.add_column(justify='center')
        grid.add_row()
        color_selected = 'bold green on magenta'
        color_other = 'blue'
        grid.add_row('', Text.assemble(
            self.__generate_button(POS_YEAR),
            ('  '),
            self.__generate_button(POS_MONTH),
            ('  '),
            self.__generate_button(POS_WEEK),
            ('  '),
            self.__generate_button(POS_DAY),
            ('  '),
            self.__generate_button(POS_HOUR),
            ('  '),
            self.__generate_button(POS_MINUTE),
        ))
        grid.add_row('Akkodis Edge')
        return grid
    def plot_time_range(self):
        time_now = time.time()
        if self.cursor_sel == POS_YEAR:
            return (time_now - (60*60*24*365), time_now)
        elif self.cursor_sel == POS_MONTH:
            return (time_now - (60*60*24*30), time_now)
        elif self.cursor_sel == POS_WEEK:
            return (time_now - (60*60*24*7), time_now)
        elif self.cursor_sel == POS_DAY:
            return (time_now - (60*60*24), time_now)
        elif self.cursor_sel == POS_HOUR:
            return (time_now - (60*60), time_now)
        elif self.cursor_sel == POS_MINUTE:
            return (time_now - (60), time_now)
        else:
            raise RuntimeError('Invalid plotting range')

def main():
    parser = argparse.ArgumentParser(description='Logger separated in daemon writer and client reader(s)')
    parser.add_argument('--db', required=True, help='Path to database file')
    parser.add_argument('--debug', action='store_true', help='Debug output')
    args = parser.parse_args()

    db = LibOwl(args.db)

    temp_datapoints = {}
    ratio_datapoints = {}

    console = Console()

    layout = Layout(name='root')
    layout.split(
        Layout(name='main'),
        Layout(name='footer', size=3),
    )
    layout['main'].split(
        Layout(name='temp'),
        Layout(name='ratio'),
    )
    layout['temp'].split(
        Layout(name='plot'),
        Layout(name='info', size=1),
    )
    layout['ratio'].split(
        Layout(name='plot'),
        Layout(name='info', size=1),
    )

    control = Control()

    with Live(layout, refresh_per_second=4) as live:
        while True:
            control.process_input()
            layout['footer'].update(control.footer())
            (time_from, time_to) = control.plot_time_range()
            if read_datapoints(db, temp_datapoints, 100, [SENSOR_TEMPERATURE], time_from, time_to):
                update_plot(layout['temp'], temp_datapoints, time_from, time_to, '%.1fC')
            if read_datapoints(db, ratio_datapoints, 100, [SENSOR_RATIO], time_from, time_to):
                update_plot(layout['ratio'], ratio_datapoints, time_from, time_to, '%.1f%%')
            time.sleep(1)

    sys.exit(1)

if __name__ == '__main__':
    main()
