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
    def __init__(self, owl):
        self.owl = owl
        self.input = Input()
        self.cursor_pos = POS_HOUR
        self.cursor_sel = POS_HOUR
        self.dp_temperature = self.__create_datapoints([SENSOR_TEMPERATURE])
        self.dp_ratio = self.__create_datapoints([SENSOR_RATIO])
        self.footer = self.__generate_footer()
    def __create_datapoints(self, types):
        (points, interval) = self.__calc_points_and_interval()
        filters = Filters()
        for type in types:
            filters.type(LIBOWL_OP_IN, type)
        return self.owl.datapoints(points, interval, filters, aggregate='avg')
    def __calc_points_and_interval(self):
        points = 100
        if self.cursor_sel == POS_YEAR:
            interval = 60.0*60.0*24.0*365.0/points
        elif self.cursor_sel == POS_MONTH:
            interval = 60.0*60.0*24.0*30.0/points
        elif self.cursor_sel == POS_WEEK:
            interval = 60.0*60.0*24.0*7.0/points
        elif self.cursor_sel == POS_DAY:
            interval = 60.0*60.0*24.0/points
        elif self.cursor_sel == POS_HOUR:
            interval = 60.0*60.0/points
        elif self.cursor_sel == POS_MINUTE:
            interval = 60.0/points
        else:
            raise RuntimeError('Invalid plotting range')
        return (points, interval)
    def process_input(self):
        selected_before = self.cursor_sel
        cursor_before = self.cursor_pos
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
        # update datapoints if new range selected
        if selected_before != self.cursor_sel:
            self.dp_temperature = self.__create_datapoints([SENSOR_TEMPERATURE])
            self.dp_ratio = self.__create_datapoints([SENSOR_RATIO])
        # Generate new footer if any update
        if selected_before != self.cursor_sel or \
            cursor_before != self.cursor_pos:
            self.footer = self.__generate_footer()
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
    def __generate_footer(self):
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

def main():
    parser = argparse.ArgumentParser(description='Logger separated in daemon writer and client reader(s)')
    parser.add_argument('--db', required=True, help='Path to database file')
    parser.add_argument('--debug', action='store_true', help='Debug output')
    args = parser.parse_args()

    db = LibOwl(args.db)

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

    control = Control(db)

    with Live(layout, refresh_per_second=2) as live:
        while True:
            control.process_input()
            layout['footer'].update(control.footer)
            if control.dp_temperature.update():
                (start, end) = control.dp_temperature.range()
                data = control.dp_temperature.get()
                update_plot(layout['temp'], data, start, end, '%.1fC')
            if control.dp_ratio.update():
                (start, end) = control.dp_ratio.range()
                data = control.dp_ratio.get()
                update_plot(layout['ratio'], data, start, end, '%.1f%%')
            time.sleep(0.1)

    sys.exit(1)

if __name__ == '__main__':
    main()
