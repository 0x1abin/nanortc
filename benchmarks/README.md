# CPU benchmarks

Build the optional target with the same compiler, crypto backend and feature flags for both revisions:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_TESTS=OFF \
  -DNANORTC_BUILD_BENCHMARKS=ON -DNANORTC_FEATURE_AUDIO=ON \
  -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON
cmake --build build-bench --target nanortc_benchmark
./build-bench/nanortc_benchmark
```

The program warms each operation, then measures 11 batches of 2,000 iterations using process CPU time. It prints the median and p95 of batch averages; these are not individual-packet tail latencies. The video workload sends a 12,000-byte Annex B access unit containing two parameter NALs and a large slice, enables TWCC extension ID 3, protects the packets with SRTP, drains output, and reports actual sends through the 0.2 API. The output-only workload copies and drains `NANORTC_TX_SLOT_COUNT` packets per iteration. SCTP measures DATA encoding and CRC verification; it does not simulate a network.

For comparisons, pin both executables to the same available CPU (for example, `taskset -c 19 ...` on the host below), alternate their run order, and repeat. Keep raw output; compiler, feature flags, frame size, crypto backend and state size are printed with each run. Host results do not predict embedded frame rates or end-to-end latency.

## 0.2 measurement

Measured against the preserved baseline `9af5849` executable using the same benchmark source: Intel Core i5-14600K, Linux x86_64, CPU 19 affinity, GCC 13.3.0, OpenSSL 3.0.13, Release, DC/AUDIO/VIDEO/H265 enabled, FEC disabled. The final library was rebuilt with `-fstack-usage`. Values below are the median of three run medians; pair order was before/after, after/before, before/after. All six runs produced checksum `817864513`.

| Operation | Before | After | CPU time change |
| --- | ---: | ---: | ---: |
| Copy, enqueue and drain four outputs | 195.0 ns | 229.0 ns | +17.44% |
| RTP pack and SRTP protect | 2.4025 µs | 2.4165 µs | +0.58% |
| SCTP encode and CRC verification | 3.9685 µs | 3.9695 µs | +0.03% |
| H.264 access unit, SRTP and drain | 48.5615 µs | 42.6520 µs | −12.17% |
| H.265 access unit, SRTP and drain | 40.8900 µs | 40.7815 µs | −0.27% |

The output workload includes four new transmit reports after the change; its increase is 34 ns per four-packet batch. The final media path also includes send-result bookkeeping and transport history. H.264 reuses Annex B lookahead. H.265 writes AP/FU payloads into their final RTP slots, but its complete send path was essentially unchanged in CPU time in this measurement: run medians ranged from 40.307–41.163 µs before and 40.484–41.068 µs after. The small SRTP and SCTP differences likewise do not establish a meaningful performance change.

With GCC `-O3 -fstack-usage`, the former H.265 AP/FU helper frames were each 1,312 bytes; the new iterator's `next` frame is 128 bytes. The final direct send path has frames of 304 bytes for `nanortc_send_video` (previously 576), 496 bytes for `rtc_send_video_h265`, and 128 bytes for `h265_packet_iter_next`. These are individual function frames, not the total call-chain or task-stack requirement. The compatibility callback wrapper `h265_packetize_au` retains scratch storage and has a 1,376-byte frame; the direct send path does not call it.

The local validation directory `.cache/validation02/benchmark/` contains `commands.txt`, all six raw logs under `logs/`, `summary.json` with every run's median and p95 batch average, source and executable SHA-256 digests, and the before/after compiler stack reports. The source digests were unchanged across the measurement.
