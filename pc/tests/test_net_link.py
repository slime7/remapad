"""网络调试会话的纯逻辑：UDP 地址解析与 netlog 回读行的解析。

解析错了会把用户引向连不上的地址、或在设置页显示错的状态；解析是纯函数，
与设备是否在网无关，接上设备之前就能守住。
"""

import unittest

import link
import remapadctl


class ParseEndpointTest(unittest.TestCase):
    """工具栏「网络」栏的 ip:port 解析。"""

    def test_ip_only_uses_default_port(self) -> None:
        self.assertEqual(link.parse_endpoint("192.168.1.5"), ("192.168.1.5", link.NETLOG_PORT_DEFAULT))

    def test_ip_with_port(self) -> None:
        self.assertEqual(link.parse_endpoint("192.168.1.5:6000"), ("192.168.1.5", 6000))

    def test_whitespace_tolerated(self) -> None:
        self.assertEqual(link.parse_endpoint(" 10.0.0.2 : 9998 "), ("10.0.0.2", 9998))

    def test_port_out_of_range_rejected(self) -> None:
        with self.assertRaises(ValueError):
            link.parse_endpoint("1.2.3.4:70000")
        with self.assertRaises(ValueError):
            link.parse_endpoint("1.2.3.4:0")

    def test_port_not_number_rejected(self) -> None:
        with self.assertRaises(ValueError):
            link.parse_endpoint("1.2.3.4:abc")

    def test_empty_host_rejected(self) -> None:
        with self.assertRaises(ValueError):
            link.parse_endpoint(":9999")
        with self.assertRaises(ValueError):
            link.parse_endpoint("   ")


class NetlogReplyTest(unittest.TestCase):
    """设备 netlog 状态行的回读解析（设置页摘要跟着它走）。"""

    def test_status_line_parsed(self) -> None:
        channel, fields = remapadctl.parse_device_reply(
            "netlog state=connected ssid=slime_nest dest=192.168.1.5:9999 frames=10 sent=20 dropped=0")
        self.assertEqual(channel, "netlog")
        self.assertEqual(fields["state"], "connected")
        self.assertEqual(fields["ssid"], "slime_nest")
        self.assertEqual(fields["dest"], "192.168.1.5:9999")

    def test_off_state_parsed(self) -> None:
        channel, fields = remapadctl.parse_device_reply(
            "netlog state=off ssid=- dest=- frames=0 sent=0 dropped=0")
        self.assertEqual(channel, "netlog")
        self.assertEqual(fields["state"], "off")

    def test_line_without_state_ignored(self) -> None:
        self.assertIsNone(remapadctl.parse_device_reply("netlog session stopped unexpectedly"))


class NetlogCredReplyTest(unittest.TestCase):
    """`netlog cred` 凭据回读行的解析（设置页 WiFi 输入框跟着它走）。"""

    def test_saved_credentials_parsed(self) -> None:
        channel, fields = remapadctl.parse_device_reply("ok netlog cred ssid=slime_nest pass=hunter2")
        self.assertEqual(channel, "netlog_cred")
        self.assertEqual(fields, {"ssid": "slime_nest", "pass": "hunter2"})

    def test_unconfigured_credentials_become_empty(self) -> None:
        channel, fields = remapadctl.parse_device_reply("ok netlog cred ssid=- pass=-")
        self.assertEqual(channel, "netlog_cred")
        self.assertEqual(fields, {"ssid": "", "pass": ""})

    def test_cred_line_not_misparsed_as_status(self) -> None:
        channel, _ = remapadctl.parse_device_reply("ok netlog cred ssid=slime_nest pass=hunter2")
        self.assertNotEqual(channel, "netlog")


if __name__ == "__main__":
    unittest.main()
