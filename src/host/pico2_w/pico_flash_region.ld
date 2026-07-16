/* Reserve the final 256 KiB of the 4 MiB flash for the YAJIR drive. */
FLASH(rx) : ORIGIN = 0x10000000, LENGTH = (3840 * 1024)
