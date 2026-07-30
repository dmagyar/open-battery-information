# ArduinoOBI

## Hardware

This simple interface can be built using an Ardunio Uno and some external resistors. 

![screenshot](../docs/images/arduino-obi.png)

---

## Prerequisites

Ensure you have the following installed on your system:

1. **VS Code (Visual Studio Code)**  
   Download from [here](https://code.visualstudio.com/).

2. **PlatformIO Extension for VS Code**  
   Install the PlatformIO extension from the Extensions Marketplace in VS Code.

3. **Git (OPTIONAL)**  

4. **Arduino UNO**  
   Ensure you have a working Arduino UNO board and a USB cable to connect it to your computer.
   Build the circuit according to the schematic.
   It also can be done using an ESP32C3 board (e.g. the SuperMini C3). See the important notes
   on using ESPs [below](#using-an-esp).

---

## Step 1: Clone the ArduinoOBI Repository

1. Open your terminal.
2. Clone the repository using the command:

   ```bash
   git clone https://github.com/mnh-jansson/open-battery-information.git
   ```

Or,

1. Download the repository as a .ZIP file.
---

## Step 2: Open the Project in VS Code

  Open VS Code.
  Go to File > Open Folder and select the ArduinoOBI project folder.
  PlatformIO will automatically detect the project. If not, ensure the folder contains a platformio.ini file.

## Step 3: Compile the Project

  Open the PlatformIO sidebar by clicking on the PlatformIO icon in the VS Code activity bar.
  Click on the "Project Tasks" dropdown for uno.
  Under "General", click Build to compile the code.
  Check the output terminal for any errors. A successful build will show a "Success" message.

## Step 4: Flash the Code to the Arduino UNO

  Connect your Arduino UNO to your computer using a USB cable.
  In the PlatformIO sidebar, go to the "Project Tasks" dropdown for uno.
  Under "General", click Upload.
  PlatformIO will detect the correct port and upload the firmware to your Arduino UNO.
  A successful upload will display an "Upload complete" message in the terminal.

---

## Using an ESP

Tested with an ESP32C3 SuperMini. When building up the circuit, make sure to **connect the pull-up
resistors to the 3.3V pin** of the ESP board, NOT the 5V. This would fry the GPIOs and destroy the module.

Also, use Pin 0 for the ENABLE pin and Pin 1 for the ONEWIRE. This is geared towards the C3 SuperMini.
If other GPIOs are more convenient, change the ESP_EN_PIN and ESP_OW_PIN in platformio.ini.

To build the software for the ESP, make sure to select the corresponding
platformio build env "esp32-c3-devkitm-1", the other steps are the same.

To support other ESP chip types you have to change the board type and esp type in platformio.ini.

A Lolin S2 Mini environment ("lolin_s2_mini") is also provided, using GPIO5 for ENABLE and
GPIO7 for ONEWIRE — again, change ESP_EN_PIN/ESP_OW_PIN in platformio.ini if you wire it up
differently. The same 3.3V pull-up warning above applies.

---

## WiFi captive portal (ESP builds)

Any ESP build (esp32-c3-devkitm-1 or lolin_s2_mini) also hosts a standalone web UI, so you can
read a battery from your phone without a PC:

1. Flash and power the board as usual.
2. On your phone, join the WiFi network **OBIWiFi** (open, no password).
3. Your phone should prompt to open the sign-in/captive portal page automatically. If it
   doesn't, open a browser and go to `http://192.168.4.1/`.
4. Use the buttons to read the battery model/info, read live pack data, run the LED test, or
   clear the error latch — the same operations as the Windows client's Makita LXT module. A
   "Raw bytes" panel at the bottom shows the hex response for whatever you last read.

"Reset Battery Message" is shown disabled in the UI on purpose — see PROTOCOL.md for why.

This is in addition to, not instead of, the USB-serial connection the Windows/Python client
uses — both work at the same time.
