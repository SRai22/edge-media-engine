# Architecture (as built, M1)

```text
SyntheticVideoSource ──Distributor<VideoFrame>──▶ EncoderNode(IVideoEncoder) ──Distributor<EncodedVideoFrame>──┬─▶ sink / p2p peer
   (FramePool, never blocks)    DropOldest q=2       (1 worker, keyframe rate-limit)     DropUntilKeyframe q=N each ├─▶ SFU publisher
                                                                                                                └─▶ ...
```

| Concept | Header | Notes |
|---|---|---|
| Frames, buffers | `eme/core/frames.hpp`, `media_buffer.hpp`, `frame_pool.hpp` | ADR-001 |
| Queues, fan-out | `eme/core/distributor.hpp` | ADR-002 |
| Graph | `eme/core/node.hpp`, `graph.hpp` | ADR-003 |
| Encoder interface | `eme/processing/video_encoder.hpp`, `encoder_node.hpp` | MF backend: EME-006 |
| Synthetic source | `eme/sources/synthetic_video_source.hpp` | MF camera: EME-004 |
| Test doubles | `eme/testing/` (`eme::mock`) | mock H.264 encoder, counting sink |

Threads: one per source (camera callback in MF), one per encoder, and one per mock sink. Real transports
must not use a thread per viewer (EME-008).
