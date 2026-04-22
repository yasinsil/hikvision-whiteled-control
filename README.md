# KameraIOT -- ColorVu Smart Light Control (ESP32)

## Overview

This project allows you to fully control the white light behavior of
your Hikvision ColorVu cameras using an ESP32-S3 Super Mini.

It solves a key limitation of the built-in Smart Light feature and gives
you:

-   Continuous full-color recording at night\
-   Instant light response on motion\
-   Full control over brightness, timing, and behavior\
-   Remote configuration via MQTT

## Why not use Smart Light?

The built-in Smart Light feature does not behave as most people expect:

-   At night, the camera switches to black & white (IR mode)\
-   It only returns to color after motion is detected and the white
    light turns on

This means: - No continuous color recording\
- The beginning of events is always black & white\
- Light behavior cannot be controlled precisely

During the day: - The camera never turns on the white light, no matter
what

## Solution

By locking the camera to Night mode and setting:

-   Supplement Light: White Light (Manual)\
-   Brightness: 0%

The camera stays in full-color mode 24/7, even at night, with no visible
light output.

## How it works

1.  ESP32 subscribes to the camera's alertStream\
2.  When motion (VMD) is detected, the ESP32 receives the event in real
    time\
3.  It sends a PUT request to the supplementLight endpoint\
4.  Only whiteLightBrightness is changed\
5.  After the sequence, brightness is set back to 0

## Features

-   Flash + steady mode\
-   Sequential mode\
-   Day/night brightness control\
-   Multiple camera groups\
-   MQTT remote control\
-   Web-based configuration panel

## Requirements

-   ESP32-S3 Super Mini\
-   Hikvision ColorVu camera\
-   WiFi network\
-   MQTT broker (HiveMQ Cloud recommended)

## Setup

### Camera Settings

For these ISAPI commands to work, your camera settings should be
configured as follows:

-   Night Mode: Forced Night\
-   Supplement Light Mode: White Light (Manual)\
-   White Light Brightness: 0

### ESP32 Configuration

You'll need to create an MQTT account (free) and update the relevant
parts in the ESP32 code.

## Source Code

-   KameraIOT.ino → Main firmware\
-   index.html → Web panel

## Notes

All code was written with Claude.

You can upload the full code to Claude and ask it to customize or modify
it.

## Cost

ESP32-S3 Super Mini: under \$5
