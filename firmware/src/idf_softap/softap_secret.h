// =============================================================================
// softap_secret.h - SoftAP credentials for the ESP-IDF SoftAP build
// =============================================================================
// The hybrid firmware kept its credentials in a one-off src/secret.h. Copying
// that file here would have duplicated a secret into a second place in the
// repository, so this header just re-uses the project's existing gitignored
// firmware/src/config_secret.h (the same file the Arduino builds read through
// config.h) and pulls out the two macros this build needs.
//
// Only CFG_WIFI_SSID and CFG_WIFI_PASS are used: the ESP-IDF path does not
// include config.h at all, because config.h is built around the Arduino
// TARGET_DONGLE_* capability split and this component has exactly one target.
// =============================================================================
#ifndef LILY_DUCKY_SOFTAP_SECRET_H
#define LILY_DUCKY_SOFTAP_SECRET_H

#if __has_include("../config_secret.h")
#  include "../config_secret.h"
#else
#  error "Create firmware/src/config_secret.h from config_secret.h.example (see README)"
#endif

#endif  // LILY_DUCKY_SOFTAP_SECRET_H
