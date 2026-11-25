"""
SRR: Synchronous Send / Receive / Reply client library.
Pure Python standard library implementation with zero third-party dependencies.
"""

from .protocol import (
    SRR_WIRE_MAGIC,
    SRR_WIRE_VERSION,
    SRR_MSG_HELLO,
    SRR_MSG_REQUEST,
    SRR_MSG_REPLY,
    SRR_MSG_ERROR,
    SRR_MSG_PULSE,
    SRR_MSG_CANCEL,
    SRR_MSG_GOODBYE,
    srr_crc16,
)
from .client import SRRClient, SRRUDPClient, SRRResolver

__version__ = "0.1.0"
__all__ = [
    "SRRClient",
    "SRRUDPClient",
    "SRRResolver",
    "SRR_WIRE_MAGIC",
    "SRR_WIRE_VERSION",
    "SRR_MSG_HELLO",
    "SRR_MSG_REQUEST",
    "SRR_MSG_REPLY",
    "SRR_MSG_ERROR",
    "SRR_MSG_PULSE",
    "SRR_MSG_CANCEL",
    "SRR_MSG_GOODBYE",
    "srr_crc16",
]
