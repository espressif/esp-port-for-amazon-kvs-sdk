ESP32-P4 Technical Notes
========================

This document contains important technical notes specific to ESP32-P4 implementation.

Video Player with H.264 Decoder
--------------------------------

The ESP32-P4 video player implementation includes a full H.264 software decoder with LVGL display integration.

**Features**

* **H.264 Software Decoder**: Uses ``esp_h264_dec_sw`` component (tinyh264)
* **Color Space Conversion**: YUV420 → RGB565 for display
* **LVGL Display**: Live video playback on MIPI DSI display
* **Statistics Overlay**: Real-time FPS, bitrate, resolution, and error tracking
* **Automatic Resolution Detection**: Detects video dimensions from stream

**Implementation Details**

The video player decodes received H.264 frames in real-time and displays them on the screen:

.. code-block:: c

   // Video player automatically:
   // 1. Receives H.264 NAL units from WebRTC peer
   // 2. Decodes to YUV420 planar format using software decoder
   // 3. Converts YUV420 to RGB565 using BT.601 coefficients
   // 4. Displays on LVGL canvas widget
   // 5. Shows statistics overlay (FPS, bitrate, errors)

**Performance Characteristics**

* **CPU Usage**: ~30-50% for 720p@30fps (software decoder)
* **Memory**: ~3-4MB SPIRAM for 720p (YUV + RGB buffers)
* **Latency**: ~100-200ms (decode + display)
* **Supported Resolutions**: Up to 1920x1080 (recommended 1280x720)
* **Frame Rate**: 15-30 fps

**Memory Layout**

All video player buffers are allocated in SPIRAM:

* Decoder internal state: ~200KB
* YUV420 buffer: width × height × 1.5 bytes
* RGB565 buffer: width × height × 2 bytes

For 720p (1280×720):
* YUV buffer: ~1.3MB
* RGB buffer: ~1.8MB
* **Total**: ~3.3MB in SPIRAM

**Display Configuration**

The video player integrates with LVGL for display:

* **Refresh Rate**: 15Hz (configurable)
* **Widget**: ``lv_canvas`` for video frame
* **Format**: RGB565 (16-bit true color)
* **Statistics**: ``lv_label`` overlay in top-left corner

**Color Space Conversion Optimization**

The YUV420 to RGB565 conversion is optimized to prevent watchdog timeouts and maintain system responsiveness:

* **Block Processing**: Converts 32 rows at a time, yielding 1ms between blocks
* **Pixel Pairing**: Processes 2 pixels simultaneously (sharing U/V chrominance)
* **Watchdog Safe**: Yields to other tasks regularly to prevent IDLE task watchdog
* **BT.601 Coefficients**: Standard color space conversion
* **Performance**: 50-100ms for 640x480, 200-400ms for 1920x1080
* **CPU Impact**: ~10-20% additional CPU during conversion

This ensures smooth operation even with high-resolution video streams.

Display/Camera Hardware Conflict
---------------------------------

**Critical Initialization Order**

The ESP32-P4 requires a specific initialization order for camera and display due to shared hardware resources. The initialization happens in ``kvs_media_start_global_transmission()`` when the first WebRTC session is created:

.. code-block:: c

   // CORRECT order - Camera MUST initialize first
   kvs_media_start_global_transmission() {
       1. I2C bus initialization (if needed)
       2. Camera hardware init (MIPI CSI) - FIRST
       3. Camera streaming start (encoder task starts)
       4. Display init (MIPI DSI) - AFTER camera hardware and streaming
       5. Video thread starts (just gets frames, everything already initialized)
   }

**Root Cause**

MIPI CSI (camera) and MIPI DSI (display) share the same PLL clock source (``PLL_F20M``) for their PHY layers. The initialization order matters because:

1. **Camera initialization** configures ``PLL_F20M`` for the CSI PHY clock
2. **Display initialization** reconfigures clocks, which can affect the CSI PHY
3. If display initializes first, the camera sensor may not receive the correct clock and fail to stream
4. Camera streaming must also start before display init to ensure frames are being produced

**Symptoms of Wrong Order**

If display initializes before camera:

* Camera initialization succeeds (I2C communication works)
* ``VIDIOC_STREAMON`` ioctl succeeds
* ``VIDIOC_DQBUF`` hangs indefinitely (no frames from sensor)
* Display shows: ``E lcd.dsi: can't fetch data from external memory fast enough, underrun happens``
* Video thread reports: ``No frame available from queue``

**Solution**

The initialization order is enforced in ``kvs_media.c::kvs_media_start_global_transmission()``:

.. code-block:: c

   STATUS kvs_media_start_global_transmission(void* client_data, kvs_media_config_t* config)
   {
       // 1. Initialize camera hardware FIRST (synchronously)
       video_capture->init(&video_config, &g_global_media.video_handle);

       // 2. Start camera streaming (encoder task starts producing frames)
       video_capture->start(g_global_media.video_handle);

       // 3. Initialize display AFTER camera hardware and streaming start
       if (config->video_player != NULL) {
           media_display_init();
       }

       // 4. Start video thread (hardware already initialized, thread just gets frames)
       THREAD_CREATE_EX_PRI(&g_global_media.video_sender_tid, ...);
   }

**Important Notes**

* Camera hardware initialization happens **synchronously** before starting the video thread
* Camera streaming starts **synchronously** before display initialization
* Display initialization happens **only if** video player is configured (for receiving video)
* The video thread does NOT initialize hardware - it only gets frames from the already-running encoder

**Hardware Details**

* **MIPI CSI PHY Clock**: Derived from ``PLL_F20M``
* **MIPI DSI DPI Clock**: Derived from ``PLL_F240M``
* **OV5647 Sensor**: Requires stable IDI clock from CSI PHY
* **ILI9881C Display**: Uses DSI DPI clock for pixel output

This initialization order is enforced in the ``media_stream`` component and should not be changed.

LVGL Memory Configuration
--------------------------

**Custom Memory Allocator**

The display uses a custom LVGL memory allocator to save internal RAM:

.. code-block:: c

   // Configure in sdkconfig.defaults.esp32p4:
   CONFIG_LV_USE_BUILTIN_MALLOC=n
   CONFIG_LV_USE_CUSTOM_MALLOC=y

   // Allocates LVGL memory pool from SPIRAM
   void *lv_malloc_core(size_t size) {
       return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
   }

**Memory Savings**

* Default: ~64KB internal RAM for LVGL memory pool
* Custom: 0KB internal RAM (all in SPIRAM)

**Display Buffer Configuration**

.. code-block:: c

   CONFIG_BSP_DISPLAY_LVGL_BUF_PSRAM=y      // Buffers in SPIRAM
   CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=10    // 10 lines per buffer

   // Results in:
   // Buffer size = BSP_LCD_H_RES × 10 × 2 bytes (RGB565)
   // For 1280x800: ~25KB per buffer

**Task Configuration**

To save internal RAM, LVGL task stack is also in SPIRAM:

.. code-block:: c

   lvgl_port_cfg_t lvgl_cfg = {
       .task_priority = 2,
       .task_stack = 4096,                      // 4KB stack
       .task_stack_caps = MALLOC_CAP_SPIRAM,   // Stack in SPIRAM
       .task_max_sleep_ms = 67,                // ~15Hz refresh
       .timer_period_ms = 67,
   };

Video Encoder (Outgoing Stream)
--------------------------------

The ESP32-P4 uses hardware H.264 encoder for outgoing video:

**Encoder Pipeline**

.. code-block:: text

   OV5647 Camera → MIPI CSI → ISP → YUV420 → H.264 HW Encoder → WebRTC

**Configuration**

* **Resolution**: 1920x1080@30fps (configurable)
* **Codec**: H.264 (hardware acceleration)
* **Bitrate**: Adaptive (500-2000 kbps)
* **GOP**: 30 frames (1 second at 30fps)

**Hardware Acceleration**

* Uses ESP32-P4 H.264 encoder IP
* Encodes YUV420 to H.264 in hardware
* Significantly lower CPU usage than software encoding
* Output queue managed by ``esp32p4_frame_grabber.c``

Audio Codec Configuration
--------------------------

**Codec**: ES8311 (I2C + I2S interface)

**Configuration**

* **Sample Rate**: 16kHz (configurable)
* **Channels**: Mono (1 channel)
* **Bits**: 16-bit samples
* **Encoding**: Opus (software)
* **Bitrate**: 16 kbps

**I2S Configuration**

.. code-block:: c

   I2S_STD_CONFIG_DEFAULT(
       16000,                    // Sample rate
       I2S_SLOT_MODE_MONO,      // Mono
       I2S_DATA_BIT_WIDTH_16BIT // 16-bit
   )

Memory Optimization Summary
---------------------------

**Internal RAM Savings (ESP32-P4)**

All large buffers moved to SPIRAM:

* LVGL memory pool: 64KB → 0KB (moved to SPIRAM)
* LVGL task stack: 7KB → 0KB (moved to SPIRAM)
* Display buffers: 25KB → 0KB (moved to SPIRAM)
* Video encoder output: Variable → SPIRAM
* JPEG encoder output: Variable → SPIRAM
* Ring buffers: Variable → SPIRAM
* Video player YUV/RGB: 3-4MB → SPIRAM

**Total Internal RAM Saved**: ~100KB

**SPIRAM Usage**

* Display + LVGL: ~90KB
* Video player (720p): ~3.3MB
* Video encoder queue: ~500KB
* Audio buffers: ~50KB
* **Total**: ~4MB

Performance Tuning
------------------

**Display Refresh Rate**

To reduce SPIRAM bandwidth usage, display refresh is set to 15Hz:

.. code-block:: c

   .task_max_sleep_ms = 67,  // ~15Hz (1000ms / 15 = 67ms)
   .timer_period_ms = 67,

This balances smooth video playback with system performance.

**Video Encoder Priority**

Video encoder task runs at higher priority than display:

* Video encoder: Priority 4
* LVGL task: Priority 2

This ensures outgoing video stream is not interrupted by display updates.

**Frame Rate Control**

The video pipeline automatically adapts to system load:

* Adaptive bitrate control based on send performance
* Frame skipping under high CPU load
* Automatic resolution detection for received video

Troubleshooting
---------------

**H.264 Decode Errors (ACCESS UNIT BOUNDARY / failed to activate param sets)**

The H.264 decoder requires SPS/PPS (Sequence/Picture Parameter Sets) before it can decode frames:

1. **Expected behavior**: Initial frames dropped until first frame with SPS/PPS arrives
2. **Logs to expect**: ``Dropping frame N (no SPS/PPS)`` then ``Found frame with SPS/PPS!``
3. **Solution**: The video player scans NAL units for SPS/PPS instead of relying on keyframe flag
4. **Typical wait**: 1-3 seconds for first valid frame

**Why This Matters:**

Some WebRTC senders don't properly set the keyframe flag, but still send I-frames with SPS/PPS. The video player now:

* Scans frame content for H.264 NAL types 7 (SPS) and 8 (PPS)
* Accepts any frame containing SPS/PPS, regardless of flag
* More robust interoperability with different senders

**Display Shows Nothing**

1. Check initialization order (camera before display)
2. Verify LVGL custom malloc is enabled
3. Check display power and backlight
4. Review logs for ``bsp_display_start_with_config`` errors
5. Wait for first keyframe to arrive (can take 1-2 seconds)

**Camera Not Streaming**

1. Ensure camera initializes before display
2. Check ``VIDIOC_STREAMON`` success in logs
3. Verify sensor I2C communication
4. Check CSI lane configuration

**Video Player No Output**

1. Verify video player is enabled in WebRTC config
2. Check H.264 decoder initialization
3. Review logs for decode errors
4. Ensure sufficient SPIRAM available

**High CPU Usage**

1. Software H.264 decoder uses ~30-50% CPU
2. YUV to RGB conversion adds ~10-20% CPU
3. Consider reducing video resolution (1080p → 720p → 480p)
4. Reduce frame rate (30fps → 15fps)
5. Check for memory allocation failures

**Task Watchdog Triggered**

If you see "Task watchdog got triggered" with ``yuv420_to_rgb565`` in backtrace:

1. This is normal for high-resolution video (1080p)
2. The conversion yields every 32 rows to prevent lockup
3. Consider reducing resolution to 720p or 480p
4. Increase ``CONFIG_ESP_TASK_WDT_TIMEOUT_S`` if needed (default 5s)

**Memory Allocation Failures**

1. Ensure SPIRAM is enabled
2. Check available SPIRAM with ``heap_caps_get_free_size(MALLOC_CAP_SPIRAM)``
3. Monitor peak usage during operation
4. Consider reducing video resolution

See Also
--------

* :doc:`api-reference/media_stream` - Media Stream API Guide
* :doc:`getting_started` - Getting Started Guide
* :doc:`overview` - System Overview

