/*
 * Lua Runtime for ESP32
 *
 * SPIFFS-backed script storage + Lua 5.4 VM + C bindings for hardware.
 */

#include "lua_runtime.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_spiffs.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "lua_rt";

#define SPIFFS_BASE_PATH "/spiffs"
#define LUA_TASK_STACK   8192
#define LUA_TASK_PRIO    5

static lua_State *L = NULL;
static TaskHandle_t lua_task_handle = NULL;
static volatile bool lua_task_running = false;

/* ── I2C bus state ─────────────────────────────────────────────── */

#define I2C_MAX_DEVICES  4
#define I2C_WRITE_BUF_SZ 256
#define I2C_READ_BUF_SZ  256
#define I2C_TIMEOUT_MS   100

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static uint32_t i2c_bus_freq = 400000;

static struct {
    uint16_t addr;
    i2c_master_dev_handle_t handle;
} i2c_dev_cache[I2C_MAX_DEVICES];
static int i2c_dev_count = 0;

static i2c_master_dev_handle_t i2c_get_device(uint16_t addr)
{
    for (int i = 0; i < i2c_dev_count; i++) {
        if (i2c_dev_cache[i].addr == addr) return i2c_dev_cache[i].handle;
    }
    if (!i2c_bus_handle || i2c_dev_count >= I2C_MAX_DEVICES) return NULL;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = i2c_bus_freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(i2c_bus_handle, &cfg, &dev) != ESP_OK) return NULL;

    i2c_dev_cache[i2c_dev_count].addr = addr;
    i2c_dev_cache[i2c_dev_count].handle = dev;
    i2c_dev_count++;
    return dev;
}

/* ── Default scripts (embedded) ─────────────────────────────────── */

static const char *default_di_container_lua =
    "local M = { providers = {}, bindings = {}, singletons = {} }\n"
    "\n"
    "function M.reset()\n"
    "    M.providers = {}\n"
    "    M.bindings = {}\n"
    "    M.singletons = {}\n"
    "end\n"
    "\n"
    "function M.provide(name, factory)\n"
    "    assert(type(name) == 'string' and name ~= '', 'bad provider name')\n"
    "    assert(type(factory) == 'function', 'factory must be function')\n"
    "    M.providers[name] = factory\n"
    "end\n"
    "\n"
    "function M.bind(iface, provider, opts)\n"
    "    assert(type(iface) == 'string' and iface ~= '', 'bad interface')\n"
    "    assert(type(provider) == 'string' and provider ~= '', 'bad provider')\n"
    "    M.bindings[iface] = { provider = provider, opts = opts or {} }\n"
    "    M.singletons[iface] = nil\n"
    "end\n"
    "\n"
    "function M.resolve(iface)\n"
    "    if M.singletons[iface] then\n"
    "        return M.singletons[iface]\n"
    "    end\n"
    "    local b = assert(M.bindings[iface], 'no binding: ' .. iface)\n"
    "    local f = assert(M.providers[b.provider], 'no provider: ' .. b.provider)\n"
    "    local obj = f(b.opts, M)\n"
    "    M.singletons[iface] = obj\n"
    "    return obj\n"
    "end\n"
    "\n"
    "return M\n";

static const char *default_provider_ssd1306_lua =
    "local P = {}\n"
    "\n"
    "local function cmd(addr, value)\n"
    "    i2c.write(addr, 0x00, value)\n"
    "end\n"
    "\n"
    "local function set_pos(addr, col, page)\n"
    "    cmd(addr, 0xB0 | (page & 0x07))\n"
    "    cmd(addr, col & 0x0F)\n"
    "    cmd(addr, 0x10 | ((col >> 4) & 0x0F))\n"
    "end\n"
    "\n"
    "local function send_page(addr, byte_value)\n"
    "    local data = {0x40}\n"
    "    for i = 1, 128 do\n"
    "        data[i + 1] = byte_value\n"
    "    end\n"
    "    i2c.write(addr, data)\n"
    "end\n"
    "\n"
    "function P.factory(opts, _container)\n"
    "    opts = opts or {}\n"
    "    local addr = opts.addr or 0x3C\n"
    "    local sda = opts.sda or 5\n"
    "    local scl = opts.scl or 6\n"
    "    local freq = opts.freq or 400000\n"
    "\n"
    "    local o = {}\n"
    "\n"
    "    function o:init()\n"
    "        i2c.setup(sda, scl, freq)\n"
    "        local init_seq = {\n"
    "            0xAE,0xA8,0x3F,0xD3,0x00,0x40,0xA1,0xC8,0xDA,0x12,\n"
    "            0x81,0xCF,0xA4,0xA6,0xD5,0x80,0xD9,0xF1,0xDB,0x40,\n"
    "            0x8D,0x14,0x20,0x00,0xAF\n"
    "        }\n"
    "        for _, v in ipairs(init_seq) do\n"
    "            cmd(addr, v)\n"
    "        end\n"
    "        o:clear()\n"
    "    end\n"
    "\n"
    "    function o:clear()\n"
    "        for page = 0, 7 do\n"
    "            set_pos(addr, 0, page)\n"
    "            send_page(addr, 0x00)\n"
    "        end\n"
    "    end\n"
    "\n"
    "    function o:fill(on)\n"
    "        local value = on and 0xFF or 0x00\n"
    "        for page = 0, 7 do\n"
    "            set_pos(addr, 0, page)\n"
    "            send_page(addr, value)\n"
    "        end\n"
    "    end\n"
    "\n"
    "    function o:test_pattern(step)\n"
    "        step = step or 0\n"
    "        for page = 0, 7 do\n"
    "            set_pos(addr, 0, page)\n"
    "            local data = {0x40}\n"
    "            for col = 0, 127 do\n"
    "                local val = ((col + page + step) % 2 == 0) and 0xAA or 0x55\n"
    "                data[col + 2] = val\n"
    "            end\n"
    "            i2c.write(addr, data)\n"
    "        end\n"
    "    end\n"
    "\n"
    "    return o\n"
    "end\n"
    "\n"
    "return P\n";

static const char *default_bindings_lua =
    "return {\n"
    "    display = {\n"
    "        provider = 'ssd1306',\n"
    "        opts = { addr = 0x3C, sda = 5, scl = 6, freq = 400000 }\n"
    "    }\n"
    "}\n";

static const char *default_main_lua =
    "log.info('main.lua (DI OLED case) started')\n"
    "\n"
    "local container = dofile('/spiffs/di_container.lua')\n"
    "local provider = dofile('/spiffs/provider_ssd1306.lua')\n"
    "local bindings = dofile('/spiffs/bindings.lua')\n"
    "\n"
    "container.reset()\n"
    "container.provide('ssd1306', provider.factory)\n"
    "\n"
    "for iface, b in pairs(bindings) do\n"
    "    container.bind(iface, b.provider, b.opts)\n"
    "end\n"
    "\n"
    "local display = container.resolve('display')\n"
    "display:init()\n"
    "display:clear()\n"
    "\n"
    "local step = 0\n"
    "while true do\n"
    "    display:test_pattern(step)\n"
    "    log.info(string.format('DI display tick step=%d', step))\n"
    "    step = (step + 1) % 16\n"
    "    time.sleep_ms(1000)\n"
    "end\n";

/* ── SPIFFS helpers ─────────────────────────────────────────────── */

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", (int)used, (int)total);
    return ESP_OK;
}

static bool script_exists(const char *name)
{
    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);
    struct stat st;
    return (stat(path, &st) == 0);
}

static esp_err_t write_script_if_missing(const char *name, const char *content)
{
    if (script_exists(name)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Writing default %s", name);

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create %s", name);
        return ESP_FAIL;
    }

    fputs(content, f);
    fclose(f);
    return ESP_OK;
}

static esp_err_t write_default_script(void)
{
    esp_err_t ret = write_script_if_missing("di_container.lua", default_di_container_lua);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = write_script_if_missing("provider_ssd1306.lua", default_provider_ssd1306_lua);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = write_script_if_missing("bindings.lua", default_bindings_lua);
    if (ret != ESP_OK) {
        return ret;
    }

    return write_script_if_missing("main.lua", default_main_lua);
}

/* ── Lua C bindings: gpio ───────────────────────────────────────── */

static int l_gpio_setup(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    const char *mode_str = luaL_checkstring(L, 2);

    gpio_mode_t mode = GPIO_MODE_OUTPUT;
    if (strcmp(mode_str, "input") == 0) {
        mode = GPIO_MODE_INPUT;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return luaL_error(L, "gpio.setup failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_gpio_set(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    int level = luaL_checkinteger(L, 2);
    gpio_set_level(pin, level);
    return 0;
}

static int l_gpio_get(lua_State *L)
{
    int pin = luaL_checkinteger(L, 1);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

static const luaL_Reg gpio_lib[] = {
    {"setup", l_gpio_setup},
    {"set",   l_gpio_set},
    {"get",   l_gpio_get},
    {NULL, NULL}
};

/* ── Lua C bindings: time ───────────────────────────────────────── */

static int l_time_sleep_ms(lua_State *L)
{
    int ms = luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static const luaL_Reg time_lib[] = {
    {"sleep_ms", l_time_sleep_ms},
    {NULL, NULL}
};

/* ── Lua C bindings: log ────────────────────────────────────────── */

static int l_log_info(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGI("lua", "%s", msg);
    return 0;
}

static int l_log_warn(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGW("lua", "%s", msg);
    return 0;
}

static int l_log_error(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    ESP_LOGE("lua", "%s", msg);
    return 0;
}

static const luaL_Reg log_lib[] = {
    {"info",  l_log_info},
    {"warn",  l_log_warn},
    {"error", l_log_error},
    {NULL, NULL}
};

/* ── Lua C bindings: system ─────────────────────────────────────── */

static int l_system_heap_free(lua_State *L)
{
    lua_pushinteger(L, esp_get_free_heap_size());
    return 1;
}

static int l_system_uptime(lua_State *L)
{
    lua_pushnumber(L, (double)esp_timer_get_time() / 1000000.0);
    return 1;
}

static const luaL_Reg system_lib[] = {
    {"heap_free", l_system_heap_free},
    {"uptime",    l_system_uptime},
    {NULL, NULL}
};

/* ── Lua C bindings: wifi ───────────────────────────────────────── */

static int l_wifi_rssi(lua_State *L)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lua_pushinteger(L, ap.rssi);
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static const luaL_Reg wifi_lib[] = {
    {"rssi", l_wifi_rssi},
    {NULL, NULL}
};

/* ── Lua C bindings: i2c ────────────────────────────────────────── */

static int l_i2c_setup(lua_State *L)
{
    int sda = luaL_checkinteger(L, 1);
    int scl = luaL_checkinteger(L, 2);
    int freq = luaL_optinteger(L, 3, 400000);

    /* Clean up existing bus */
    if (i2c_bus_handle) {
        for (int i = 0; i < i2c_dev_count; i++) {
            i2c_master_bus_rm_device(i2c_dev_cache[i].handle);
        }
        i2c_dev_count = 0;
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
    }

    i2c_bus_freq = freq;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus_handle);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.setup failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_write(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);
    int nargs = lua_gettop(L);

    uint8_t buf[I2C_WRITE_BUF_SZ];
    int len = 0;

    for (int i = 2; i <= nargs && len < I2C_WRITE_BUF_SZ; i++) {
        if (lua_isinteger(L, i)) {
            buf[len++] = (uint8_t)lua_tointeger(L, i);
        } else if (lua_isstring(L, i)) {
            size_t slen;
            const char *s = lua_tolstring(L, i, &slen);
            for (size_t j = 0; j < slen && len < I2C_WRITE_BUF_SZ; j++) {
                buf[len++] = (uint8_t)s[j];
            }
        } else if (lua_istable(L, i)) {
            int tlen = luaL_len(L, i);
            for (int j = 1; j <= tlen && len < I2C_WRITE_BUF_SZ; j++) {
                lua_rawgeti(L, i, j);
                buf[len++] = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
        }
    }

    if (len == 0) return 0;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    esp_err_t ret = i2c_master_transmit(dev, buf, len, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write failed: %s", esp_err_to_name(ret));
    }
    return 0;
}

static int l_i2c_read(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);
    int rlen = luaL_checkinteger(L, 2);
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    uint8_t buf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_receive(dev, buf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.read failed: %s", esp_err_to_name(ret));
    }

    lua_createtable(L, rlen, 0);
    for (int i = 0; i < rlen; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_i2c_write_read(lua_State *L)
{
    int addr = luaL_checkinteger(L, 1);

    uint8_t wbuf[I2C_WRITE_BUF_SZ];
    int wlen = 0;

    if (lua_istable(L, 2)) {
        int tlen = luaL_len(L, 2);
        for (int j = 1; j <= tlen && wlen < I2C_WRITE_BUF_SZ; j++) {
            lua_rawgeti(L, 2, j);
            wbuf[wlen++] = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
    } else if (lua_isinteger(L, 2)) {
        wbuf[wlen++] = (uint8_t)lua_tointeger(L, 2);
    }

    int rlen = luaL_checkinteger(L, 3);
    if (rlen > I2C_READ_BUF_SZ) rlen = I2C_READ_BUF_SZ;

    i2c_master_dev_handle_t dev = i2c_get_device(addr);
    if (!dev) return luaL_error(L, "i2c: cannot get device 0x%02X", addr);

    uint8_t rbuf[I2C_READ_BUF_SZ];
    esp_err_t ret = i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return luaL_error(L, "i2c.write_read failed: %s", esp_err_to_name(ret));
    }

    lua_createtable(L, rlen, 0);
    for (int i = 0; i < rlen; i++) {
        lua_pushinteger(L, rbuf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static const luaL_Reg i2c_lib[] = {
    {"setup",      l_i2c_setup},
    {"write",      l_i2c_write},
    {"read",       l_i2c_read},
    {"write_read", l_i2c_write_read},
    {NULL, NULL}
};

/* ── Register all C libraries into a Lua state ──────────────────── */

static void register_libs(lua_State *L)
{
    luaL_newlib(L, gpio_lib);   lua_setglobal(L, "gpio");
    luaL_newlib(L, time_lib);   lua_setglobal(L, "time");
    luaL_newlib(L, log_lib);    lua_setglobal(L, "log");
    luaL_newlib(L, system_lib); lua_setglobal(L, "system");
    luaL_newlib(L, wifi_lib);   lua_setglobal(L, "wifi");
    luaL_newlib(L, i2c_lib);    lua_setglobal(L, "i2c");
}

/* ── Lua VM lifecycle ───────────────────────────────────────────── */

static lua_State* create_vm(void)
{
    lua_State *state = luaL_newstate();
    if (!state) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return NULL;
    }
    luaL_openlibs(state);
    register_libs(state);
    return state;
}

static void destroy_vm(lua_State *state)
{
    if (state) {
        lua_close(state);
    }
}

/* ── Lua task (runs main.lua) ───────────────────────────────────── */

static void lua_task(void *pvParameters)
{
    lua_task_running = true;
    ESP_LOGI(TAG, "Lua task started, executing main.lua");

    int ret = luaL_dofile(L, SPIFFS_BASE_PATH "/main.lua");
    if (ret != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGE(TAG, "main.lua error: %s", err ? err : "unknown");
        lua_pop(L, 1);
    }

    ESP_LOGI(TAG, "Lua task finished (main.lua returned)");
    lua_task_running = false;
    lua_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t lua_runtime_init(void)
{
    esp_err_t ret = spiffs_init();
    if (ret != ESP_OK) return ret;

    ret = write_default_script();
    if (ret != ESP_OK) return ret;

    L = create_vm();
    if (!L) return ESP_FAIL;

    ESP_LOGI(TAG, "Lua runtime initialized");
    return ESP_OK;
}

esp_err_t lua_runtime_start(void)
{
    if (lua_task_handle) {
        ESP_LOGW(TAG, "Lua task already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(lua_task, "lua_task", LUA_TASK_STACK,
                                  NULL, LUA_TASK_PRIO, &lua_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lua task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lua_runtime_restart(void)
{
    ESP_LOGI(TAG, "Restarting Lua VM");

    /* Stop running task */
    if (lua_task_handle) {
        vTaskDelete(lua_task_handle);
        lua_task_handle = NULL;
        lua_task_running = false;
        /* Give a tick for task cleanup */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Destroy and recreate VM (task is dead, safe to access directly) */
    destroy_vm(L);
    L = create_vm();

    if (!L) {
        ESP_LOGE(TAG, "Failed to recreate Lua VM");
        return ESP_FAIL;
    }

    /* Restart task */
    return lua_runtime_start();
}

esp_err_t lua_runtime_exec(const char *code, char *result, size_t max_len)
{
    if (!L || !code || !result) return ESP_ERR_INVALID_ARG;

    /* Stop running task so we can safely access the VM */
    bool was_running = false;
    if (lua_task_handle) {
        vTaskDelete(lua_task_handle);
        lua_task_handle = NULL;
        lua_task_running = false;
        was_running = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    int ret = luaL_dostring(L, code);
    if (ret != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(result, max_len, "error: %s", err ? err : "unknown");
        lua_pop(L, 1);
        if (was_running) lua_runtime_start();
        return ESP_FAIL;
    }

    /* Capture return value from top of stack */
    if (lua_gettop(L) > 0) {
        const char *s = luaL_tolstring(L, -1, NULL);
        snprintf(result, max_len, "%s", s ? s : "nil");
        lua_pop(L, 2);  /* pop tolstring result + original value */
    } else {
        snprintf(result, max_len, "ok");
    }

    /* Resume main.lua if it was running */
    if (was_running) lua_runtime_start();
    return ESP_OK;
}

esp_err_t lua_runtime_get_script(const char *name, char *buf, size_t max_len)
{
    if (!name || !buf) return ESP_ERR_INVALID_ARG;

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, max_len, "Script not found: %s", name);
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0;
    while (total < max_len - 1) {
        size_t n = fread(buf + total, 1, max_len - 1 - total, f);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    fclose(f);
    return ESP_OK;
}

esp_err_t lua_runtime_push_script(const char *name, const char *content, bool append)
{
    if (!name || !content) return ESP_ERR_INVALID_ARG;

    char path[280];
    snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", name);

    FILE *f = fopen(path, append ? "a" : "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    fputs(content, f);
    fclose(f);
    ESP_LOGI(TAG, "Script %s: %s (%d bytes)", append ? "appended" : "written",
             name, (int)strlen(content));
    return ESP_OK;
}

esp_err_t lua_runtime_list_scripts(char *buf, size_t max_len)
{
    if (!buf) return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) {
        snprintf(buf, max_len, "Failed to open SPIFFS directory");
        return ESP_FAIL;
    }

    int offset = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && offset < (int)max_len - 1) {
        /* Get file size */
        char path[280];
        snprintf(path, sizeof(path), SPIFFS_BASE_PATH "/%s", entry->d_name);
        struct stat st;
        int size = 0;
        if (stat(path, &st) == 0) {
            size = (int)st.st_size;
        }
        offset += snprintf(buf + offset, max_len - offset,
                           "%s (%d bytes)\n", entry->d_name, size);
    }
    closedir(dir);

    if (offset == 0) {
        snprintf(buf, max_len, "(no scripts)");
    }
    return ESP_OK;
}
