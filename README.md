# Ness-2-WIFI - Interface

This PCBA requires the **[NESS 2 Web API](https://github.com/afkiwers/ness_web_application)** which is a Django based web application and exposes the NESS Security Panel DX8/16 to the user using a modern, easy to understand interface.

The PCB is pushed onto the Ness Serial and Reader header and connectes the NESS Security PCB to the internet using an ESP32 programmed in Arduino Studio.

## 3D Views of PCB
<p align="center">
    <img src="images/ness_2_wifi_pcb_3d_render_top.png" alt="Ness-2-WIFI PCB 3D Render (Top)" height="200px" style="margin-right: 16px;"/>
    <img src="images/ness_2_wifi_pcb_3d_render_no_esp32_top.png" alt="Ness-2-WIFI PCB 3D Render (Top, No ESP32)" height="200px"/>
</p>

## Ness-Bridge Ethernet Interface

NESS offers an inhouse solution called ***Ness-Bridge Ethernet Interface*** but that will cost around $194.00. Feel free to have a look: https://smartness.com.au/106-014

Below is an image of the original Ness-Bridge Ethernet Interface for reference:

![Ness-Bridge Ethernet Interface](images/0003012_ness_bridge.jpeg)

## NESS Security Panel DX8/16
Below an image of the D8X/D16X main board from NESS Security

![NESS Main PCB](images/ness_main_pcb.png)

The Ness-2-WIFI bridge needs to be pushed onto the READER and SERIAL port header.

![Ness-2-WIFI PCB](images/ness_2_wifi_pcb_installed.png)