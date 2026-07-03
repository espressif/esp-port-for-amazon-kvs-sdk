Browser WebRTC Compatibility
=============================

This guide explains compatibility issues when receiving video from web browsers.

H.264 Parameter Set Problem
----------------------------

**Symptom**

When receiving video from a Chrome/Firefox browser:

* Initial frames fail to decode with "failed to activate param sets"
* Logs show NAL type 1 (non-IDR slices) only
* No SPS (NAL 7) or PPS (NAL 8) in first ~100 frames
* Video appears after 2-3 seconds (or never if encoder misconfigured)

**Root Cause - Timing Issue**

Chrome's SDP **does NOT contain** ``sprop-parameter-sets``:

.. code-block:: text

   Chrome SDP:
   a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f

   (NO sprop-parameter-sets field!)

This means Chrome sends SPS/PPS **in-band** (with IDR frames), BUT:

1. Chrome **does not send IDR immediately** after connection
2. Typical delay: 2-3 seconds before first IDR
3. Chrome sends P-frames (NAL type 1) while waiting
4. IDR with SPS/PPS arrives eventually (unless encoder misconfigured)

.. code-block:: text

   Chrome Behavior (timeline):
   t=0s:    Connection established
   t=0-2s:  P, P, P, P... (NAL type 1 - cannot decode without SPS/PPS)
   t=2-3s:  SPS+PPS+IDR (first decodable frame!)
   t=3s+:   P, P, P, IDR, P, P, P, IDR... (normal operation)

   ESP32 Sender Behavior (immediate):
   t=0s:    Connection established
   t=0s:    SPS+PPS+IDR (first frame is immediately decodable!)
   t=0s+:   P, P, P, IDR, P, P, P, IDR... (all frames decodable)

**Why This Is Different**

* **ESP32**: Sends IDR+SPS/PPS as **first frame** (immediate decode)
* **Browser**: Sends P-frames first, IDR+SPS/PPS after 2-3 seconds (delayed decode)

The tinyh264 decoder **correctly drops** P-frames until it receives SPS/PPS.

Solutions
---------

### Solution 1: Wait for IDR Frame (Recommended)

**Current Implementation**: The video player automatically waits for the first IDR frame with SPS/PPS.

Expected behavior:

1. Connection establishes → P-frames arrive (dropped, cannot decode)
2. Wait 2-3 seconds → IDR+SPS/PPS arrives
3. Decoder activates → Video displays
4. Normal operation → Periodic IDR frames keep stream decodable

.. code-block:: text

   Expected logs:
   I kvs_media: [RX] Frame 1, NAL type: 1 (P-frame)
   W video_player: Waiting for IDR frame with SPS/PPS from browser...
   I kvs_media: [RX] Frame 2, NAL type: 1 (P-frame)
   ...
   I kvs_media: [RX] Frame 73, NAL type: 1 (P-frame)
   W video_player: Still waiting for IDR frame (72 frames dropped, 3 sec elapsed)
   I kvs_media: [RX] Frame 74, NAL type: 5 (IDR!)
   I video_player: First IDR frame with SPS/PPS received! (dropped 73 frames)
   I video_player: Decoder can now start processing video stream.

**If video still doesn't appear after 10+ seconds**, see troubleshooting below.

### Solution 2: Use ESP32-to-ESP32 (Immediate Decode)

Use another ESP32 as the sender instead of a browser:

* ESP32 → ESP32: **Immediate** decode (no waiting)
* ESP32 sends IDR+SPS/PPS as first frame
* Full video decode and display works from frame 1

### Solution 3: Request Keyframe from Browser

The AWS KVS WebRTC test page may have video encoder settings:

1. Open browser developer console (F12)
2. Check if keyframe interval is too long
3. Force shorter keyframe interval:

.. code-block:: javascript

   // In browser console after connection established:
   // (This may not work on the test page, but worth trying)
   const sender = pc.getSenders().find(s => s.track.kind === 'video');
   const params = sender.getParameters();
   params.encodings[0].maxFramerate = 30;
   // Note: Cannot directly set keyframe interval in WebRTC API
   await sender.setParameters(params);

### Solution 4: Use GStreamer or Native Client

Instead of the AWS KVS WebRTC JS test page, use:

1. **KVS GStreamer Viewer** (C application)

   * Sends IDR immediately
   * Located in ``samples/kvsWebRTCClientViewerGstSample.c``

2. **ESP32-based sender**

   * Build another ESP32 as sender
   * Immediate IDR, full compatibility

3. **Mobile app with native encoder**

   * Android/iOS native encoders can be configured for immediate IDR

Troubleshooting
---------------

**Problem**: No video after 10+ seconds, logs show only P-frames

.. code-block:: text

   E video_player: BROWSER ENCODER FAILURE DETECTED
   E video_player: No IDR frame after 15 seconds and 750 P-frames.

**This is a Browser Encoder Bug**

Normal WebRTC behavior is to send IDR within 2-10 seconds. If you see 10+ seconds with NO IDR frames, the browser's video encoder is misconfigured or broken.

**Possible Causes**:

1. **Browser encoder keyframe interval misconfigured**

   * AWS KVS WebRTC test page may have encoder bugs
   * Some browsers configure very long keyframe intervals (30-60s)
   * Solution: Refresh page, try different browser

2. **Browser encoder completely broken**

   * Some browser versions have known WebRTC encoder bugs
   * Encoder may not generate keyframes at all
   * Solution: Update browser to latest version

3. **Network dropping all IDR frames** (unlikely)

   * IDR frames are 10-50x larger than P-frames
   * Severe packet loss might drop all IDRs
   * Check WebRTC stats: if packet loss >50%, this may be cause
   * Solution: Improve network quality

**Solutions**:

1. **Refresh browser page** - restarts encoder
2. **Try Firefox instead of Chrome** - different encoder
3. **Check Chrome version** - update to latest stable
4. **Use ESP32 as sender** - this ALWAYS works (immediate IDR)

Testing with Browser
--------------------

Using Chrome browser with the [AWS KVS WebRTC Test Page](https://awslabs.github.io/amazon-kinesis-video-streams-webrtc-sdk-js/examples/index.html):

**Current Status**

* Audio: ✅ Works immediately (Opus codec, no parameter sets needed)
* Video: ⏳ Works after 2-3 second delay (waiting for first IDR frame)

**Expected Diagnostic Logs**

.. code-block:: text

   I kvs_media: [RX] Frame 1, pFrame->flags=0x00000000, keyframe=0
   I kvs_media: [RX] First NAL type: 1 (non-IDR slice)
   W video_player: Waiting for IDR frame with SPS/PPS from browser...
   I kvs_media: [RX] Frame 50, NAL type: 1 (P-frame)
   W video_player: Still waiting for IDR frame (50 frames dropped, 2 sec elapsed)
   I kvs_media: [RX] Frame 75, NAL type: 5 (IDR!)
   I video_player: First IDR frame with SPS/PPS received!
   I video_player: Decoder can now start processing video stream.
   I video_player: Video resolution detected: 640x480
   [Video displays on screen]

**Alternative Testing Direction**

For **immediate** video (no delay), reverse the direction:

* **ESP32 (master) → Browser (viewer)**: ✅ Immediate video (ESP32 sends IDR as first frame)
* **Browser (viewer) → ESP32 (master)**: ⏳ 2-3 second delay (wait for browser IDR)

This allows testing ESP32 camera and encoder without waiting.

Recommended Testing Setup
--------------------------

**For Full Bidirectional Video**

.. code-block:: text

   ESP32-A (Master) ←→ ESP32-B (Viewer)

   Both directions work perfectly:
   - ESP32-A sends video → ESP32-B displays ✅
   - ESP32-B sends video → ESP32-A displays ✅

**For Browser Testing**

.. code-block:: text

   ESP32 (Master) → Browser (Viewer)    ✅ Browser displays ESP32 video
   Browser (Viewer) → ESP32 (Master)    ❌ ESP32 cannot decode browser video

Use this setup to verify:
* ESP32 camera capture
* ESP32 H.264 encoder
* WebRTC connection establishment
* KVS signaling
* One-way video streaming

Summary
-------

**Browser Video Playback**: ⏳ Works with 2-3 second initial delay

* **Cause**: Browser sends P-frames first, IDR+SPS/PPS after 2-3 seconds
* **Behavior**: Video player automatically waits for first IDR frame
* **Result**: Video displays after initial delay (normal WebRTC behavior)

**For Immediate Video** (no delay):

* Use ESP32 → ESP32 or ESP32 → Browser communication
* ESP32 sends IDR+SPS/PPS as first frame

Implementation Status
---------------------

**What Works** ✅

* ESP32 → ESP32: Immediate video, no delay
* ESP32 → Browser: Immediate video, no delay
* Browser → ESP32: 2-3 second delay (waiting for IDR)
* ESP32 → GStreamer: Immediate video, no delay

**Known Limitations** ⏳

* Browser → ESP32: Initial 2-3 second delay while waiting for first IDR frame
* This is **normal WebRTC behavior**, not a bug
* Some browsers may have longer keyframe intervals (10-30 seconds)

**Not Needed** ❌

* SDP parameter set extraction (Chrome doesn't send ``sprop-parameter-sets``)
* Codec negotiation changes (H.264 packetization-mode=1 is standard)

**Known Limitation** ⚠️

* **KVS SDK does not expose API to send PLI/FIR** (Picture Loss Indication / Full Intra Request)

  * SDK can receive PLI but cannot send it
  * Cannot explicitly request keyframe from remote sender
  * Receiver must wait for sender to generate IDR naturally
  * This is a KVS C SDK architectural limitation
  * Workaround: Ensure sender generates IDR periodically (<10s interval)

See Also
--------

* :doc:`esp32p4_notes` - ESP32-P4 specific notes
* :doc:`api-reference/media_stream` - Media Stream API
* :doc:`getting_started` - Getting Started Guide

