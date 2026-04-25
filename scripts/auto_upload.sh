source bin/activate
python3 meb_ota_update.py --ble --reboot --chunk-size 576 --ble-write-chunk 253 --no-ble-write-with-response ../build/meb-preheat.bin
