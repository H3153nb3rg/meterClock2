#include "thread_br.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if __has_include("mdns.h")
#include "mdns.h"
#define THREAD_BR_HAVE_MDNS 1
#endif

#include "openthread/border_router.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/logging.h"
#include "openthread/thread.h"

#include "thread_config.h"

static const char *TAG = "thread_br";

// OpenThread keeps its dataset/settings in this dedicated NVS partition (see partitions.csv).
// Note the stock "nvs" partition is only initialised by nvs_flash_init(), and nothing brings up
// a custom partition for us, so it is initialised explicitly in thread_br_start().
#define THREAD_STORAGE_PARTITION "ot_storage"
#define THREAD_BR_TASK_STACK_SIZE (20 * 1024)

static esp_openthread_platform_config_t s_ot_config;

static int hex_digit_to_int(char hex) {
  if (hex >= 'A' && hex <= 'F') return 10 + hex - 'A';
  if (hex >= 'a' && hex <= 'f') return 10 + hex - 'a';
  if (hex >= '0' && hex <= '9') return hex - '0';
  return -1;
}

static size_t hex_string_to_binary(const char *hex_string, uint8_t *buf, size_t buf_size) {
  int num_char = strlen(hex_string);
  if (num_char != (int)(buf_size * 2)) return 0;
  for (int i = 0; i < num_char; i += 2) {
    int digit0 = hex_digit_to_int(hex_string[i]);
    int digit1 = hex_digit_to_int(hex_string[i + 1]);
    if (digit0 < 0 || digit1 < 0) return 0;
    buf[i / 2] = (digit0 << 4) + digit1;
  }
  return buf_size;
}

static void create_or_join_network(otInstance *instance) {
  otOperationalDataset dataset;

  if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE) {
    ESP_LOGI(TAG, "Active dataset already present, resuming existing Thread network");
    return;
  }

  if (sizeof(THREAD_JOIN_DATASET_TLV) > 1) {
    const char *hex = THREAD_JOIN_DATASET_TLV;
    size_t hex_len = strlen(hex);
    otOperationalDatasetTlvs tlvs = {};
    if (hex_len % 2 != 0 || hex_len / 2 > OT_OPERATIONAL_DATASET_MAX_LENGTH) {
      ESP_LOGE(TAG, "THREAD_JOIN_DATASET_TLV has an invalid length");
      return;
    }
    if (hex_string_to_binary(hex, tlvs.mTlvs, hex_len / 2) != hex_len / 2) {
      ESP_LOGE(TAG, "THREAD_JOIN_DATASET_TLV is not valid hex");
      return;
    }
    tlvs.mLength = hex_len / 2;
    if (otDatasetSetActiveTlvs(instance, &tlvs) != OT_ERROR_NONE) {
      ESP_LOGE(TAG, "Failed to set joined active dataset");
    } else {
      ESP_LOGI(TAG, "Joined existing Thread network from dataset TLV");
    }
    return;
  }

  if (otDatasetCreateNewNetwork(instance, &dataset) != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "Failed to create new Thread dataset");
    return;
  }

  dataset.mChannel = THREAD_CHANNEL;
  dataset.mComponents.mIsChannelPresent = true;
  dataset.mPanId = THREAD_PANID;
  dataset.mComponents.mIsPanIdPresent = true;

  size_t name_len = strnlen(THREAD_NETWORK_NAME, sizeof(THREAD_NETWORK_NAME));
  memcpy(dataset.mNetworkName.m8, THREAD_NETWORK_NAME, name_len);
  dataset.mComponents.mIsNetworkNamePresent = true;

  if (hex_string_to_binary(THREAD_EXTPANID, dataset.mExtendedPanId.m8, sizeof(dataset.mExtendedPanId.m8)) != sizeof(dataset.mExtendedPanId.m8) ||
      hex_string_to_binary(THREAD_MASTERKEY, dataset.mNetworkKey.m8, sizeof(dataset.mNetworkKey.m8)) != sizeof(dataset.mNetworkKey.m8) ||
      hex_string_to_binary(THREAD_PSKC, dataset.mPskc.m8, sizeof(dataset.mPskc.m8)) != sizeof(dataset.mPskc.m8)) {
    ESP_LOGE(TAG, "Invalid THREAD_EXTPANID / THREAD_MASTERKEY / THREAD_PSKC hex value");
    return;
  }
  dataset.mComponents.mIsExtendedPanIdPresent = true;
  dataset.mComponents.mIsNetworkKeyPresent = true;
  dataset.mComponents.mIsPskcPresent = true;

  if (otDatasetSetActive(instance, &dataset) != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "Failed to set active dataset");
    return;
  }
  if (otBorderRouterRegister(instance) != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "Failed to register border router network data");
  }
  ESP_LOGI(TAG, "Formed new Thread network \"%s\" on channel %d", THREAD_NETWORK_NAME, THREAD_CHANNEL);
}

static void ot_task_worker(void *backbone_netif) {
  ESP_LOGE(TAG, "step: task start");
  esp_netif_t *ot_netif = NULL;

  ESP_LOGE(TAG, "step: set_backbone_netif");
  esp_openthread_set_backbone_netif((esp_netif_t *)backbone_netif);

  // Failures here are logged, not fatal: the clock must keep running rather than
  // reset-loop if the border router cannot be brought up.
  ESP_LOGE(TAG, "step: esp_openthread_init");
  esp_err_t err = esp_openthread_init(&s_ot_config);
  if (err == ESP_OK) {
    // Must be created after esp_openthread_init() (same order as the Arduino
    // OpenThread library and IDF's esp_openthread_start()).
    ESP_LOGE(TAG, "step: esp_netif_new");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    ot_netif = esp_netif_new(&netif_cfg);
    if (!ot_netif) {
      err = ESP_ERR_NO_MEM;
    }
  }
  if (err == ESP_OK) {
    ESP_LOGE(TAG, "step: netif_glue_init/attach");
    err = esp_netif_attach(ot_netif, esp_openthread_netif_glue_init(&s_ot_config));
  }
  if (err == ESP_OK) {
    ESP_LOGE(TAG, "step: border_router_init");
    // The border-router glue releases/re-acquires the OpenThread task-switching
    // lock around blocking work, so it must be called with the lock held.
    esp_openthread_lock_acquire(portMAX_DELAY);
    err = esp_openthread_border_router_init();
    esp_openthread_lock_release();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start OpenThread border router: %s", esp_err_to_name(err));
    if (ot_netif) {
      esp_netif_destroy(ot_netif);
    }
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGE(TAG, "step: lock_acquire");
  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance *instance = esp_openthread_get_instance();
  otLoggingSetLevel(OT_LOG_LEVEL_NOTE);
  ESP_LOGE(TAG, "step: create_or_join_network");
  create_or_join_network(instance);
  ESP_LOGE(TAG, "step: otIp6SetEnabled");
  otIp6SetEnabled(instance, true);
  ESP_LOGE(TAG, "step: otThreadSetEnabled");
  otThreadSetEnabled(instance, true);
  ESP_LOGE(TAG, "step: lock_release");
  esp_openthread_lock_release();

  ESP_LOGE(TAG, "step: launch_mainloop");
  ESP_LOGI(TAG, "OpenThread border router running");

  esp_openthread_launch_mainloop();

  esp_netif_destroy(ot_netif);
  esp_openthread_netif_glue_deinit();
  vTaskDelete(NULL);
}

void thread_br_start(void) {
  esp_err_t err;

  esp_vfs_eventfd_config_t eventfd_config = {};
  // Consumed by OpenThread: the netif, the task queue, the border router and the
  // border-router discovery delegate (the latter needs one beyond the 3 that the
  // upstream ot_br example registers).
  eventfd_config.max_fds = 5;
  err = esp_vfs_eventfd_register(&eventfd_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "eventfd register failed: %s", esp_err_to_name(err));
    return;
  }

  err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return;
  }

  // Bring up the partition OpenThread uses for its settings. Without this,
  // nvs_open_from_partition() in otPlatSettingsInit() fails with
  // ESP_ERR_NVS_NOT_INITIALIZED and OpenThread asserts (boot loop).
  err = nvs_flash_init_partition(THREAD_STORAGE_PARTITION);
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase_partition(THREAD_STORAGE_PARTITION));
    err = nvs_flash_init_partition(THREAD_STORAGE_PARTITION);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init \"%s\" NVS partition: %s", THREAD_STORAGE_PARTITION, esp_err_to_name(err));
    return;
  }

  esp_netif_init();
  esp_event_loop_create_default();

  esp_netif_t *backbone = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!backbone) {
    ESP_LOGE(TAG, "Wi-Fi station netif not found; border router needs a backbone link");
    return;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

#ifdef THREAD_BR_HAVE_MDNS
  err = mdns_init();
  if (err == ESP_OK) {
    mdns_hostname_set("esp-ot-br");
  } else if (err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mdns init failed: %s", esp_err_to_name(err));
  }
#endif

  s_ot_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
  s_ot_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
  s_ot_config.port_config.storage_partition_name = THREAD_STORAGE_PARTITION;
  s_ot_config.port_config.netif_queue_size = 10;
  s_ot_config.port_config.task_queue_size = 10;

  if (xTaskCreate(ot_task_worker, "ot_br_main", THREAD_BR_TASK_STACK_SIZE, backbone, 5, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create OpenThread task");
  }
}
