import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from awg_sched_host import (
    AWG_EVENT_V1_SIZE,
    AWG_SCHED_FLAG_PHASE_REINIT,
    AwgSchedEvent,
    build_awg_sweep_events,
    build_uniform_freq_list,
    pack_events,
    pack_awg_payload_v1,
    parse_info_line,
    parse_last_artifact_block,
)
from run_nco_scope_test import build_scheduler_batch_specs, chunk_sequence


class AwgSchedHostTest(unittest.TestCase):
    def test_event_pack_size(self) -> None:
        event = AwgSchedEvent(
            timestamp_ticks=1000,
            channel=0,
            flags=AWG_SCHED_FLAG_PHASE_REINIT,
            payload_word0=0x12345678,
            payload_word1=0x9ABCDEF0,
            payload_word2=0x11112222,
            payload_word3=0x33334444,
        )
        packed = event.pack()
        self.assertEqual(len(packed), AWG_EVENT_V1_SIZE)

    def test_pack_events_multiple(self) -> None:
        events = build_awg_sweep_events(
            build_uniform_freq_list(200_000_000, 202_000_000, 1_000_000),
            tick_hz=1_000_000,
            dds_clock_hz=983_056_640,
            dds_phase_dw=32,
            tone=0,
            scale_u=700_000,
            start_ticks=1000,
            dwell_us=1000,
        )
        packed = pack_events(events)
        self.assertEqual(len(events), 3)
        self.assertEqual(len(packed), 3 * AWG_EVENT_V1_SIZE)

    def test_parse_info_line(self) -> None:
        info = parse_info_line(
            "[AWG-UART] INFO base=0x44AA0000 max_events=64 tick_hz=1000000 "
            "timeout_ms=2000 dds_clock_hz=983056640 dds_phase_dw=32 loaded=0 configured=1"
        )
        self.assertEqual(info.base_addr, 0x44AA0000)
        self.assertEqual(info.max_events, 64)
        self.assertEqual(info.tick_hz, 1_000_000)
        self.assertEqual(info.dds_clock_hz, 983_056_640)
        self.assertTrue(info.configured)

    def test_parse_last_artifact_block(self) -> None:
        text = "\n".join(
            [
                "[AWG-UART] ARTIFACT_BEGIN",
                "[SCHED-ARTIFACT] config base=0x44AA0000 max_events=64 tick_hz=1000000 timeout_ms=2000",
                "[SCHED-ARTIFACT] event idx=0 ts=0x00000000_000003E8 ch=0 fl=0x0001 p0=0x12345678 p1=0x00001234 p2=0x00005678 p3=0x00000000",
                "[SCHED-ARTIFACT] status armed=1 running=0 done=1 error=0 err_code=0x00 current=1 loaded=1 commit=1 reinit=0 reinit_reject=0 irq=0x00000001",
                "[SCHED-ARTIFACT] time_now=0x00000000_00000400 last_exec=0x00000000_000003E8",
                "[AWG-UART] ARTIFACT_END",
            ]
        )
        artifact = parse_last_artifact_block(text)
        self.assertIsNotNone(artifact.config)
        self.assertEqual(len(artifact.events), 1)
        self.assertIsNotNone(artifact.status)
        self.assertTrue(artifact.status.done)
        self.assertEqual(artifact.status.commit_count, 1)
        self.assertIsNotNone(artifact.time)

    def test_pack_awg_payload_v1_32bit(self) -> None:
        word0, word1, word2, word3 = pack_awg_payload_v1(
            scale=0x1234,
            init=0x89ABCDEF,
            incr=0x13579BDF,
            dds_phase_dw=32,
        )
        self.assertEqual(word0, 0xCDEF1234)
        self.assertEqual(word1, 0x9BDF89AB)
        self.assertEqual(word2, 0x00001357)
        self.assertEqual(word3, 0x00000000)

    def test_chunk_sequence(self) -> None:
        self.assertEqual(chunk_sequence([1, 2, 3, 4, 5], 2), [[1, 2], [3, 4], [5]])

    def test_build_scheduler_batch_specs(self) -> None:
        specs = build_scheduler_batch_specs(
            [200_000_000.0, 201_000_000.0, 202_000_000.0, 203_000_000.0, 204_000_000.0],
            2,
        )
        self.assertEqual(len(specs), 3)
        self.assertEqual(specs[0].start_index, 0)
        self.assertEqual(specs[1].start_index, 2)
        self.assertEqual(specs[2].start_index, 4)
        self.assertEqual(specs[2].freqs_hz, [204_000_000.0])


if __name__ == "__main__":
    unittest.main()
