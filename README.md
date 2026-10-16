# Distance Game

This repository contains the implementation of a two-player hardware-based guessing game based on the specifications originally outlined in Distance_game-report.docx. 

The system generates a random target distance, and players compete over three rounds to estimate and measure this distance as accurately as possible using a LiDAR sensor. The system communicates over Bluetooth Low Energy (BLE).

## Hardware Requirements
* 2× ESP32 development boards (ESP Learning Kit Starter)
* 1× VL53L0X LiDAR sensor
* Sensor wiring to the Secondary Node:
  * **SCL**: IO10
  * **SDA**: IO9

## System Architecture
The project is divided into two primary nodes communicating via BLE.

### 1. Main Node (`distance_game-main_node`)
The main node acts as the central controller, generating target distances for each round and directing the game flow. It also contains the integrated web server.
* **Web Interface**: Hosts an HTML webpage that displays the entire game session, text descriptions, and all control buttons. It does not use any physical buttons on the board for game control.
* **Storage**: Saves game history and results directly to the filesystem.
* **LED Indicators**:
  * **Red**: Successfully connected to the secondary node.
  * **Orange**: Game is currently running.
  * **Blinking Blue and Green**: Round in progress (waiting for players on the secondary node).

### 2. Secondary Node (`distance_game-secondary_node`)
The secondary node acts as a BLE server and handles direct communication with the VL53L0X sensor. It uses a customized STMicroelectronics VL53L0X API for I2C communication.
* **Hardware Controls**:
  * **Left Button**: Player 1 confirms their measurement.
  * **Right Button**: Player 2 confirms their measurement.
  * **Middle Button**: Aborts the game when double-pressed.
* **LED Indicators**:
  * **Red**: Inactive state or connected to the main node.
  * **Green and Blue**: Indicates which player's turn it is during a round.
  * **Orange**: Standby state between rounds.

## How to Play
1. **Power On**: Turn on both ESP32 nodes to automatically establish a BLE connection.
2. **Start Game**: Open the Main Node's webpage and click the corresponding button to start the game. The web interface will display the target distance and the live session details.
3. **Measure**:
   * Player 1 aims the sensor at a physical target corresponding to their guessed distance and presses the **Left button** on the Secondary Node.
   * Player 2 then takes their turn and presses the **Right button** on the Secondary Node.
4. **Next Round**: Advance to the next round using the control buttons on the Main Node's webpage.
5. **End / Abort**: The game concludes after three rounds. Game results are logged via the ESP log. These results are saved to the filesystem and fully displayed on the web page. A session can be prematurely aborted using the webpage controls or by double-pressing the middle button on the secondary device.
