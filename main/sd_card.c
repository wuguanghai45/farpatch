/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// This example uses SDMMC peripheral to communicate with SD card.

#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "nvs_flash.h"
#include "nvs.h"

#define EXAMPLE_MAX_CHAR_SIZE 64
#define MOUNT_POINT "/sdcard"
#define MAX_FILE_SIZE 20*1024*1024 // 20M

#define CONFIG_SD_CARD_PIN_CLK 18
#define CONFIG_SD_CARD_PIN_CMD 17
#define CONFIG_SD_CARD_PIN_D0 16
#define CONFIG_SD_CARD_PIN_D1 7
#define CONFIG_SD_CARD_PIN_D2 6
#define CONFIG_SD_CARD_PIN_D3 15

static const char *TAG = "sd_card";

static u_int8_t file_counter = 0;
static sdmmc_card_t *card;
static int cache_log_file_size = 0;
static int max_save_file_count = 30;
static int max_file_count = 10000;

static bool sd_card_init = false;


void calculate_file_storage_capacity(uint32_t total_space_kb) {
    // 计算可以存储的文件数量
    uint32_t max_file_size_kb = MAX_FILE_SIZE / 1024;
    uint32_t file_count = (total_space_kb * 4 / 5) / max_file_size_kb;
    max_save_file_count = file_count;

    max_file_count = file_count * 2;

    // printf("Total SD card space: %d KB\n", total_space_kb);
    // printf("Each file size: %d KB\n", max_file_size_kb);
    ESP_LOGI(TAG, "Maximum number of files that can be stored: %lu max_file_count: %u \n", file_count, max_file_count);
}

static esp_err_t append_file(const char *path, uint8_t *data, size_t len)
{
    // ESP_LOGI(TAG, "正在打开文件 %s", path);
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "打开文件失败");
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "写入数据到文件失败");
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}
static esp_err_t read_file_count()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    } else {
        ESP_LOGI(TAG, "NVS handle opened");

        // 读取计数器值
        err = nvs_get_u8(nvs_handle, "file_counter", &file_counter);
        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "File counter = %d", file_counter);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "The counter is not initialized yet!");
                file_counter = 0; // 初始化计数器
                break;
            default :
                ESP_LOGE(TAG, "Error (%s) reading!", esp_err_to_name(err));
                return err;
        }
        nvs_close(nvs_handle);
    }
    return ESP_OK;
}

static esp_err_t write_file_count()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    } else {
        ESP_LOGI(TAG, "NVS handle opened");

        // 写入计数器值
        err = nvs_set_u8(nvs_handle, "file_counter", file_counter);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) writing!", esp_err_to_name(err));
            return err;
        }

        // 提交写入操作
        err = nvs_commit(nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) committing!", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "File counter written");
        nvs_close(nvs_handle);
    }
    return ESP_OK;
}

//检查文件数量， 如果超过30个文件，删除最早的文件
static void check_and_remove_file_count()
{
    char path[EXAMPLE_MAX_CHAR_SIZE];
    struct stat st;
    int delete_file_counter;
    if(file_counter <= max_save_file_count) {
       delete_file_counter = file_counter + max_file_count - max_save_file_count;
    } else {
       delete_file_counter = file_counter - max_save_file_count - 1;
    }

    snprintf(path, sizeof(path), MOUNT_POINT"/uart_%d.log", delete_file_counter);
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "Delete file: %s", path);
        remove(path);
    }
}

void save_to_file(uint8_t *buf, size_t len)
{
    if(!sd_card_init) {
        ESP_LOGE(TAG, "SD card not initialized");
        return;
    }

    char path[EXAMPLE_MAX_CHAR_SIZE];

    // 构造文件路径
    snprintf(path, sizeof(path), MOUNT_POINT"/uart_%d.log", file_counter);

    // 检查文件大小
    struct stat st;

    if (cache_log_file_size == 0) {
        if (stat(path, &st) == 0) {
            cache_log_file_size = st.st_size;
        } else {
            cache_log_file_size = 0;
        }
    }

    // ESP_LOGI(TAG, "File size: %ld bytes, cache_log_file_size %d bytes", st.st_size, cache_log_file_size);
    if (cache_log_file_size > MAX_FILE_SIZE) { // 文件大小超过10M
        file_counter = file_counter + 1;
        if(file_counter > 100) {
            file_counter = 0;
        }

        cache_log_file_size = 0;
        write_file_count();
        check_and_remove_file_count();
        snprintf(path, sizeof(path), MOUNT_POINT"/uart_%d.log", file_counter);
    }

    // 写入文件
    esp_err_t ret = append_file(path, buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write file: %s", path);
    } else {
        cache_log_file_size = cache_log_file_size + len;
    }
}

u_int8_t get_file_counter()
{
    return file_counter;
}

esp_err_t init_sd_card()
{
    read_file_count();

    esp_err_t ret;
    // 文件系统挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = 8 * 1024
    };

    ESP_LOGI(TAG, "Initializing SD card");

    // 使用SDMMC外设初始化SD卡并挂载FAT文件系统
    ESP_LOGI(TAG, "Using SDMMC peripheral");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // 初始化SD卡插槽
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // 设置总线宽度
    slot_config.width = 4;

    slot_config.clk = CONFIG_SD_CARD_PIN_CLK;
    slot_config.cmd = CONFIG_SD_CARD_PIN_CMD;
    slot_config.d0 = CONFIG_SD_CARD_PIN_D0;
    slot_config.d1 = CONFIG_SD_CARD_PIN_D1;
    slot_config.d2 = CONFIG_SD_CARD_PIN_D2;
    slot_config.d3 = CONFIG_SD_CARD_PIN_D3;

    // 启用内部上拉电阻
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    
    // 打印SD卡信息
    sdmmc_card_print_info(stdout, card);
    // 获取文件系统信息
    FATFS* fs;
    DWORD fre_clust, fre_sect, tot_sect;

    // 挂载文件系统
    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) {
        ESP_LOGE(TAG, "Failed to get free clusters.");
    } else {

        // 计算总扇区和可用扇区
        tot_sect = (fs->n_fatent - 2) * fs->csize;
        fre_sect = fre_clust * fs->csize;

        // 打印SD卡容量信息
        ESP_LOGI(TAG, "SD card total size: %lu KB", tot_sect / 2);
        ESP_LOGI(TAG, "SD card free space: %lu KB", fre_sect / 2);
        calculate_file_storage_capacity(fre_sect / 2);
    }

    sd_card_init = true;

    return ESP_OK;
}