#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// =============================================================================
//  Assistente de voz — ESP32-S3 N16R8 + INMP441 + MAX98357A + GC9A01 redondo
//  Baseado na placa "bread-compact-wifi-lcd" do projeto xiaozhi-esp32.
// =============================================================================

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Microfone e alto-falante em barramentos I2S separados (modo simplex)
#define AUDIO_I2S_METHOD_SIMPLEX

// --- Microfone digital INMP441 --------------------------------------------
//  INMP441  ->  ESP32-S3
//  WS       ->  GPIO 4
//  SCK      ->  GPIO 5
//  SD       ->  GPIO 6
//  L/R      ->  GND   (canal esquerdo)
//  VDD      ->  3V3   (NUNCA 5V)
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// --- Amplificador MAX98357A ------------------------------------------------
//  MAX98357A ->  ESP32-S3
//  DIN       ->  GPIO 7
//  BCLK      ->  GPIO 15
//  LRC       ->  GPIO 16
//  VIN       ->  5V   (mais volume e menos chiado que 3V3)
//  GAIN / SD ->  sem ligação
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

// --- Botões e LED ----------------------------------------------------------
#define BUILTIN_LED_GPIO        GPIO_NUM_48   // LED RGB da placa (em algumas é o 38)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0    // botão BOOT da placa
// Sensor de toque TTP223 (saída ALTA quando tocado) escondido no topo do case.
//  Toque curto  = falar / interromper
//  Segurar 2 s  = encerrar a conversa (ela "dorme")
#define TOUCH_BUTTON_GPIO       GPIO_NUM_14
#define TOUCH_LONG_PRESS_MS     1500

// --- Tela redonda GC9A01 1,28" 240x240 (7 pinos, sem BLK) -------------------
//  GC9A01  ->  ESP32-S3
//  SDA     ->  GPIO 47   (MOSI)
//  SCL     ->  GPIO 21   (CLK)
//  DC      ->  GPIO 40
//  CS      ->  GPIO 41
//  RST     ->  GPIO 42
//  VCC     ->  3V3
#define DISPLAY_MOSI_PIN      GPIO_NUM_47
#define DISPLAY_CLK_PIN       GPIO_NUM_21
#define DISPLAY_DC_PIN        GPIO_NUM_40
#define DISPLAY_CS_PIN        GPIO_NUM_41
#define DISPLAY_RST_PIN       GPIO_NUM_42
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC     // sem pino BK: luz sempre acesa (com BLK: ex. GPIO_NUM_1)

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
// Se a imagem sair espelhada, troque MIRROR_X para false.
// Se as cores saírem trocadas (azul no lugar de vermelho), troque para RGB.
#define DISPLAY_MIRROR_X        true
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0
#define DISPLAY_SPI_MODE        0
#define DISPLAY_SPI_CLOCK_HZ    (40 * 1000 * 1000)

// --- Segredos e endereços (gerados pelo assistente/instalar_placa.py) --------
#if __has_include("gerado.h")
#include "gerado.h"
#else
// Sem gerado.h o firmware compila, mas sem Wi-Fi pré-configurado, sem agenda/e-mail
// e sem atualização remota. Rode: python assistente/instalar_placa.py --gerar
#define ASSISTENTE_VERSAO  "0"
#define WIFI_1_SSID        ""
#define WIFI_1_SENHA       ""
#define WIFI_2_SSID        ""
#define WIFI_2_SENHA       ""
#define APPS_SCRIPT_URL    ""
#define APPS_SCRIPT_TOKEN  ""
#define OTA_MANIFESTO_URL  ""
#define LEMBRETE_FALADO    1
#define INTERVALO_VERIFICACAO_MIN 2
#endif

#endif // _BOARD_CONFIG_H_
