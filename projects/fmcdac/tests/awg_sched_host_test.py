import struct
import unittest
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from awg_sched_host import (
    AWG_EVENT_V1_SIZE,
    AWG_SCHED_FLAG_PHASE_REINIT,
    AWG_STREAM_PROTO_MAGIC,
    AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF,
    AWG_STREAM_PROTO_FLAG_OPEN,
    AwgSchedEvent,
    build_awg_sweep_events,
    build_uniform_freq_list,
    pack_events,
    pack_stream_frame,
    pack_awg_payload_v1,
    parse_info_line,
    parse_last_artifact_block,
    parse_stream_ack_line,
    parse_stream_status_line,
    stream_crc32_ieee,
    unpack_stream_ack,
)
from run_nco_scope_test import (
    RfPowerCalibration,
    SpectrumMetrics,
    apply_scheduler_fsh_marker_flatline_guard,
    apply_scheduler_fsh_missing_thresholds,
    apply_spectrum_power_calibration,
    build_scheduler_batch_specs,
    chunk_sequence,
    extract_maxhold_bins_from_trace,
    interpolate_rf_power_correction,
    resolve_scheduler_fsh_capture_geometry,
    select_scheduler_fsh_calibration_candidate,
    summarize_scheduler_dense_rf_quality,
    write_scheduler_fsh_maxhold_plot_svg,
)


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

    def test_parse_stream_artifact_line(self) -> None:
        text = "\n".join(
            [
                "[AWG-UART] ARTIFACT_BEGIN",
                "[SCHED-ARTIFACT] stream depth=511 low_wmark=127 ctrl=0x00000005 occupancy=0 free_space=511 pushes=12 stalls=3 irq=0x00000011 err=0x00000000",
                "[AWG-UART] ARTIFACT_END",
            ]
        )
        artifact = parse_last_artifact_block(text)
        self.assertIsNotNone(artifact.stream)
        self.assertEqual(artifact.stream.stream_depth, 511)
        self.assertEqual(artifact.stream.stream_pushes, 12)

    def test_pack_stream_frame_crc(self) -> None:
        event = AwgSchedEvent(
            timestamp_ticks=1000,
            channel=0,
            flags=AWG_SCHED_FLAG_PHASE_REINIT,
            payload_word0=0x12345678,
            payload_word1=0,
            payload_word2=0,
            payload_word3=0,
        )
        frame = pack_stream_frame([event], seq=0, open_stream=True, close_with_eof=True)
        self.assertEqual(len(frame), 12 + AWG_EVENT_V1_SIZE + 4)
        self.assertEqual(int.from_bytes(frame[0:4], "little"), AWG_STREAM_PROTO_MAGIC)
        self.assertEqual(int.from_bytes(frame[4:8], "little"), 0)
        self.assertEqual(int.from_bytes(frame[8:10], "little"), 1)
        flags = int.from_bytes(frame[10:12], "little")
        self.assertEqual(flags, AWG_STREAM_PROTO_FLAG_OPEN | AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF)
        self.assertEqual(
            int.from_bytes(frame[-4:], "little"),
            stream_crc32_ieee(frame[:-4]),
        )

    def test_parse_stream_ack_line(self) -> None:
        ack = parse_stream_ack_line(
            "[AWG-STREAM] ACK magic=0x53415747 seq=3 ddr_free=4096 status=0 "
            "stream_free=511 stalls=2 irq=0x00000011 ret=0 bytes=48 events=1 flags=0x0003"
        )
        self.assertEqual(ack.magic, AWG_STREAM_PROTO_MAGIC)
        self.assertEqual(ack.seq_acked, 3)
        self.assertEqual(ack.status_name, "ok")
        self.assertEqual(ack.stream_free_events, 511)

    def test_unpack_stream_wire_ack(self) -> None:
        wire = struct.pack("<IIIIIII", AWG_STREAM_PROTO_MAGIC, 9, 100, 0, 511, 2, 0x11)
        ack = unpack_stream_ack(wire)
        self.assertTrue(ack.ok)
        self.assertEqual(ack.seq_acked, 9)
        self.assertEqual(ack.stream_free_events, 511)

    def test_open_sequence_and_empty_close_rejected(self) -> None:
        event = AwgSchedEvent(1, 0, 0, 0, 0, 0, 0)
        with self.assertRaises(ValueError):
            pack_stream_frame([event], seq=1, open_stream=True)
        with self.assertRaises(ValueError):
            pack_stream_frame([], seq=1, close_with_eof=True)

    def test_parse_stream_status_line(self) -> None:
        status = parse_stream_status_line(
            "[AWG-STREAM] STATUS tag=status ip_id=0x41574753 ip_version=0x00010000 "
            "stream_depth=511 low_wmark=127 stream_ctrl=0x00000005 occupancy=0 free_space=511 "
            "stream_pushes=12 stream_stalls=1 commit=12 err=0x00000000 irq=0x00000011 "
            "hw_status=0x00000008 mode=1 overflow=0 eof_seen=1 running=0 done=1 error=0"
        )
        self.assertEqual(status.stream_depth, 511)
        self.assertTrue(status.done)
        self.assertTrue(status.eof_seen)

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

    def test_scheduler_fsh_geometry_defaults(self) -> None:
        geometry = resolve_scheduler_fsh_capture_geometry(
            [200_000_000.0, 201_000_000.0, 202_000_000.0],
            rbw_hz=100_000.0,
            span_pad_hz=None,
            bin_window_hz=None,
        )
        self.assertEqual(geometry.span_left_hz, 198_000_000.0)
        self.assertEqual(geometry.span_right_hz, 204_000_000.0)
        self.assertEqual(geometry.bin_half_width_hz, 450_000.0)

    def test_extract_maxhold_bins_from_trace(self) -> None:
        bins = extract_maxhold_bins_from_trace(
            [199_900_000.0, 200_010_000.0, 201_020_000.0, 202_500_000.0],
            [-80.0, -10.0, -11.5, -65.0],
            [200_000_000.0, 201_000_000.0, 202_000_000.0],
            100_000.0,
            missing_relative_db=30.0,
        )
        self.assertFalse(bins[0]["missing"])
        self.assertFalse(bins[1]["missing"])
        self.assertTrue(bins[2]["missing"])

    def test_maxhold_bins_mark_outside_frequency_missing(self) -> None:
        bins = [
            {
                "index": 1,
                "expected_hz": 200_000_000.0,
                "left_hz": 199_750_000.0,
                "right_hz": 200_250_000.0,
                "power_dbm": -20.0,
                "power_freq_hz": 208_822_222.22,
                "freq_error_hz": 8_822_222.22,
                "missing": False,
                "missing_reason": "",
            }
        ]
        apply_scheduler_fsh_missing_thresholds(
            bins,
            min_power_dbm=None,
            missing_relative_db=30.0,
        )
        self.assertTrue(bins[0]["missing"])
        self.assertIn("outside_bin", bins[0]["missing_reason"])

    def test_maxhold_bins_mark_nonfinite_power_missing(self) -> None:
        bins = [
            {
                "index": 1,
                "expected_hz": 200_000_000.0,
                "left_hz": 199_750_000.0,
                "right_hz": 200_250_000.0,
                "power_dbm": float("nan"),
                "power_freq_hz": 200_000_000.0,
                "freq_error_hz": 0.0,
                "missing": False,
                "missing_reason": "",
            }
        ]

        apply_scheduler_fsh_missing_thresholds(
            bins,
            min_power_dbm=None,
            missing_relative_db=30.0,
        )
        self.assertTrue(bins[0]["missing"])
        self.assertIn("nonfinite_power", bins[0]["missing_reason"])

    def test_maxhold_bins_mark_fsh_invalid_power_sentinel_missing(self) -> None:
        bins = [
            {
                "index": 1,
                "expected_hz": 200_000_000.0,
                "left_hz": 199_750_000.0,
                "right_hz": 200_250_000.0,
                "power_dbm": 9.91e37,
                "power_freq_hz": 200_000_000.0,
                "freq_error_hz": 0.0,
                "missing": False,
                "missing_reason": "",
            }
        ]

        apply_scheduler_fsh_missing_thresholds(
            bins,
            min_power_dbm=None,
            missing_relative_db=30.0,
        )
        self.assertTrue(bins[0]["missing"])
        self.assertIn("invalid_power_sentinel", bins[0]["missing_reason"])

    def test_maxhold_plot_handles_identical_powers(self) -> None:
        bins = [
            {
                "index": 1,
                "expected_hz": 200_000_000.0,
                "power_dbm": -87.64,
                "missing": False,
            },
            {
                "index": 2,
                "expected_hz": 201_000_000.0,
                "power_dbm": -87.64,
                "missing": False,
            },
        ]

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "plot.svg"
            write_scheduler_fsh_maxhold_plot_svg(path, bins, "identical")
            text = path.read_text(encoding="utf-8")
        self.assertIn("<svg", text)
        self.assertIn("identical", text)

    def test_marker_flatline_guard_marks_floor_readout_missing(self) -> None:
        bins = [
            {
                "index": index,
                "expected_hz": 200_000_000.0 + index * 1_000_000.0,
                "power_dbm": -84.27,
                "freq_error_hz": 0.0,
                "missing": False,
                "missing_reason": "",
                "readout_mode": "marker",
            }
            for index in range(1, 5)
        ]

        changed = apply_scheduler_fsh_marker_flatline_guard(
            bins,
            floor_power_dbm=-60.0,
            epsilon_db=0.02,
        )
        self.assertTrue(changed)
        self.assertTrue(all(item["missing"] for item in bins))
        self.assertTrue(all("marker_flatline_untrusted" in item["missing_reason"] for item in bins))

    def test_marker_flatline_guard_allows_strong_flat_readout(self) -> None:
        bins = [
            {
                "index": index,
                "expected_hz": 200_000_000.0 + index * 1_000_000.0,
                "power_dbm": -10.0,
                "freq_error_hz": 0.0,
                "missing": False,
                "missing_reason": "",
                "readout_mode": "marker",
            }
            for index in range(1, 5)
        ]

        changed = apply_scheduler_fsh_marker_flatline_guard(
            bins,
            floor_power_dbm=-60.0,
            epsilon_db=0.02,
        )
        self.assertFalse(changed)
        self.assertTrue(all(not item["missing"] for item in bins))

    def test_scheduler_dense_rf_quality_flags_peak_marker_delta(self) -> None:
        class Metrics:
            power_dbm = -80.0
            marker_power_dbm = -95.0
            nearest_error_hz = 10_000.0

        class Summary:
            name = "step"
            expected_freq_hz = [200_000_000.0]
            metrics = Metrics()

        summary = summarize_scheduler_dense_rf_quality(
            [Summary()],
            max_freq_error_hz=500_000.0,
            min_power_dbm=None,
            max_flatness_db=6.0,
            max_peak_marker_delta_db=6.0,
        )
        self.assertFalse(summary["passed"])
        self.assertEqual(summary["failures"][0]["criterion"], "max_peak_marker_delta_db")

    def test_rf_power_correction_interpolates_and_marks_metrics(self) -> None:
        calibration = RfPowerCalibration(
            enabled=True,
            fixed_correction_db=1.0,
            table_path="",
            table_points=[(100.0, 2.0), (200.0, 4.0)],
            label="bench",
            note="unit test",
        )
        self.assertAlmostEqual(interpolate_rf_power_correction(calibration.table_points, 150.0), 3.0)
        metrics = SpectrumMetrics(
            trace_points=1,
            center_hz=150.0,
            span_hz=10.0,
            search_left_hz=145.0,
            search_right_hz=155.0,
            rbw_hz=1.0,
            vbw_hz=1.0,
            sweep_count=1,
            trace_mode="clearwrite",
            detector="positive",
            reference_level_dbm=0.0,
            display_range_db=80.0,
            attenuation_auto=True,
            preamp_on=False,
            impedance_ohms=50,
            power_dbm=-20.0,
            power_freq_hz=150.0,
            marker_power_dbm=-21.0,
            marker_freq_hz=150.0,
            trace_peak_power_dbm=-20.0,
            trace_peak_freq_hz=150.0,
            nearest_expected_hz=150.0,
            nearest_error_hz=0.0,
        )
        apply_spectrum_power_calibration(metrics, calibration)
        self.assertAlmostEqual(metrics.power_correction_db, 4.0)
        self.assertAlmostEqual(metrics.corrected_power_dbm, -16.0)

    def test_scheduler_dense_rf_quality_uses_corrected_power_when_present(self) -> None:
        class Metrics:
            power_dbm = -80.0
            corrected_power_dbm = -70.0
            power_correction_db = 10.0
            marker_power_dbm = -85.0
            corrected_marker_power_dbm = -75.0
            marker_power_correction_db = 10.0
            nearest_error_hz = 10_000.0

        class Summary:
            name = "step"
            expected_freq_hz = [200_000_000.0]
            metrics = Metrics()

        summary = summarize_scheduler_dense_rf_quality(
            [Summary()],
            max_freq_error_hz=500_000.0,
            min_power_dbm=-75.0,
            max_flatness_db=6.0,
            max_peak_marker_delta_db=6.0,
        )
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["rows"][0]["power_dbm"], -70.0)
        self.assertEqual(summary["rows"][0]["raw_power_dbm"], -80.0)

    def test_select_scheduler_fsh_calibration_candidate(self) -> None:
        selected = select_scheduler_fsh_calibration_candidate(
            [
                {"candidate_index": 1, "passed": False, "estimated_seconds": 1.0, "rbw_hz": 30_000.0},
                {"candidate_index": 2, "passed": True, "estimated_seconds": 5.0, "rbw_hz": 100_000.0},
                {"candidate_index": 3, "passed": True, "estimated_seconds": 2.0, "rbw_hz": 300_000.0},
            ]
        )
        self.assertIsNotNone(selected)
        self.assertEqual(selected["candidate_index"], 3)


if __name__ == "__main__":
    unittest.main()
