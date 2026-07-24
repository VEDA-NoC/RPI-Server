import ssl
import unittest
from unittest import mock

from tools import probe_rtsp_status
from tools import test_slow_rtsp_client


class RtspTestClientTests(unittest.TestCase):
    def test_status_line_parser(self):
        self.assertEqual(probe_rtsp_status.parse_status_line("RTSP/1.0 404 Not Found"), 404)
        with self.assertRaises(RuntimeError):
            probe_rtsp_status.parse_status_line("not an RTSP response")
        with self.assertRaises(RuntimeError):
            probe_rtsp_status.parse_status_line("RTSP/1.0 999 Invalid")

    def test_plain_connection_does_not_create_tls_context(self):
        raw = mock.MagicMock()
        with (
            mock.patch.object(probe_rtsp_status.socket, "create_connection", return_value=raw),
            mock.patch.object(probe_rtsp_status.ssl, "create_default_context") as create_context,
        ):
            connected = probe_rtsp_status.connect("rtsp", "127.0.0.1", 8554, 2.0, False, None)
        self.assertIs(connected, raw)
        create_context.assert_not_called()

    def test_insecure_tls_connection_wraps_socket_without_verification(self):
        raw = mock.MagicMock()
        wrapped = mock.MagicMock()
        context = mock.MagicMock()
        context.wrap_socket.return_value = wrapped
        with (
            mock.patch.object(test_slow_rtsp_client.socket, "create_connection", return_value=raw),
            mock.patch.object(test_slow_rtsp_client.ssl, "create_default_context", return_value=context),
        ):
            connected = test_slow_rtsp_client.connect("rtsps", "localhost", 8554, 2.0, True, None)
        self.assertIs(connected, wrapped)
        self.assertFalse(context.check_hostname)
        self.assertEqual(context.verify_mode, ssl.CERT_NONE)
        context.wrap_socket.assert_called_once_with(raw, server_hostname="localhost")

    def test_tls_ca_is_forwarded_to_context(self):
        raw = mock.MagicMock()
        context = mock.MagicMock()
        with (
            mock.patch.object(probe_rtsp_status.socket, "create_connection", return_value=raw),
            mock.patch.object(probe_rtsp_status.ssl, "create_default_context", return_value=context) as create_context,
        ):
            probe_rtsp_status.connect("rtsps", "pi.example", 8554, 2.0, False, "/tmp/test-ca.crt")
        create_context.assert_called_once_with(cafile="/tmp/test-ca.crt")
        context.wrap_socket.assert_called_once_with(raw, server_hostname="pi.example")


if __name__ == "__main__":
    unittest.main()
