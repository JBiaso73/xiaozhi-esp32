// Placa "assistente-amiga": ESP32-S3 N16R8 + INMP441 + MAX98357A + GC9A01.
// Derivada de boards/bread-compact-wifi-lcd, com:
//  - tela redonda GC9A01 sem pino de luz de fundo
//  - sensor de toque (toque = falar/parar, segurar = encerrar), lido de um jeito
//    que funciona com qualquer módulo: TTP223 momentâneo, liga/desliga (trava),
//    saída que vai a 3V3 ou que vai ao GND
//  - ferramentas extras (e-mail, agenda, encerrar conversa, atualização remota)

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "ferramentas.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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

// -----------------------------------------------------------------------------
// Sensor de toque universal. Em vez de supor como o módulo funciona, a placa
// observa o pino a cada 20 ms e aprende sozinha:
//  - nível de repouso = como o pino fica sem ninguém tocar (alto ou baixo);
//  - modo momentâneo: o nível muda enquanto o dedo está encostado;
//      toque curto = falar/parar, segurar 1,5 s = encerrar a conversa;
//  - modo liga/desliga (trava): cada toque inverte o nível e ele fica assim;
//      cada mudança = um toque. É detectado quando o nível fica "tocado" por 6 s.
// O pino usa pull-up interno: funciona com saída push-pull e com saída que só
// puxa para o GND (comum nos módulos de vidro com LED que muda de cor).
// -----------------------------------------------------------------------------
namespace toque {

int64_t AgoraMs() {
    return esp_timer_get_time() / 1000;
}

void Clique() {
    ESP_LOGI(TAG, "Toque: clique");
    Application::GetInstance().ToggleChatState();
}

void Tarefa(void*) {
    const gpio_num_t pino = TOUCH_BUTTON_GPIO;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pino;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(200));

    int estavel = gpio_get_level(pino);
    int repouso = estavel;
    int seguidas = 0;
    bool modo_trava = false;
    bool longo_feito = false;
    int64_t desde = AgoraMs();         // quando o nível estável mudou pela última vez
    int64_t clique_pendente_em = -1;   // modo trava: espera 400 ms antes de confirmar
    ESP_LOGI(TAG, "Toque: GPIO %d iniciado, nível de repouso %d", static_cast<int>(pino), repouso);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t agora = AgoraMs();
        const int nivel = gpio_get_level(pino);

        if (nivel != estavel) {
            if (++seguidas >= 3) {  // 60 ms iguais = mudança de verdade (sem ruído)
                const int64_t duracao = agora - desde;
                estavel = nivel;
                seguidas = 0;
                desde = agora;
                assistente::RegistrarToque();
                ESP_LOGI(TAG, "Toque: nível %d (modo %s)", nivel, modo_trava ? "trava" : "momentâneo");
                if (modo_trava) {
                    if (clique_pendente_em >= 0 && duracao < 400) {
                        // Duas mudanças rápidas = pulso de um módulo momentâneo.
                        clique_pendente_em = -1;
                        modo_trava = false;
                        repouso = nivel;
                        ESP_LOGW(TAG, "Toque: voltou ao modo momentâneo");
                        Clique();
                    } else {
                        clique_pendente_em = agora;
                    }
                } else if (nivel != repouso) {
                    longo_feito = false;  // dedo encostou
                } else if (!longo_feito) {
                    Clique();  // dedo saiu antes de 1,5 s
                }
            }
        } else {
            seguidas = 0;
        }

        if (modo_trava && clique_pendente_em >= 0 && agora - clique_pendente_em >= 400) {
            clique_pendente_em = -1;
            Clique();
        }

        if (!modo_trava && estavel != repouso) {
            const int64_t segurando = agora - desde;
            if (!longo_feito && segurando >= TOUCH_LONG_PRESS_MS) {
                longo_feito = true;
                ESP_LOGI(TAG, "Toque: segurou, encerrando a conversa");
                assistente::EncerrarConversaAgora();
            }
            if (segurando >= 6000) {
                modo_trava = true;
                ESP_LOGW(TAG, "Toque: módulo do tipo liga/desliga detectado");
            }
        }
    }
}

}  // namespace toque

class AssistenteBoard : public WifiBoard {
private:
    Button boot_button_;
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

        // Sensor de toque: tarefa própria que se adapta ao tipo de módulo.
        if (TOUCH_BUTTON_GPIO != GPIO_NUM_NC) {
            xTaskCreate(toque::Tarefa, "toque", 3072, nullptr, 4, nullptr);
        }
    }

public:
    AssistenteBoard()
        : boot_button_(BOOT_BUTTON_GPIO) {
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
