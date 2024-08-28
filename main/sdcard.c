#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "sdcard.h"

#define TAG "SDCARD"

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define SD_MAX_PATH_LENGTH 128

static sdmmc_card_t *card;

void sd_init(void) {
  esp_err_t ret;

  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
  };
  ESP_LOGI(TAG, "Initializing SD card");

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };
  ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize bus.");
    return;
  }

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGE(TAG, "Failed to initialize the card (%s). ", esp_err_to_name(ret));
    }
    return;
  }
  ESP_LOGI(TAG, "Filesystem mounted");
}

void sd_delete_file(const char *filename) {
  char path_to_file[SD_MAX_PATH_LENGTH];
  snprintf(path_to_file, SD_MAX_PATH_LENGTH, "%s/%s", MOUNT_POINT, filename);
  // Check if destination file exists before deleting
  struct stat st;
  if (stat(path_to_file, &st) == 0) {
    // Delete it if it exists
    unlink(path_to_file);
    ESP_LOGI(TAG, "File deleted");
  } else {
    ESP_LOGE(TAG, "File does not exist");
  }
}

void sd_append_to_file(const char *filename, const char *buffer) {
  char path_to_file[SD_MAX_PATH_LENGTH];
  snprintf(path_to_file, SD_MAX_PATH_LENGTH, "%s/%s", MOUNT_POINT, filename);
  ESP_LOGI(TAG, "Opening file %s", path_to_file);
  FILE *f = fopen(path_to_file, "a");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return;
  }
  char line[SD_MAX_LINE_LENGTH];
  snprintf(line, SD_MAX_LINE_LENGTH, "%s\n", buffer);
  int res = fprintf(f, line, card->cid.name);
  if (res > 0) {
    ESP_LOGI(TAG, "Buffer written to file %d", res);
  } else {
    ESP_LOGE(TAG, "Failed to write to file");
  }
  fclose(f);
}

FILE *sd_open_file_for_read(const char *filename) {
  char path_to_file[SD_MAX_PATH_LENGTH];
  snprintf(path_to_file, SD_MAX_PATH_LENGTH, "%s/%s", MOUNT_POINT, filename);
  ESP_LOGI(TAG, "Opening file %s", path_to_file);
  FILE *f = fopen(path_to_file, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for reading");
    return NULL;
  }
  return f;
}

void sd_close_file(FILE *f) { fclose(f); }

esp_err_t sd_read_line_from_file(FILE *f, char *buffer, size_t size) {
  if (fgets(buffer, size, f)) {
    // strip newline
    char *pos = strchr(buffer, '\n');
    if (pos) {
      *pos = '\0';
    }
    return ESP_OK;
  }
  return ESP_FAIL;
}

void sd_clear_file(const char *filename) {
  char path_to_file[SD_MAX_PATH_LENGTH];
  snprintf(path_to_file, SD_MAX_PATH_LENGTH, "%s/%s", MOUNT_POINT, filename);
  FILE *f = fopen(path_to_file, "w");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to clear file");
    return;
  }
  fclose(f);
}

long sd_get_file_size(const char *filename) {
  char path_to_file[SD_MAX_PATH_LENGTH];
  snprintf(path_to_file, SD_MAX_PATH_LENGTH, "%s/%s", MOUNT_POINT, filename);
  FILE *f = fopen(path_to_file, "r");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file");
    return 0;
  }
  fseek(f, 0L, SEEK_END);
  long size = ftell(f);
  rewind(f);
  fclose(f);
  ESP_LOGI(TAG, "file size %d", size);

  return size;
}