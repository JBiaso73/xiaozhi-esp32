// Placa "assistente-amiga": ESP32-S3 N16R8 + INMP441 + MAX98357A + GC9A01.
// Derivada de boards/bread-compact-wifi-lcd, com:
//  - tela redonda GC9A01 sem pino de luz de fundo
//  - sensor de toque TTP223 (toque = falar/parar, segurar = encerrar)
//  - ferramentas extras (e-mail, agenda, encerrar conversa, atualização remota)

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "ferramentas.h"

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "esp_lcd_gc9a01.h"

#define TAG "AssistenteBoard"

// Mesmo codec do padrão (INMP441 + MAX98357A), mas que também mede o nível
// do microfone a cada leitura: alimenta o painel e o modo vigia.
class CodecComMedidor : public NoAudioCodecSimplex {
public:
    using NoAudioCodecSimplex::NoAudioCodecSimplex;

protected:
    int Read(int16_t* dest, int samples) override {
        const int lidas = NoAudioCodecSimplex::Read(dest, samples);
        assistente::MedirSom(dest, lidas);
        return lidas;
    }
};

class AssistenteBoard : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    LcdDisplay* display_ = nullptr;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        // Sequência de inicialização padrão do driver GC9A01 (a mesma que
        // funciona na placa bread-compact-wifi-lcd).
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        // Botão BOOT da placa: ao ligar, abre a configuração de Wi-Fi;
        // depois funciona igual ao toque.
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Toque curto: começa a conversa / interrompe a fala / encerra a escuta.
        touch_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
        // Segurar: encerra tudo e volta a dormir.
        touch_button_.OnLongPress([]() {
            assistente::EncerrarConversaAgora();
        });
    }

public:
    AssistenteBoard()
        : boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO, true /* TTP223: ativo em nível alto */,
                        TOUCH_LONG_PRESS_MS) {
        assistente::Inicializar();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        assistente::RegistrarFerramentas();
        assistente::IniciarVigia();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static CodecComMedidor audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    // Tela de 7 pinos (sem BK): sem controle de brilho. Se trocar por um GC9A01
    // com pino BLK, basta definir DISPLAY_BACKLIGHT_PIN no config.h; aí a IA
    // ganha a ferramenta de brilho ("diminui a tela").
    Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, false);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(AssistenteBoard);
