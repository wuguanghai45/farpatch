
esp_err_t init_sd_card();
void save_to_file(const uint8_t *data, int len);
u_int8_t get_file_counter();
//     // 使用 SPI 总线初始化 SD 卡