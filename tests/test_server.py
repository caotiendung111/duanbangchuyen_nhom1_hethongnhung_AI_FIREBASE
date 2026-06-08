import sys
from unittest.mock import MagicMock, patch

# Inject mock modules into sys.modules to prevent actual Firebase/YOLO load during tests
sys.modules['firebase_admin'] = MagicMock()
sys.modules['ultralytics'] = MagicMock()

import pytest
import socket
from main import CLASS_MAP, CLASS_NAME, TCPSerial

def test_class_mappings():
    """Verifies that classification IDs correctly map to abbreviations and names."""
    assert CLASS_MAP[0] == 'A'
    assert CLASS_MAP[1] == 'B'
    assert CLASS_MAP[2] == 'O'
    assert CLASS_MAP[3] == 'M'
    
    assert CLASS_NAME['A'] == 'Apple'
    assert CLASS_NAME['B'] == 'Banana'
    assert CLASS_NAME['O'] == 'Orange'
    assert CLASS_NAME['M'] == 'Milk'
    assert CLASS_NAME['U'] == 'Unknown'

def test_tcp_serial_read_parsing():
    """Verifies that TCPSerial correctly reads lines from mock socket chunks."""
    mock_sock = MagicMock()
    
    # Configure mock socket to return chunks ended by newline
    mock_sock.recv.side_effect = [b"CAPTURE\n", b""]
    
    tcp_serial = TCPSerial(mock_sock)
    
    # Sleep briefly to allow background thread to process mock data
    import time
    time.sleep(0.3)
    
    assert tcp_serial.in_waiting == 1
    assert tcp_serial.readline() == b"CAPTURE\n"
    
    tcp_serial.close()

def test_tcp_serial_write():
    """Verifies that TCPSerial write delegates to socket sendall."""
    mock_sock = MagicMock()
    tcp_serial = TCPSerial(mock_sock)
    
    tcp_serial.write(b"A")
    mock_sock.sendall.assert_called_once_with(b"A")
    
    tcp_serial.close()

def test_tcp_serial_close():
    """Verifies that closing TCPSerial closes underlying socket."""
    mock_sock = MagicMock()
    tcp_serial = TCPSerial(mock_sock)
    
    tcp_serial.close()
    mock_sock.close.assert_called_once()
