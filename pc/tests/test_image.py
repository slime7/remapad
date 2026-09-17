"""升级镜像的本地校验：拒绝条件与提示文案。"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import remapadctl  # noqa: E402  （先把 pc/ 放进来再导入）


def fake_image(version="1.2.3", project="remapad_firmware", size=0, first_byte=0xE9,
               chip=0x0009, desc_magic=0xABCD5432):
    """拼一个满足头部约定的假镜像：首字节 + 芯片标识 + 应用描述符。"""
    size = size or remapadctl.APP_DESC_PROJECT_OFFSET + 64
    data = bytearray(size)
    data[0] = first_byte
    data[remapadctl.ESP_CHIP_ID_OFFSET:remapadctl.ESP_CHIP_ID_OFFSET + 2] = chip.to_bytes(2, "little")
    data[remapadctl.APP_DESC_OFFSET:remapadctl.APP_DESC_OFFSET + 4] = desc_magic.to_bytes(4, "little")
    data[remapadctl.APP_DESC_VERSION_OFFSET:
         remapadctl.APP_DESC_VERSION_OFFSET + len(version)] = version.encode()
    data[remapadctl.APP_DESC_PROJECT_OFFSET:
         remapadctl.APP_DESC_PROJECT_OFFSET + len(project)] = project.encode()
    return bytes(data)


class LoadImageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, data: bytes) -> Path:
        path = self.dir / "image.bin"
        path.write_bytes(data)
        return path

    def test_accepts_valid_image_and_reads_version(self):
        path = self.write(fake_image(version="52b8bad-dirty"))
        data, version = remapadctl.load_image(path)
        self.assertEqual(version, "52b8bad-dirty")
        self.assertEqual(len(data), remapadctl.APP_DESC_PROJECT_OFFSET + 64)

    def test_rejects_missing_file(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.dir / "nope.bin")
        self.assertIn("读不到镜像", str(caught.exception))

    def test_rejects_truncated_file(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(b"\xe9\x00\x00"))
        self.assertIn("不是应用镜像", str(caught.exception))

    def test_rejects_foreign_first_byte(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(fake_image(first_byte=0x42)))
        self.assertIn("首字节", str(caught.exception))

    def test_rejects_other_chip(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(fake_image(chip=0x0002)))
        self.assertIn("不是 ESP32-S3", str(caught.exception))

    def test_rejects_missing_app_descriptor(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(fake_image(desc_magic=0x12345678)))
        self.assertIn("缺少应用描述符", str(caught.exception))

    def test_rejects_other_project(self):
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(fake_image(project="other_app")))
        self.assertIn("本设备只接受 remapad_firmware", str(caught.exception))

    def test_rejects_image_over_partition_size(self):
        oversized = fake_image(size=remapadctl.PARTITION_MAX_BYTES + 1)
        with self.assertRaises(remapadctl.ImageError) as caught:
            remapadctl.load_image(self.write(oversized))
        self.assertIn("超过应用分区容量", str(caught.exception))


if __name__ == "__main__":
    unittest.main()

