\page 1 - Platform Specific Code directory
will be included at the end of ~/xeforth.ino

## MCU proxy for Forth MCU extended words
+ ../uvac.h  - MCU proxy for forth MCU extended words and the following

### ESP32 specific
+ xactor - message definition
+ xserver2     - web server task (on core0)
+ xsvr_actor   - web coordinator task (on core0)
+ xforth_actor - forth vm task (on core0)
+ xgl_actor    - display and touch screen task (on core1)
