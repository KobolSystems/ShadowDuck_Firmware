/* This software is licensed under the MIT License: https://github.com/spacehuhntech/usbnova */

#include "msc.h"

#include "config.h"
#include "debug.h"

#include <string>
#include <stack>

#include <SPI.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>

#include "format.h"

namespace msc {
    // ===== PRIVATE ===== //
    typedef struct file_element_t {
        std::string path;
        uint32_t    pos;
    } file_element_t;

    std::stack<file_element_t> file_stack;

#if defined(ARDUINO_ARCH_RP2040)
    // RP2040 use same flash device that store code for file system. Therefore we
    // only need to specify start address and size (no need SPI or SS)
    // By default (start=0, size=0), values that match file system setting in
    // 'Tools->Flash Size' menu selection will be used.
    Adafruit_FlashTransport_RP2040 flashTransport;
#else // if defined(ARDUINO_ARCH_RP2040)
    Adafruit_FlashTransport_SPI flashTransport(EXTERNAL_FLASH_USE_CS, EXTERNAL_FLASH_USE_SPI);
#endif // if defined(ARDUINO_ARCH_RP2040)

    Adafruit_SPIFlash flash(&flashTransport);
    Adafruit_USBD_MSC usb_msc;

    FatFileSystem fatfs;
    FatFile file;

    bool fs_changed = false; // Flag which goes to true when PC write to flash
    bool in_line    = false;

    // Callback invoked when received READ10 command.
    // Copy disk's data to buffer (up to bufsize) and
    // return number of copied bytes (must be multiple of block size)
    int32_t read_cb(uint32_t lba, void* buffer, uint32_t bufsize) {
        // Note: SPIFLash Block API: readBlocks/writeBlocks/syncBlocks
        // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
        return flash.readBlocks(lba, (uint8_t*)buffer, bufsize / 512) ? bufsize : -1;
    }

    // Callback invoked when received WRITE10 command.
    // Process data in buffer to disk's storage and
    // return number of written bytes (must be multiple of block size)
    int32_t write_cb(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
        digitalWrite(LED_BUILTIN, HIGH);

        // Note: SPIFLash Block API: readBlocks/writeBlocks/syncBlocks
        // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
        return flash.writeBlocks(lba, buffer, bufsize / 512) ? bufsize : -1;
    }

    // Callback invoked when WRITE10 command is completed (status received and accepted by host).
    // used to flush any pending cache.
    void flush_cb(void) {
        // sync with flash
        flash.syncBlocks();

        // clear file system's cache to force refresh
        fatfs.cacheClear();

        fs_changed = true;

        digitalWrite(LED_BUILTIN, LOW);
    }

    // ===== PUBLIC ===== //
    bool init() {
        if (!flash.begin()) {
            debugln("Couldn't find flash chip!");
            return false;
        }

        // Try formatting the drive if initialization failed
        if (!fatfs.begin(&flash)) {
            format();

            if (!fatfs.begin(&flash)) {
                debugln("Couldn't mount flash!");
                return false;
            }
        }

        return true;
    }

    bool format(const char* drive_name) {
        return format::start(drive_name);
    }

    void print() {
        debuglnF("Available files:");

        // Close file(s)
        if (file.isOpen()) file.close();

        while (!file_stack.empty()) file_stack.pop();

        SdFile root;
        root.open("/");

        while (file.openNext(&root, O_RDONLY)) {
            file.printFileSize(&Serial);
            debugF(" ");
            file.printName(&Serial);
            if (file.isDir()) {
                // Indicate a directory.
                debugF("/");
            }
            debugln();
            file.close();
        }
    }

    void enableDrive() {
        usb_msc.setReadWriteCallback(read_cb, write_cb, flush_cb);
        usb_msc.setCapacity(flash.size() / 512, 512);
        usb_msc.setUnitReady(true);
        usb_msc.begin();
    }

    bool changed() {
        bool tmp = fs_changed;

        fs_changed = false;
        return tmp;
    }

    bool exists(const char* filename) {
        return fatfs.exists(filename);
    }

    bool open(const char* path, bool add_to_stack) {
        debug("Open new file: ");
        debugln(path);

        // Check if filepath isn't empty
        if (!path) return false;

        // If the stack isn't empty, save the current position
        if (add_to_stack && !file_stack.empty()) {
            file_stack.top().pos = file.curPosition();
        }

        // If a file is already open, close it
        if (file.isOpen()) file.close();

        // Create a new file element and push it to the stack
        if (add_to_stack) {
            file_element_t file_element;
            file_element.path = std::string(path);
            file_element.pos  = 0;
            file_stack.push(file_element);
        }

        // Open file and return whether it was successful
        return file.open(path);
    }

    bool openNextFile() {
        debug("Opening next file: ");

        // Close current file and remove it from stack (it's not needed anymore)
        close();

        // If stack is now empty, we're done
        if (file_stack.empty()) {
            debugln("Stack is empty");
            return false;
        }

        debugln(file_stack.top().path.c_str());

        // Get the next file from the stack
        file_element_t file_element = file_stack.top();

        // Open the file
        if (file.open(file_element.path.c_str())) {
            // Seek to the saved position
            gotoPosition(file_element.pos);
            debugln("OK");
            return true;
        } else {
            debug("ERROR failed to open ");
            debugln(file_element.path.c_str());
        }

        return false;
    }

    void close() {
        // Close current file and remove it from stack (it's not needed anymore)
        debug("Stack (before file close): ");
        debugln(file_stack.size());
        debugln(file_stack.top().path.c_str());

        file.close();
        file_stack.pop();

        debug("Stack (after file close): ");
        debugln(file_stack.size());
    }

    uint32_t getPosition() {
        return file.curPosition();
    }

    void gotoPosition(uint32_t pos) {
        file.seekSet(pos);
    }

    size_t read(char* buffer, size_t len) {
        return file.read(buffer, len);
    }

    size_t readLine(char* buffer, size_t len) {
        size_t read { 0 };

        // Read as long as the file has data and buffer is not full
        // -1 to compensate for a extra linebreak at the end of the file
        while (file.isOpen() && file.available() > 0 && read < len-1) {
            // Read character by character
            char c = file.read();

            if (c == '\r') c = '\n';

            // Write into buffer
            buffer[read] = c;
            ++read;

            // If linebreak found, break loop
            if (c == '\n') {
                while (file.peek() == '\n') file.read();
                in_line = false;
                break;
            }
            // If reached end of the file, add linebreak as last character
            else if (!file.available()) {
                buffer[read] = '\n';
                in_line      = false;
                ++read;
            } else {
                in_line = true;
            }
        }

        return read;

        // return file.readBytesUntil('\n', buffer, len);
    }

    bool getInLine() {
        return in_line;
    }

    size_t write(const char* path, const char* buffer, size_t len) {
        FatFile wfile;

        wfile.open(path, (O_RDWR | O_CREAT));
        if (!wfile.isOpen()) return 0;

        size_t written = wfile.write(buffer, len);
        wfile.close();

        debug("Wrote ");
        debugln(written);

        return written;
    }
}