| Supported Targets | ESP32-P4 | ESP32-S3 |
| ----------------- | -------- | -------- |

# Simple Video Server Example

(See the [README.md](../README.md) file in the upper level [examples](../) directory for more information about examples.)

## Overview

The example starts an HTTP server on a local network. You can use a browser to access the server.
This example provides several API endpoints to fetch video resources as follows:

| URL         | Method | Description                                                  |
| ----------- | ------ | ------------------------------------------------------------ |
| /capture    | GET    | Used for clients to get a JPEG image. Refreshing the webpage retrieves a new image, which can be saved by right-clicking the save button on the webpage. |
| /record.bin | GET    | Used for clients to download binary data of the original image. |
| /stream     | GET    | Used for clients to get continuous MJPEG stream. The server continuously pushes JPEG images to the client. |

By default, the example will start an MDNS domain name system. Therefore, the server can be accessed by domain name. For example, accessing the URL for obtaining images by entering URL `http://esp-web.local/capture` in the browser. You can also access URLs using IP addresses.

## How to use example

### Configure the project

Open the project configuration menu (`idf.py menuconfig`).

#### Hardware Configuration

This example uses the media_stream component which provides a unified interface for video capture across different ESP32 devices:

- For ESP32-P4: Supports high-resolution video using the built-in camera interface
- For ESP32-S3: Supports video capture with ESP-S3-EYE or other compatible camera modules

#### Connection Configuration:
In the `Example Connection Configuration` menu:

* If you select the Wi-Fi interface, you also have to set:
  * Wi-Fi SSID and Wi-Fi password that your ESP32 will connect to.
  * Wi-Fi SoftAP's SSID and password if you want ESP32 to work as an Access Point.

* If you select the Ethernet interface (ESP32-P4 only), you also have to set:
  * PHY model in `Ethernet PHY` option, e.g. IP101.
  * PHY address in `PHY Address` option, which should be determined by your board schematic.
  * EMAC Clock mode, GPIO used by SMI.

### Build and Flash

This example is a standalone HTTP video server — it has no KVS WebRTC dependencies, so no
`KVS_SDK_PATH` setup is needed.

```
idf.py set-target [esp32p4/esp32s3]
idf.py menuconfig          # Example Connection Configuration (Wi-Fi / Ethernet)
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.) See the [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/index.html) for ESP-IDF setup.

## Example Output

Running this example, you will see the following log output on the serial monitor:

```
I (3366) example: Video capture initialized successfully
I (3366) example: width=640 height=480
I (3396) example: Camera Web server started: http://esp-web.local:80/stream
I (4276) esp_netif_handlers: eth ip: 192.168.47.100, mask: 255.255.255.0, gw: 192.168.47.1
```

Enter `http://esp-web.local/capture` or `http://<ip-address>/capture` in the browser to access a still image.
Enter `http://esp-web.local/record.bin` or `http://<ip-address>/record.bin` in the browser to download the binary image data.
Enter `http://esp-web.local/stream` or `http://<ip-address>/stream` in the browser to access the video stream.

## Troubleshooting

1. If you encounter network connection issues:
   - Check your Wi-Fi credentials or Ethernet configuration
   - Ensure your network allows device discovery for MDNS
   - Try accessing using the IP address directly

2. If you encounter video issues:
   - Verify the camera module is properly connected
   - For ESP32-S3, ensure the correct camera pins are configured
   - Check the serial monitor for specific error messages

3. If the video stream is not working:
   - Some browsers have limitations with MJPEG streams. Try Chrome or Firefox
   - Check that your network bandwidth is sufficient for streaming
   - Reduce the resolution in app_video_init() if the stream is too slow
