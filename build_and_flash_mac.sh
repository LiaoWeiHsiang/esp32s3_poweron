cd examples/basic_connect
#rm -rf build
sudo fuser -k /dev/cu.usbmodem1101
pkill -f idf_monitor
sudo chmod 666 /dev/cu.usbmodem1101
eim run "idf.py set-target esp32s3"
#eim run "idf.py erase-flash"
eim run "idf.py build"
eim run "idf.py -p /dev/cu.usbmodem1101 flash monitor"