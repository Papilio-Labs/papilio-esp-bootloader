# JTAG Programming

The bootloader owns FPGA programming operations for the Papilio Retrocade.
The loader can send a bitstream over USB serial or OTA; the bootloader stages
the Gowin SPI bridge and writes the external FPGA flash.

Use the USB path for recovery and for the one-time Phase-6 migration. Use OTA
for routine application and FPGA updates after the factory loader is present.

The FPGA companion is intentionally stripped of its former flashing and
recovery code. It is therefore dependent on this bootloader for programming
and recovery services.