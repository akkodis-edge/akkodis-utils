#!/usr/bin/env python3

import sys
import time
import argparse
import subprocess
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

def plotv2(datapoints, x_size, y_size):
    input = bytearray()
    datablock_names = []
    for name in datapoints:
        datablock_names.append(name)
        input += f'$Data{len(datablock_names)} << EOD\n'.encode('utf-8')
        for (epoch, type, value) in datapoints[name]:
            input += f'{epoch} {value}\n'.encode('utf-8')
        input += b'EOD\n'
    input += \
f'''
set terminal dumb ansi256 {x_size},{y_size}
set xdata time
set yrange[0:]
set timefmt "%s"
set key outside
set format x ""
set xtics nomirror time
set ytics nomirror
'''.encode('utf-8')

    for index, name in enumerate(datablock_names):
        prefix = 'plot' if index == 0 else ','
        input += f'{prefix} $Data{index+1} using 1:2 with line title "{name}"'.encode('utf-8')
    input += b'\n'

    try:
        res = subprocess.run(['gnuplot'], check=True, input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return res.stdout
    except subprocess.CalledProcessError as e:
        print(e, e.stdout, e.stderr)

class Gnuplot:
    def __init__(self, datapoints):
        self.datapoints = datapoints
        self.prev_widht = 0
        self.prev_height = 0
        self.prev_plot = None
    def __rich_console__(self, console, options):
        if self.prev_widht == options.max_width and self.prev_height == options.max_height and self.prev_plot != None:
            return self.prev_plot
        plot = plotv2(self.datapoints, options.max_width, options.max_height).decode('utf-8')
        plot = plot.rstrip()
        self.prev_plot = Text.from_ansi(plot)
        self.prev_width = options.max_width
        self.prev_height = options.max_height
        return self.prev_plot

def read_datapoints(db, datapoints, points, types, time_from, time_to):
    last_index = 0
    # Calculate interval per datapoint
    interval = (time_to - time_from) / points
    # Minimum supported interval is 1 ms
    if interval < 0.001:
        interval = 0.001
    options = Options()
    options.interval(interval)
    options.avg()
    while True:
        filters = Filters()
        filters.index(LIBOWL_OP_GREATER_THAN, last_index)
        for type in types:
            filters.type(LIBOWL_OP_IN, type)
        filters.epoch(LIBOWL_OP_GREATER_THAN, time_from)
        filters.epoch(LIBOWL_OP_LESS_EQUAL, time_to)
        data = db.read(filters, options, 1000)
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

def update_plot(root, datapoints, time_from, time_to):
    root['plot'].update(Gnuplot(datapoints))
    info_text = Table.grid(expand=True)
    info_text.add_column(justify='left')
    info_text.add_column(justify='right')
    info_text.add_row(
        Text(datetime.fromtimestamp(time_from).strftime('%Y-%m-%d %H:%M:%S.%f')),
        Text(datetime.fromtimestamp(time_to).strftime('%Y-%m-%d %H:%M:%S.%f')),
    )
    root['info'].update(info_text)

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
    layout["footer"].update('Akkodis Edge')

    with Live(layout, refresh_per_second=4) as live:
        while True:
            time_now = time.time()
            time_from = time_now - (60*60)
            read_datapoints(db, temp_datapoints, 100, [SENSOR_TEMPERATURE], time_from, time_now)
            update_plot(layout['temp'], temp_datapoints, time_from, time_now)
            read_datapoints(db, ratio_datapoints, 100, [SENSOR_RATIO], time_from, time_now)
            update_plot(layout['ratio'], ratio_datapoints, time_from, time_now)
            time.sleep(1)

    sys.exit(1)

if __name__ == '__main__':
    main()
