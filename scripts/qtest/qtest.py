# QEMU qtest library
#
# Copyright (C) 2014 Red Hat Inc.
#
# Authors:
#  Fam Zheng <famz@redhat.com>
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.
#
# Based on qmp.py.
#

import errno
import socket

class QEMUQtestProtocol:
    def __init__(self, address, server=False):
        """
        Create a QEMUQtestProtocol object.

        @param address: QEMU address, can be either a unix socket path (string)
                        or a tuple in the form ( address, port ) for a TCP
                        connection
        @param server: server mode listens on the socket (bool)
        @raise socket.error on socket connection errors
        @note No connection is established, this is done by the connect() or
              accept() methods
        """
        self.__address = address
        self.__sock = self.__get_sock()
        if server:
            self.__sock.bind(self.__address)
            self.__sock.listen(1)

    def __get_sock(self):
        if isinstance(self.__address, tuple):
            family = socket.AF_INET
        else:
            family = socket.AF_UNIX
        return socket.socket(family, socket.SOCK_STREAM)

    def connect(self):
        """
        Connect to the qtest socket.

        @raise socket.error on socket connection errors
        """
        self.__sock.connect(self.__address)
        self.__sockfile = self.__sock.makefile()

    def accept(self):
        """
        Await connection from QEMU.

        @raise socket.error on socket connection errors
        """
        self.__sock, _ = self.__sock.accept()
        self.__sockfile = self.__sock.makefile()

    def cmd(self, qtest_cmd):
        """
        Send a qtest command on the wire.

        @param qtest_cmd: qtest command text to be sent
        """
        self.__sock.sendall(qtest_cmd + "\n")

    def close(self):
        self.__sockfile.close()
        self.__sock.close()

    def settimeout(self, timeout):
        self.__sock.settimeout(timeout)
