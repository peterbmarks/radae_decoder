#Test code for spotting on FreeDV Reporter
# Contributed by Joe, VK3SRC

from time import time
from socketio import Client

class FreeDVReporter():
    #The default configuration for test purposes only
    def __init__(self,
                role='report',
                callsign= 'VK3YSP',
                grid_square = 'QF22NC',
                frequency = 7083000,
                mode = 'RADEV1',
                status = 'TX',
                message = 'Testing FreeDV upload only',
                version = 'PyFreeDV 1.0',
                url = 'https://qso.freedv.org:443',
                rx_only = False,
                os = 'Windows 11',
                protcol_version = 2):
        self.role = role
        self.callsign = callsign
        self.grid_square = grid_square
        self.frequency = frequency
        self.mode = mode
        self.status = status
        self.message = message
        self.version = version
        self.url = url
        self.rx_only = rx_only
        self.os = os
        self.protcol_version = protcol_version
        self.sio = Client(logger=False, engineio_logger=False)
        #Register socketio callbacks
        self.sio.on('connect', self.on_connect)
        self.sio.on('connect_error', self.on_connect_error)
        self.sio.on('connection_successful', self.on_connection_successful)
        self.sio.on('disconnect', self.on_disconnect)
        self.connected = False

    def wait(self, seconds):
        #Efficiently wait for a number of seconds
        deadline = time() + seconds
        while time() < deadline:
            self.sio.sleep(0.1)

    def wait_for_connection(self, seconds):
        #Efficiently wait for a socketio connection for a number of seconds
        deadline = time() + seconds
        while time() < deadline:
            self.sio.sleep(0.1)
            if self.connected:
                break

    #Define socketio callbacks for various connection events
    def on_connect(self):
        print('Connected')

    def on_connect_error(self, data):
        print('Connection Error')

    def on_connection_successful(self, *_args):
        self.connected = True
        print('Connection Successful')

    def on_disconnect(self):
        print('Disconnected')

    def connect(self, timeout=5):
        #Connect to FreeDV reporter using TLS
        try:
            self.sio.connect(
                self.url,
                auth = {
                    'role': self.role,
                    'callsign': self.callsign,
                    'grid_square': self.grid_square,
                    'version': self.version,
                    'rx_only': self.rx_only,
                    'os': self.os,
                    'protocol_version': self.protcol_version,
                },
                transports=['websocket'],
                socketio_path='/socket.io',
                wait_timeout=10,
            )
        except Exception as exc:
            print(f'Connection Error: {exc}')
            self.connected = False

        self.wait_for_connection(timeout) #Wait for a connection for up to timeout seconds

        if self.connected:
            #Set the radio frequency
            self.set_frequency(self.frequency)

            #Set the text message
            self.set_message(self.message)

    def set_frequency(self, frequency):
        #Set the radio frequency in Hz
        self.sio.emit('freq_change', {'freq': frequency})
       
    def set_message(self, message):
        #Set the text message
        self.sio.emit('message_update', {'message': message})
       
    def disconnect(self):
        #Disconnect the from FreeDV Reporter
        self.sio.disconnect()

    def send_rx(self, callsign, snr):
        #Send a receive update
        self.sio.emit('rx_report', {'callsign': callsign, 'mode': self.mode, 'snr': snr,})
       
    def send_tx(self, tx):
        #Send a transmit update
        self.sio.emit('tx_report', {'mode': self.mode, 'transmitting': tx,})

if __name__ == '__main__':
    #Test script only
    f = FreeDVReporter()                #Create A FreeDV Reporter object using the default configuration
    f.connect(timeout=5)                #Connect to https://qso.freedv.org with a 5-second timeout
    if f.connected:                     #The connection was successful
        while True:                     #Run this test sequence until CTRL-C is pressed
            try:
                f.send_rx('', 0)        #The VK3YSP row appears green with no RX Call, SNR = 0 and goes white after 5 seconds
                f.wait(10)
                f.send_rx('VK3SRC', 20) #The row appears green for 5 seconds with RX Call = VK3SRC and SNR = 20
                f.wait(5)
                f.send_tx(True)         #The row appears red for 2 seconds with SNR = 20
                f.wait(2)
                f.send_tx(False)        #The row appears green
            except KeyboardInterrupt:
                break
        f.disconnect()
    else:
        print('Could not connect')
