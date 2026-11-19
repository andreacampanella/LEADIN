/* audio_es8388.c - I2S output to an ES8388 codec, and its LINE IN mixer
 *
 * Playback: I2S master (the codec is the slave), 16-bit, 44.1 kHz, MCLK =
 * 256 fs. The codec's DAC goes to its output mixer. The LINE IN input goes
 * through the analog bypass to the same mixer, so the DOS PC's sound is
 * mixed without the ESP32. The register sequence follows Espressif's ES8388
 * driver in ESP-ADF.
 */
#include <math.h>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "hal_drivers.h"

/* registers */
#define R_CONTROL1      0x00
#define R_CONTROL2      0x01
#define R_CHIPPOWER     0x02
#define R_ADCPOWER      0x03
#define R_DACPOWER      0x04
#define R_MASTERMODE    0x08
#define R_ADCCONTROL2   0x0A
#define R_DACCONTROL1   0x17
#define R_DACCONTROL2   0x18
#define R_DACCONTROL3   0x19
#define R_DACCONTROL4   0x1A    /* left DAC digital volume, 0.5 dB steps down */
#define R_DACCONTROL5   0x1B
#define R_DACCONTROL16  0x26    /* mixer input select */
#define R_DACCONTROL17  0x27    /* left mixer: DAC and LIN switches, LIN gain */
#define R_DACCONTROL20  0x2A    /* right mixer */
#define R_DACCONTROL21  0x2B
#define R_DACCONTROL23  0x2D
#define R_DACCONTROL24  0x2E    /* LOUT1 volume */
#define R_DACCONTROL25  0x2F    /* ROUT1 volume */
#define R_DACCONTROL26  0x30    /* LOUT2 volume */
#define R_DACCONTROL27  0x31    /* ROUT2 volume */

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t dev;
static bool present;
static const es8388_cfg_t *cfg;


static int rd(uint8_t reg)
{
    uint8_t v = 0;
    if (i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 50) != ESP_OK)
        return -1;
    return v;
}

static bool wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}

static bool try_pins(int sda, int scl)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK)
        return false;
    if (i2c_master_probe(bus, cfg->i2c_addr, 50) == ESP_OK) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = cfg->i2c_addr,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK)
            return true;
    }
    i2c_del_master_bus(bus);
    bus = NULL;
    return false;
}

static void line_level(int level);

static bool codec_init(int level)
{
    if (cfg->pa_pin >= 0) {
        gpio_reset_pin(cfg->pa_pin);
        gpio_set_direction(cfg->pa_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->pa_pin, cfg->pa_on);
    }
    int found = -1;
    for (int i = 0; i < 4 && cfg->i2c_pins[i][0] >= 0; i++)
        if (try_pins(cfg->i2c_pins[i][0], cfg->i2c_pins[i][1])) {
            found = i;
            break;
        }
    if (found < 0) {
        hal_log("Audio: no ES8388 codec found");
        return false;
    }
    hal_log("ES8388 found on I2C SDA %d, SCL %d", cfg->i2c_pins[found][0], cfg->i2c_pins[found][1]);

    bool ok = true;
    ok &= wr(R_DACCONTROL3, 0x04);      /* mute the DAC during setup */
    ok &= wr(R_CONTROL2, 0x50);
    ok &= wr(R_CHIPPOWER, 0x00);        /* power up everything */
    ok &= wr(0x35, 0xA0);               /* internal DLL off */
    ok &= wr(0x37, 0xD0);
    ok &= wr(0x39, 0xD0);
    ok &= wr(R_MASTERMODE, 0x00);       /* I2S slave */
    ok &= wr(R_DACPOWER, 0xC0);         /* DAC and outputs off for now */
    ok &= wr(R_CONTROL1, 0x12);
    ok &= wr(R_DACCONTROL1, 0x18);      /* 16-bit I2S */
    ok &= wr(R_DACCONTROL2, 0x02);      /* single speed, MCLK = 256 fs */
    ok &= wr(R_DACCONTROL16, cfg->mixer_input);     /* mixer input: the LINE IN jack */
    ok &= wr(R_DACCONTROL17, 0x90);     /* DAC to left mixer, LIN off */
    ok &= wr(R_DACCONTROL20, 0x90);
    ok &= wr(R_DACCONTROL21, 0x80);
    ok &= wr(R_DACCONTROL23, 0x00);
    ok &= wr(R_DACCONTROL4, 0x00);      /* 0 dB */
    ok &= wr(R_DACCONTROL5, 0x00);
    ok &= wr(R_ADCPOWER, 0x09);         /* inputs, ADC and its bias on, as the ESP-ADF driver does. Microphone bias off. */
    ok &= wr(R_ADCCONTROL2, cfg->mixer_input == 0x09 ? 0x50 : 0x00);   /* same input */
    ok &= wr(R_DACPOWER, 0x3C);         /* DAC and LOUT1/ROUT1/LOUT2/ROUT2 on */
    ok &= wr(R_CHIPPOWER, 0xF0);        /* reset the state machines */
    ok &= wr(R_CHIPPOWER, 0x00);
    line_level(level);
    ok &= wr(R_DACCONTROL3, 0x00);      /* unmute */
    present = ok;
    if (!ok)
        hal_log("ES8388: register setup failed");
    /* read back, so the log shows whether the writes took effect */
    char dump[3 * 0x35 + 1];
    for (int r = 0; r <= 0x34; r++)
        sprintf(dump + 3 * r, "%02x ", rd(r) & 0xFF);
    hal_log("ES8388 registers 00..34: %s", dump);
    return ok;
}

/* 0..255 linear -> attenuation in 0.5 dB steps, up to 96 dB (0xC0) */
static uint8_t att(uint8_t v)
{
    if (v == 0)
        return 0xC0;
    float db = -20.0f * log10f(v / 255.0f);
    int steps = (int)(db * 2 + 0.5f);
    return steps > 0xC0 ? 0xC0 : steps;
}

static void codec_volume(uint8_t left, uint8_t right)
{
    if (!present)
        return;
    wr(R_DACCONTROL4, att(left));
    wr(R_DACCONTROL5, att(right));
}

static void bypass(bool on, int level)
{
    if (!present)
        return;
    if (level < 0)
        level = 0;
    if (level > 7)
        level = 7;
    /* bit 7: DAC to mixer, bit 6: LIN to mixer, bits 5..3: LIN gain */
    uint8_t v = 0x80 | (on ? 0x40 : 0) | (level << 3);
    wr(R_DACCONTROL17, v);
    wr(R_DACCONTROL20, v);
}

static void line_level(int level)
{
    if (!present && !dev)
        return;
    if (level < 0)
        level = 0;
    if (level > 33)
        level = 33;
    wr(R_DACCONTROL24, level);
    wr(R_DACCONTROL25, level);
    wr(R_DACCONTROL26, level);
    wr(R_DACCONTROL27, level);
}

/* ------------------------------------------------------------------ I2S */
static i2s_chan_handle_t tx;

static int pin_ws, pin_dout;

static bool i2s_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 480;
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, NULL) != ESP_OK)
        return false;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = cfg->i2s_mclk,
            .bclk = cfg->i2s_bck,
            .ws = pin_ws,
            .dout = pin_dout,
            .din = I2S_GPIO_UNUSED,
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK)
        return false;
    return i2s_channel_enable(tx) == ESP_OK;
}

/* ------------------------------------------------------------ the parts */
static bool start(const hal_audio_opts_t *opts)
{
    /* I2S first: the codec needs MCLK */
    pin_ws = opts->i2s_ws > 0 ? opts->i2s_ws : cfg->i2s_ws;
    pin_dout = opts->i2s_dout > 0 ? opts->i2s_dout : cfg->i2s_dout;
    hal_log("I2S: MCLK %d, BCK %d, WS %d, data %d%s", cfg->i2s_mclk, cfg->i2s_bck, pin_ws, pin_dout,
            opts->i2s_ws > 0 || opts->i2s_dout > 0 ? " (from LEADIN.CFG)" : "");
    if (!i2s_start()) {
        hal_log("Audio: I2S start failed");
        return false;
    }
    return codec_init(opts->line_level);
}

static void out_write(const int16_t *pcm, size_t frames)
{
    size_t done;
    i2s_channel_write(tx, pcm, frames * 4, &done, portMAX_DELAY);
}

static const hal_audio_t audio = {
    .name = "ES8388 headphone jack",
    .start = start,
    .write = out_write,
    .volume = codec_volume,
};

const hal_audio_t *audio_es8388(const es8388_cfg_t *c)
{
    cfg = c;
    return &audio;
}

/* The web UI level 0..100 % maps to the bypass gain in 3 dB steps:
 * 100 % = 0 dB, then -3 dB for each 20 %, down to -15 dB. 0 % is off. */
static void mixer_set(int percent)
{
    if (percent <= 0) {
        bypass(false, 2);
        return;
    }
    bypass(true, 2 + (100 - percent + 10) / 20);   /* level 2 = 0 dB, 7 = -15 dB */
}

/* The codec has two stereo input pairs. The pair that the LINE IN jack uses
 * differs between boards, so the web UI can change it. */
static int mixer_input(int pair)
{
    if (pair != 1 && pair != 2)
        pair = cfg->mixer_input == 0x09 ? 2 : 1;
    if (present) {
        wr(R_DACCONTROL16, pair == 2 ? 0x09 : 0x00);
        wr(R_ADCCONTROL2, pair == 2 ? 0x50 : 0x00);
    }
    return pair;
}

static bool mixer_available(void)
{
    return present;
}

static const hal_mixer_t mixer = {
    .available = mixer_available,
    .set = mixer_set,
    .input = mixer_input,
};

const hal_mixer_t *mixer_es8388(void)
{
    return &mixer;
}
