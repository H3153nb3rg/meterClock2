#pragma once

// Thread Border Router configuration.
//
// The device forms its own Thread network by default. To JOIN an existing network
// (e.g. Home Assistant -> Thread -> "Copy dataset"), paste the active-dataset TLV
// hex string into THREAD_JOIN_DATASET_TLV below (from `ot dataset active -x`).
// A committed dataset is stored in the `ot_storage` partition and survives reboots;
// once stored, these values are ignored until you run `ot factoryreset`.

// Network used when no dataset is stored yet.
#define THREAD_NETWORK_NAME  "OpenThread-ESP"
#define THREAD_CHANNEL       15
#define THREAD_PANID         0x1234
#define THREAD_EXTPANID      "dead00beef00cafe"
#define THREAD_MASTERKEY     "00112233445566778899aabbccddeeff"
#define THREAD_PSKC          "104810e2315100afd6bc9215a6bfac53"

// Optional: active-dataset TLV (hex) to join an existing Thread network.
// Leave empty ("") to form our own network.
#define THREAD_JOIN_DATASET_TLV  ""
