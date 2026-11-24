# papilio-esp-bootloader
Papilio ESP Bootloader — always-resident factory-partition firmware providing universal OTA, JTAG, and USB-serial programming for Papilio ESP32-S3 boards.

ESP-IDF log output and UDP log forwarding are paused during active flash transfers; serial transfer progress and completion/error responses remain available.

Persistent FPGA uploads erase complete 4 KB sectors, including the final
partial sector of a bitstream. USB and HTTP uploads stop on erase/write errors
instead of acknowledging failed operations. Completion acknowledgements do
not provide full-image readback verification.
