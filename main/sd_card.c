#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <nvs_flash.h>
#include <nvs.h>

#define EXAMPLE_MAX_CHAR_SIZE 64
#define MOUNT_POINT "/sdcard"
#define MAX_FILE_SIZE (20 * 1024 * 1024) // 20MB
#define QUEUE_SIZE 10

#define CONFIG_SD_CARD_PIN_CLK 18
#define CONFIG_SD_CARD_PIN_CMD 17
#define CONFIG_SD_CARD_PIN_D0 16
#define CONFIG_SD_CARD_PIN_D1 7
#define CONFIG_SD_CARD_PIN_D2 6
#define CONFIG_SD_CARD_PIN_D3 15

static const char *TAG = "sd_card";

static uint8_t file_counter = 0;
static sdmmc_card_t *card;
static int max_save_file_count = 30;
static int max_file_count = 10000;

static bool sd_card_init = false;
static QueueHandle_t file_queue;

typedef struct {
    uint8_t *data;
    size_t len;
} file_write_msg_t;

void calculate_file_storage_capacity(uint32_t total_space_kb) {
    // 计算可以存储的文件数量
    uint32_t max_file_size_kb = MAX_FILE_SIZE / 1024;
    uint32_t file_count = (total_space_kb * 4 / 5) / max_file_size_kb;
    max_save_file_count = file_count;
    max_file_count = file_count * 2;

    ESP_LOGI(TAG, "最大存储文件数量: %lu, 最大文件数: %u", file_count, max_file_count);
}



static esp_err_t read_file_count() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS句柄错误: %s", esp_err_to_name(err));
        return err;
    } else {
        ESP_LOGI(TAG, "NVS句柄已打开");
        err = nvs_get_u8(nvs_handle, "file_counter", &file_counter);
        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "文件计数器: %d", file_counter);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "计数器未初始化");
                file_counter = 0;
                break;
            default:
                ESP_LOGE(TAG, "读取错误: %s", esp_err_to_name(err));
                nvs_close(nvs_handle);
                return err;
        }
        nvs_close(nvs_handle);
    }
    return ESP_OK;
}

static esp_err_t write_file_count() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS句柄错误: %s", esp_err_to_name(err));
        return err;
    } else {
        ESP_LOGI(TAG, "NVS句柄已打开");
        err = nvs_set_u8(nvs_handle, "file_counter", file_counter);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入错误: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "提交错误: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }
        ESP_LOGI(TAG, "文件计数器已写入");
        nvs_close(nvs_handle);
    }
    return ESP_OK;
}

static void check_and_remove_file_count() {
    char path[EXAMPLE_MAX_CHAR_SIZE];
    struct stat st;
    int delete_file_counter;
    if (file_counter <= max_save_file_count) {
        delete_file_counter = file_counter + max_file_count - max_save_file_count;
    } else {
        delete_file_counter = file_counter - max_save_file_count - 1;
    }

    snprintf(path, sizeof(path), MOUNT_POINT"/uart_%d.log", delete_file_counter);
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "删除文件: %s", path);
        remove(path);
    }
}

void save_to_file(uint8_t *buf, size_t len) {
    if (!sd_card_init) {
        ESP_LOGE(TAG, "SD卡未初始化");
        return;
    }

    uint8_t *data_copy = (uint8_t *)malloc(len);
    if (data_copy == NULL) {
        ESP_LOGE(TAG, "内存分配失败");
        return; // 如果内存分配失败，记录错误日志并返回
    }

    memcpy(data_copy, buf, len); // 将数据复制到新分配的内存中

    file_write_msg_t msg;
    msg.data = data_copy;
    msg.len = len;

    if (xQueueSend(file_queue, &msg, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "无法将数据放入队列");
        free(data_copy); // 如果发送失败，释放分配的内存
    }
}

uint8_t get_file_counter() {
    return file_counter;
}

static void file_write_task(void *pvParameters) {
    file_write_msg_t msg;
    char path[EXAMPLE_MAX_CHAR_SIZE];
    struct stat st;
    static FILE *f = NULL;
    static size_t cache_log_file_size = 0;

    while (1) {
        if (xQueueReceive(file_queue, &msg, portMAX_DELAY)) {
            // 生成文件路径
            snprintf(path, sizeof(path), MOUNT_POINT "/uart_%d.log", file_counter);

            if (cache_log_file_size == 0) {
                if (stat(path, &st) == 0) {
                    cache_log_file_size = st.st_size;
                } else {
                    cache_log_file_size = 0;
                }
            }

            if (cache_log_file_size > MAX_FILE_SIZE) {
                file_counter++;
                if (file_counter > 100) {
                    file_counter = 0;
                }
                cache_log_file_size = 0;
                write_file_count();
                check_and_remove_file_count();
                snprintf(path, sizeof(path), MOUNT_POINT"/uart_%d.log", file_counter);
            }

            // 打开新文件
            f = fopen(path, "a");
            if (f == NULL) {
                ESP_LOGE(TAG, "无法打开文件: %s", path);
                continue;
            }

            // 写入数据
            size_t written = fwrite(msg.data, 1, msg.len, f);
            if (written != msg.len) {
                ESP_LOGE(TAG, "写入数据到文件失败: %s", path);
            } else {
                cache_log_file_size += written;
            }

            free(msg.data);
            fclose(f);
        }
    }
}

esp_err_t init_sd_card() {
    read_file_count();

    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = 8 * 1024
    };

    ESP_LOGI(TAG, "初始化SD卡");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    slot_config.width = 4;
    slot_config.clk = CONFIG_SD_CARD_PIN_CLK;
    slot_config.cmd = CONFIG_SD_CARD_PIN_CMD;
    slot_config.d0 = CONFIG_SD_CARD_PIN_D0;
    slot_config.d1 = CONFIG_SD_CARD_PIN_D1;
    slot_config.d2 = CONFIG_SD_CARD_PIN_D2;
    slot_config.d3 = CONFIG_SD_CARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "挂载文件系统");
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "挂载文件系统失败。");
        } else {
            ESP_LOGE(TAG, "初始化SD卡失败: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    ESP_LOGI(TAG, "文件系统已挂载");

    sdmmc_card_print_info(stdout, card);

    FATFS* fs;
    DWORD fre_clust, fre_sect, tot_sect;

    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) {
        ESP_LOGE(TAG, "获取空闲簇失败");
    } else {
        tot_sect = (fs->n_fatent - 2) * fs->csize;
        fre_sect = fre_clust * fs->csize;

        ESP_LOGI(TAG, "SD卡总大小: %lu KB", tot_sect / 2);
        ESP_LOGI(TAG, "SD卡空闲空间: %lu KB", fre_sect / 2);
        calculate_file_storage_capacity(fre_sect / 2);
    }

    sd_card_init = true;

    file_queue = xQueueCreate(QUEUE_SIZE, sizeof(file_write_msg_t));
    if (file_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return ESP_FAIL;
    }

    xTaskCreate(file_write_task, "file_write_task", 1024 * 3, NULL, 5, NULL);

    return ESP_OK;
}
