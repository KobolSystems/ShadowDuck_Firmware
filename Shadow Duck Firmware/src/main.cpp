#include "Arduino.h"

#include "config.h"
#include "debug.h"

#include "hid/hid.h"
#include "hid/keyboard.h"
#include "led/led.h"
#include "msc/msc.h"
#include "selector/selector.h"
#include "attack/attack.h"
#include "preferences/preferences.h"
#include "duckparser/duckparser.h"
#include "tasks/tasks.h"
#include "cli/cli.h"

void update() {
    led::update();
    // cli::update();
}

void setup() {
    // Start Serial (for debug) or disable it
    debug_init();

    // Initialize memory and check for problems
    if (!msc::init()) {
        led::setColor(255, 0, 0, 200);
        return;
    }

    // Read mode from selector switch
    selector::init();

    // Load setting and set USB Device IDs
    preferences::reset();
    preferences::load();
    if (selector::mode() == SETUP) preferences::save();

    hid::setID(preferences::getVID(), preferences::getPID(), preferences::getVersion());
    hid::setSerial(preferences::getSerial());
    hid::setManufacturer(preferences::getManufacturer());
    hid::setProduct(preferences::getProduct());

    // Start Keyboard
    if ((selector::mode() == ATTACK) || preferences::hidEnabled()) {
        hid::init();
    }

    // Start USB Drive
    if (preferences::mscEnabled() || (selector::mode() == SETUP)) {
        msc::enableDrive();
    }

    // Start LED
    led::init();
    led::setEnable(preferences::ledEnabled());

    if (selector::mode() == SETUP) {
        led::setColor(preferences::getSetupColor());
    } else {
        led::setColor(preferences::getAttackColor());
    }

    // Attack settings
    keyboard::setLocale(locale::get(preferences::getDefaultLayout().c_str()));
    duckparser::setDefaultDelay(preferences::getDefaultDelay());

    // Format Flash (if specified in preferences.json)
    if ((selector::mode() == SETUP) && preferences::getFormat()) {
        led::setColor(255, 255, 255);
        msc::format(preferences::getDriveName().c_str());
    }

    // Create preferences file if it doesn't exist yet
    if (!msc::exists(PREFERENCES_PATH)) {
        preferences::save();
    }

    // Create main_script.txt if it doesn't exist yet
    if (!msc::exists(preferences::getMainScript().c_str())) {
        char message[21];
        sprintf(message, "# ShadowDuck (v%s)\n", VERSION);
        msc::write(preferences::getMainScript().c_str(), message, 20);
    }

    // Setup background tasks
    tasks::setCallback(update);

    // Make sure we don't start with a mode change
    selector::changed();

    // Start attack
    if ((selector::mode() == ATTACK) && !preferences::getRunOnIndicator()) {
        delay(preferences::getInitialDelay());      // Wait to give computer time to init keyboard
        attack::start();                            // Start keystroke injection attack
        led::setColor(preferences::getIdleColor()); // Set LED to green
    }

    // Setup CLI
#ifdef ENABLE_DEBUG
    cli::init();
#endif // ifdef ENABLE_DEBUG

    debugln("[Started]");
}

void loop() {
    tasks::update();
    cli::update();

    if (selector::read() != ATTACK) return;

    // Only start the attack if run-on-indicator is disabled, or indicator actually changed
    if (preferences::getRunOnIndicator() && hid::indicatorChanged()) {
        delay(100);
        attack::start();                            // Run script
        led::setColor(preferences::getIdleColor()); // Set LED to green
    } else if (selector::changed()) {
        // ==========  Setup Mode ==========  //
        if ((selector::mode() == SETUP) && preferences::hidEnabled()) {
            preferences::load(); // Reload the settings (in case the main script path changed)

            // Attack settings
            keyboard::setLocale(locale::get(preferences::getDefaultLayout().c_str()));
            duckparser::setDefaultDelay(preferences::getDefaultDelay());

            attack::start();                             // Start keystroke injection attack
            led::setColor(preferences::getSetupColor()); // Set LED to blue
        }
        // ==========  Attack Mode ==========  //
        else if (selector::mode() == ATTACK) {
            // Only start the attack if run-on-indicator is disabled, or indicator actually changed
            attack::start();                            // Run script
            led::setColor(preferences::getIdleColor()); // Set LED to green
        }
    }
}