#!/usr/bin/python3

import sys
import argparse
import time
import serial

# Send command and return string until "OK".
def get_at_result(stream, command, max_response_time, max_retries):
    attempt = 0
    history = b''
    while attempt < max_retries:
        stream.write(command)
        start = time.monotonic()
        buf = b''
        while (time.monotonic() - start) < max_response_time:
            buf += stream.read(128)
            begin = buf.find(b'OK\r')
            if begin >= 0:
                return buf[:begin]
            time.sleep(0.1)
        history += buf
        attempt += 1
    raise RuntimeError('Timeout waiting for {}: {}'.format(command, history))

def strip_command_from_msg(command, msg):
    begin = msg.find(command)
    if begin < 0:
        raise RuntimeError('Invalid: {}'.format(msg))
    # Skip to end of request
    begin += len(command)
    value = msg[begin:]
    if len(value) < 1:
        raise RuntimeError('Invalid: {}'.format(msg))
    return value

def open_modem(device):
    attempt = 0
    while True:
        try:
            stream = serial.Serial(device, 115200, timeout=0, exclusive=True)
            return stream
        except serial.SerialException as e:
            attempt += 1
            if attempt > 20:
                raise
            time.sleep(0.1)

def main():
    parser = argparse.ArgumentParser(description='AT modem cli')
    parser.add_argument('--device', required=True, help='Path to AT device')
    parser.add_argument('--quectel-gps', action='store_true', help='Enable quectel gps')
    parser.add_argument('--no-quectel-gps', action='store_true', help='Disable quectel gps')
    parser.add_argument('--quectel-nvread', help='Read non volatile data')
    parser.add_argument('--quectel-nvwrite', nargs=2, help='Read non volatile data')
    parser.add_argument('--imsi', action='store_true', help='Read SIM IMSI')
    parser.add_argument('--imei', action='store_true', help='Read modem IMEI')
    parser.add_argument('--iccid', action='store_true', help='Read SIM ICCID')
    parser.add_argument('--quectel-firmware', action='store_true', help='Read quectel firmware version')
    args = parser.parse_args()

    if not args.device:
        print('invalid argument: --device mandatory')
        sys.exit(1)
    if args.quectel_gps and args.no_quectel_gps:
        print('invalid argument: --quectel-gps and --no-quectel-gps are mutually exclusive')
        sys.exit(1)
    if not args.quectel_gps and not args.no_quectel_gps \
        and not args.quectel_nvread and not args.quectel_nvwrite \
        and not args.imsi and not args.imei and not args.iccid \
        and not args.quectel_firmware:
        print('invalid argument: no action provided')
        sys.exit(1)

    with open_modem(args.device) as stream:
        # Make sure responds to AT
        get_at_result(stream, b'at\r', 0.3, 5)

        if args.quectel_gps or args.no_quectel_gps:
            # enable/disable only supported from respective state
            msg = get_at_result(stream, b'at+qgps?\r', 0.3, 5)
            quectel_gps_state = strip_command_from_msg(b'+QGPS:', msg).decode().strip()
            print("quectel-gps state: {}".format(quectel_gps_state))
            if args.quectel_gps and quectel_gps_state == '0':
                print('quectel-gps activate')
                get_at_result(stream, b'at+qgps=1\r', 5, 1)
            if args.no_quectel_gps and quectel_gps_state =='1':
                print('quectel-gps disable')
                get_at_result(stream, b'at+qgpsend\r', 5, 1)

        if args.quectel_nvread:
            req = b'at+qnvfr="' + args.quectel_nvread.encode() + b'"\r'
            msg = get_at_result(stream, req, 5, 1)
            value = strip_command_from_msg(b'+QNVFR:', msg).decode().strip()
            print(value)

        if args.quectel_nvwrite:
            req = b'at+qnvfw="' + args.quectel_nvwrite[0].encode() + b'",' + args.quectel_nvwrite[1].encode() + b'\r'
            get_at_result(stream, req, 5, 1)

        if args.imsi:
            req = b'at+cimi\r'
            msg = get_at_result(stream, req, 0.3, 5)
            value = strip_command_from_msg(req, msg).decode().strip()
            print(value)

        if args.imei:
            req = b'at+gsn\r'
            msg = get_at_result(stream, req, 0.3, 5)
            value = strip_command_from_msg(req, msg).decode().strip()
            print(value)

        if args.iccid:
            msg = get_at_result(stream, b'at+qccid\r', 0.3, 5)
            value = strip_command_from_msg(b'+QCCID:', msg).decode().strip()
            print(value)

        if args.quectel_firmware:
            req = b'at+qgmr\r'
            msg = get_at_result(stream, req, 1, 5)
            value = strip_command_from_msg(req, msg).decode().strip()
            print(value)

    sys.exit(0)

if __name__ == '__main__':
    try:
        main()
    except Exception as e:
        print(e)
        sys.exit(1)
