
# write back the exact image
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" `
  "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" `
  --chip esp32 --port COM11 --baud 921600 write_flash --flash_size detect 0x000000 nozzle_nat_4MB.bin
