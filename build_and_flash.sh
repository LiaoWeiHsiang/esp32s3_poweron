cd examples/basic_connect
#rm -rf build
sudo fuser -k /dev/ttyACM0
pkill -f idf_monitor
sudo chmod 666 /dev/ttyACM0
eim run "idf.py set-target esp32s3"
#eim run "idf.py erase-flash"
eim run "idf.py build"
eim run "idf.py -p /dev/ttyACM0 flash monitor"