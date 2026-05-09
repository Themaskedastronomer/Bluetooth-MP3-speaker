# Bluetooth-MP3-speaker
This is a MP3/bluetooth speaker which has been built around the esp32 with a 3 section ui in a cardboard enclosure.I built it as a hobby project and as of now it has went through 3 iterations each improving over the previous one
## Hardware
ESP32 WROOM-32, 2.8 inch spi display (driver is st7789), SD card reader module, a 10000mAh powerbank, salvaged hp speaker (from S6500 impendence is 4 ohm and 3W), PAM8406 and UDA1334A (this combo was for v0.1 and v0.2 later switched to MAX98357A).
## Pinouts
for v0.1 and v0.2 

 TFT       MOSI  23 
 TFT       SCLK  18 
 TFT       CS  15 
 TFT       DC  2 
 TFT       RST  4 
 TFT       BL  21 
 SD        MOSI  23 
 SD        SCLK  18 
 SD        MISO  19 
 SD        CS    5 
 UDA1334A  BCLK  26 
 UDA1334A  WSEL  25 
 UDA1334A  DIN  22 
 BTN UP         32 
 BTN DOWN       33 
 BTN LEFT       34 
 BTN RIGHT      27 
 BTN DISP       13 
 BTN OK         14 

v0.3 (MAX98357A replacing UDA1334A + PAM8406)

same as before but

 MAX98357A  BCLK  26 
 MAX98357A  LRC   25 
 MAX98357A  DIN  22 
 MAX98357A  SD_MODE  16 
## Libraries used
Adafruit ST7735 and ST7789
Adafruit GFX
ESP32-audioI2S (by schreibfaul1)
JPEGDEC (by Larry Bank)
ESP32-A2DP (by pschatzmann)
SD (built in esp32)
## Versions
## v0.1
This was the first iteration of the speaker and was a proof of concept.In this version i tested bluetooth playback and mp3 playback.This version had a few problems such as display button function overlap(got confused between going back or turning disp off in MP3 mode) bluetooth heap fragmetion problems.Bluetooth A2DP was eating all the heap and crashing.Fixed in v2 by reducing DMA buf count to 8 and esp_bt_controller_mem_release on stop.
## v0.2
This iteration fixed the bt heap problem and brought album art in mp3 mode via JPEGDEC fixed display button overlap included fade in fade out in MP3 mode also brought bt history to the ui.Unfortunately in this version the amplifier clipped and as the amplifier had zero protection circuits against this and its left channel died.The right channel was still alive but i realised that i had to upgrade the audio chain by buying a better amplifier and also the SD card reader spring died 
## v0.3
This iteration improved on the audio chain by switching the amp+dac combo with the MAX98357A improving overall sound quality and removing idle noise (the faint buzz sound when the speaker is not playing audio)
## Known Problems
After i switched from the PAM8406 and DAC combo to the MAX98357A i cant push the volume of the speaker as high as i used to without distorsion or downright muting it i suspect its protection circuits are causing this as high volume causes it to turn off.After reboot the speaker plays again and as for the distorsion i think i would have to use more capacitors(ald using 3 1uf and 1 470uf cap for amp power) but i havent tested it yet.
