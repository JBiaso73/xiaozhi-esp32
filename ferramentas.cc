// Recursos extras da assistente (e-mail, agenda, encerrar conversa,
// sinal de vida e atualização remota). Ver ferramentas.h.

#include "ferramentas.h"
#include "config.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#define TAG "Assistente"

#ifndef INTERVALO_VERIFICACAO_MIN
#define INTERVALO_VERIFICACAO_MIN 2
#endif
#ifndef LEMBRETE_FALADO
#define LEMBRETE_FALADO 1
#endif

namespace assistente {
namespace {

constexpr const char* kNvs = "assistente";
constexpr int kHorasEntreAtualizacoes = 12;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string g_url;      // URL do Google Apps Script (/exec)
std::string g_token;    // senha combinada com o Apps Script
std::string g_ota_url;  // manifest.json da atualização remota
std::atomic<bool> g_encerrando{false};

// --- Medidor do microfone e modo vigia ------------------------------------
// Os níveis são guardados em décimos de dB (dBFS), ex.: -425 = -42,5 dB.
constexpr int kSemSom = -1000;
std::atomic<int> g_mic_atual{kSemSom};   // nível do último pedacinho de 10 ms
std::atomic<int> g_mic_pico{kSemSom};    // maior nível desde o último relatório
std::atomic<int> g_mic_fundo{kSemSom};   // ruído de fundo estimado (ambiente calmo)
std::atomic<bool> g_ocioso{false};       // aparelho em espera (não está conversando)
std::atomic<bool> g_vigia_ativo{false};  // o servidor diz que é horário de vigia
std::atomic<int> g_vigia_limiar_db{18};  // quantos dB acima do fundo contam como barulho
std::atomic<uint32_t> g_silencio_ate_ms{0};  // ignora o próprio som de aviso do aparelho
std::atomic<bool> g_barulho_pendente{false};
std::atomic<int> g_barulho_pico{kSemSom};
std::atomic<int> g_barulho_duracao_ms{0};

int64_t AgoraMs() {
    return esp_timer_get_time() / 1000;
}

// -----------------------------------------------------------------------------
// HTTP com redirecionamento manual (o Apps Script sempre responde 302).
// -----------------------------------------------------------------------------
std::expected<std::string, std::string> Requisicao(std::string metodo, std::string url,
                                                   std::string corpo) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return std::unexpected("sem rede");
    }
    for (int salto = 0; salto < 4; ++salto) {
        auto http = network->CreateHttp(1);
        http->SetTimeout(20000);
        if (metodo == "POST") {
            http->SetHeader("Content-Type", "application/json");
            http->SetContent(std::string(corpo));
        }
        if (auto aberto = http->Open(metodo, url); !aberto) {
            return std::unexpected("falha ao conectar (" + aberto.error().ToString() + ")");
        }
        auto status = http->GetStatusCode();
        if (!status) {
            http->Close();
            return std::unexpected("sem resposta (" + status.error().ToString() + ")");
        }
        const int codigo = *status;
        if (codigo == 301 || codigo == 302 || codigo == 303 || codigo == 307 || codigo == 308) {
            std::string destino = http->GetResponseHeader("Location");
            http->Close();
            if (destino.empty()) {
                return std::unexpected("redirecionamento sem destino");
            }
            url = destino;
            if (codigo != 307 && codigo != 308) {
                metodo = "GET";
                corpo.clear();
            }
            continue;
        }
        std::string resposta = http->ReadAll();
        http->Close();
        if (codigo != 200) {
            return std::unexpected("HTTP " + std::to_string(codigo));
        }
        return resposta;
    }
    return std::unexpected("redirecionamentos demais");
}

// Chama uma ação do Apps Script. "dados" passa a pertencer a esta função.
std::expected<JsonPtr, std::string> ChamarScript(const char* acao, cJSON* dados = nullptr) {
    JsonPtr corpo(dados != nullptr ? dados : cJSON_CreateObject(), cJSON_Delete);
    if (g_url.empty()) {
        return std::unexpected(
            std::string("A agenda e o e-mail ainda não foram configurados pelo dono do aparelho."));
    }
    cJSON_AddStringToObject(corpo.get(), "token", g_token.c_str());
    cJSON_AddStringToObject(corpo.get(), "acao", acao);
    cJSON_AddStringToObject(corpo.get(), "versao", ASSISTENTE_VERSAO);

    char* texto = cJSON_PrintUnformatted(corpo.get());
    std::string json = texto != nullptr ? texto : "{}";
    cJSON_free(texto);

    auto resposta = Requisicao("POST", g_url, json);
    if (!resposta) {
        ESP_LOGW(TAG, "Apps Script (%s): %s", acao, resposta.error().c_str());
        return std::unexpected("Não consegui falar com o servidor da agenda: " + resposta.error());
    }
    JsonPtr raiz(cJSON_Parse(resposta->c_str()), cJSON_Delete);
    if (!raiz) {
        return std::unexpected("Resposta inválida do servidor da agenda.");
    }
    cJSON* ok = cJSON_GetObjectItem(raiz.get(), "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON* erro = cJSON_GetObjectItem(raiz.get(), "erro");
        return std::unexpected(std::string(cJSON_IsString(erro) ? erro->valuestring
                                                                 : "Erro desconhecido na agenda."));
    }
    return std::expected<JsonPtr, std::string>(std::move(raiz));
}

// Executa a ação e devolve à IA o campo "texto" da resposta.
ToolResult AcaoComTexto(const char* acao, cJSON* dados) {
    auto r = ChamarScript(acao, dados);
    if (!r) {
        return std::unexpected(r.error());
    }
    cJSON* texto = cJSON_GetObjectItem(r->get(), "texto");
    return std::string(cJSON_IsString(texto) ? texto->valuestring : "Feito.");
}

// -----------------------------------------------------------------------------
// Encerrar conversa
// -----------------------------------------------------------------------------
void TarefaEncerrarDepoisDeFalar(void*) {
    auto& app = Application::GetInstance();
    // 1) Dá um tempo para a resposta de despedida começar a tocar.
    for (int i = 0; i < 25 && app.GetDeviceState() != kDeviceStateSpeaking; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // 2) Deixa terminar a frase (no máximo 15 s).
    for (int i = 0; i < 150 && app.GetDeviceState() == kDeviceStateSpeaking; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    // 3) Fecha o canal de áudio: volta a dormir esperando o nome.
    if (app.GetDeviceState() == kDeviceStateListening) {
        app.ToggleChatState();
    }
    g_encerrando = false;
    vTaskDelete(nullptr);
}

void TarefaEncerrarAgora(void*) {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.ToggleChatState();  // interrompe a fala
        for (int i = 0; i < 30 && app.GetDeviceState() == kDeviceStateSpeaking; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (app.GetDeviceState() == kDeviceStateListening) {
        app.ToggleChatState();  // fecha o canal
    }
    g_encerrando = false;
    vTaskDelete(nullptr);
}

void Disparar(TaskFunction_t funcao, const char* nome) {
    bool esperado = false;
    if (!g_encerrando.compare_exchange_strong(esperado, true)) {
        return;  // já há um encerramento em andamento
    }
    if (xTaskCreate(funcao, nome, 3072, nullptr, 3, nullptr) != pdPASS) {
        g_encerrando = false;
    }
}

// -----------------------------------------------------------------------------
// Tarefa de fundo: lembretes, sinal de vida e atualização remota
// -----------------------------------------------------------------------------
void VerificarLembretes() {
    // Junto com o "sinal de vida" vai o nível do microfone, para o painel
    // mostrar se ele está ouvindo (diagnóstico à distância).
    cJSON* dados = cJSON_CreateObject();
    const int pico = g_mic_pico.exchange(kSemSom);
    // Números inteiros (dB): o printf deste firmware não formata casas decimais.
    cJSON_AddNumberToObject(dados, "mic_pico", pico / 10);
    cJSON_AddNumberToObject(dados, "mic_fundo", g_mic_fundo.load() / 10);
    auto r = ChamarScript("pendentes", dados);
    if (!r) {
        return;
    }
    // Configuração do modo vigia (horários e sensibilidade ficam no Apps Script)
    cJSON* vigia = cJSON_GetObjectItem(r->get(), "vigia");
    if (cJSON_IsObject(vigia)) {
        cJSON* ativo = cJSON_GetObjectItem(vigia, "ativo");
        cJSON* limiar = cJSON_GetObjectItem(vigia, "limiar");
        const bool estava = g_vigia_ativo.load();
        g_vigia_ativo = cJSON_IsTrue(ativo);
        if (cJSON_IsNumber(limiar) && limiar->valueint >= 6 && limiar->valueint <= 40) {
            g_vigia_limiar_db = limiar->valueint;
        }
        if (estava != g_vigia_ativo.load()) {
            ESP_LOGI(TAG, "Modo vigia %s (limiar %d dB)", g_vigia_ativo.load() ? "LIGADO" : "desligado",
                     g_vigia_limiar_db.load());
        }
    }
    cJSON* itens = cJSON_GetObjectItem(r->get(), "itens");
    if (!cJSON_IsArray(itens) || cJSON_GetArraySize(itens) == 0) {
        return;
    }
    std::string texto;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, itens) {
        if (cJSON_IsString(item)) {
            if (!texto.empty()) {
                texto += "; ";
            }
            texto += item->valuestring;
        }
    }
    if (texto.empty()) {
        return;
    }
    ESP_LOGI(TAG, "Lembrete: %s", texto.c_str());

    auto& app = Application::GetInstance();
    app.Schedule([texto]() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        g_silencio_ate_ms = static_cast<uint32_t>(AgoraMs() + 5000);  // o próprio aviso não conta
        app.Alert("Lembrete", texto.c_str(), "winking", Lang::Sounds::OGG_POPUP);
#if LEMBRETE_FALADO
        // Pede para a IA falar o lembrete em voz alta (uma frase curta).
        app.WakeWordInvoke("[Aviso automático do aparelho, não é a usuária falando] "
                           "Chegou a hora do lembrete: " + texto +
                           ". Avise a usuária em uma única frase curta e gentil.");
#endif
    });
}

void VerificarAtualizacao() {
    auto r = Requisicao("GET", g_ota_url, "");
    if (!r) {
        ESP_LOGW(TAG, "Manifesto OTA: %s", r.error().c_str());
        return;
    }
    JsonPtr raiz(cJSON_Parse(r->c_str()), cJSON_Delete);
    if (!raiz) {
        return;
    }
    cJSON* versao = cJSON_GetObjectItem(raiz.get(), "versao");
    cJSON* url = cJSON_GetObjectItem(raiz.get(), "url");
    if (!cJSON_IsString(versao) || !cJSON_IsString(url)) {
        return;
    }
    std::string nova = versao->valuestring;
    std::string endereco = url->valuestring;
    // Só atualiza para um número MAIOR: um firmware de instalação mais novo,
    // gravado por USB, nunca é "rebaixado" pela atualização remota.
    if (nova.empty() || std::atoi(nova.c_str()) <= std::atoi(ASSISTENTE_VERSAO)) {
        return;
    }
    {
        Settings settings(kNvs, true);
        if (settings.GetString("ota_bloq") == nova) {
            return;  // esta versão já falhou uma vez neste aparelho
        }
        settings.SetString("ota_tent", nova);
    }
    ESP_LOGI(TAG, "Atualização remota: %s -> %s", ASSISTENTE_VERSAO, nova.c_str());
    Application::GetInstance().Schedule([nova, endereco]() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            Settings(kNvs, true).EraseKey("ota_tent");
            return;
        }
        if (!app.UpgradeFirmware(endereco, nova)) {
            // Falhou o download: tenta de novo no próximo ciclo.
            Settings(kNvs, true).EraseKey("ota_tent");
        }
        // Se deu certo, o aparelho reinicia sozinho.
    });
}

void AvisarBarulho() {
    cJSON* dados = cJSON_CreateObject();
    cJSON_AddNumberToObject(dados, "pico", g_barulho_pico.load() / 10);
    cJSON_AddNumberToObject(dados, "fundo", g_mic_fundo.load() / 10);
    cJSON_AddNumberToObject(dados, "duracao_ms", g_barulho_duracao_ms.load());
    auto r = ChamarScript("barulho", dados);
    ESP_LOGI(TAG, "Barulho no modo vigia (pico %d dB): %s", g_barulho_pico.load() / 10,
             r ? "avisado" : r.error().c_str());
}

void TarefaVigia(void*) {
    vTaskDelay(pdMS_TO_TICKS(45 * 1000));  // deixa o aparelho terminar de ligar
    constexpr int kMinutos = INTERVALO_VERIFICACAO_MIN > 0 ? INTERVALO_VERIFICACAO_MIN : 2;
    constexpr uint32_t kSegundosPorCiclo = kMinutos * 60;
    constexpr uint32_t kCiclosOta = (kHorasEntreAtualizacoes * 60) / kMinutos;
    constexpr int64_t kIntervaloBarulhoMs = 5 * 60 * 1000;  // no máximo 1 aviso a cada 5 min
    uint32_t segundo = 0;
    uint32_t ciclo = 0;
    int64_t ultimo_barulho_ms = -kIntervaloBarulhoMs;
    while (true) {
        auto& app = Application::GetInstance();
        const bool ocioso = app.GetDeviceState() == kDeviceStateIdle;
        g_ocioso = ocioso;

        if (g_barulho_pendente.exchange(false) && ocioso && g_vigia_ativo.load() && !g_url.empty()) {
            const int64_t agora = AgoraMs();
            if (agora - ultimo_barulho_ms >= kIntervaloBarulhoMs) {
                ultimo_barulho_ms = agora;
                AvisarBarulho();
            }
        }

        if (segundo % 10 == 0) {
            ESP_LOGI(TAG, "Microfone: agora %d dB | fundo %d dB | vigia %s",
                     g_mic_atual.load() / 10, g_mic_fundo.load() / 10,
                     g_vigia_ativo.load() ? "ligado" : "desligado");
        }

        if (segundo % kSegundosPorCiclo == 0 && ocioso) {
            if (!g_url.empty()) {
                VerificarLembretes();  // também serve de "sinal de vida"
            }
            if (!g_ota_url.empty() && (ciclo % kCiclosOta) == 0) {
                VerificarAtualizacao();
            }
            ++ciclo;
        }
        ++segundo;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void LembrarValor(Settings& settings, const char* chave, const char* compilado, std::string& destino) {
    if (compilado != nullptr && std::strlen(compilado) > 0) {
        settings.SetString(chave, compilado);
    }
    destino = settings.GetString(chave);
}

void CadastrarWifi(const char* ssid, const char* senha) {
    if (ssid == nullptr || std::strlen(ssid) == 0) {
        return;
    }
    auto& ssids = SsidManager::GetInstance();
    for (const auto& item : ssids.GetSsidList()) {
        if (item.ssid == ssid) {
            return;
        }
    }
    ssids.AddSsid(ssid, senha);
    ESP_LOGI(TAG, "Wi-Fi pré-configurado: %s", ssid);
}

}  // namespace

// =============================================================================

void Inicializar() {
    Settings settings(kNvs, true);

    // Firmware de instalação traz os segredos; o de atualização remota vem vazio
    // e continua usando o que já está guardado na memória.
    LembrarValor(settings, "gas_url", APPS_SCRIPT_URL, g_url);
    LembrarValor(settings, "gas_token", APPS_SCRIPT_TOKEN, g_token);
    LembrarValor(settings, "ota_url", OTA_MANIFESTO_URL, g_ota_url);

    // Resultado da última atualização remota
    std::string tentativa = settings.GetString("ota_tent");
    if (!tentativa.empty()) {
        if (tentativa != ASSISTENTE_VERSAO) {
            ESP_LOGW(TAG, "Atualização %s falhou e voltou à versão anterior", tentativa.c_str());
            settings.SetString("ota_bloq", tentativa);
        }
        settings.EraseKey("ota_tent");
    }

    CadastrarWifi(WIFI_1_SSID, WIFI_1_SENHA);
    CadastrarWifi(WIFI_2_SSID, WIFI_2_SENHA);

    ESP_LOGI(TAG, "Versão %s | agenda: %s | atualização remota: %s", ASSISTENTE_VERSAO,
             g_url.empty() ? "não" : "sim", g_ota_url.empty() ? "não" : "sim");
}

void RegistrarFerramentas() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.conversa.encerrar",
        "Encerra a conversa e volta a dormir (o aparelho para de ouvir até chamarem o nome dele de novo). "
        "Chame IMEDIATAMENTE quando a usuária: disser que não é com você; disser tchau, até logo, obrigado "
        "é só isso, pode dormir, pode parar, silêncio; ou quando a fala que você ouviu claramente foi para "
        "outra pessoa da sala, for um barulho, TV ou conversa paralela. Depois de chamar, responda no "
        "máximo com 'Tá bom.' ou não diga nada.",
        PropertyList(), [](const PropertyList&) -> ToolResult {
            Disparar(TarefaEncerrarDepoisDeFalar, "encerrar_conv");
            return std::string("Conversa encerrada. Não diga mais nada além de, no máximo, 'Tá bom.'");
        });

    mcp.AddTool(
        "self.agenda.data_hora_atual",
        "Informa a data, o dia da semana e a hora atuais no horário de Brasília. Use antes de criar "
        "lembretes com datas relativas (amanhã, sexta que vem) se você não tiver certeza da data de hoje.",
        PropertyList(), [](const PropertyList&) -> ToolResult { return AcaoComTexto("agora", nullptr); });

    mcp.AddTool(
        "self.agenda.enviar_email",
        "Envia um e-mail para a usuária (o endereço já está configurado; nunca pergunte o e-mail). Use "
        "quando ela pedir para mandar, anotar, guardar ou imprimir algo: receitas, listas de compras, "
        "textos, resumos, explicações, roteiros. Escreva o conteúdo COMPLETO e bem organizado, em "
        "português, em texto simples: um item por linha, listas começando com '- ', passos numerados "
        "'1. ', títulos curtos terminando com ':'. Não leia o conteúdo em voz alta: depois de enviar, "
        "confirme em uma frase curta.",
        PropertyList({Property("assunto", kPropertyTypeString).SetMaxLength(120),
                      Property("conteudo", kPropertyTypeString).SetMaxLength(6000)}),
        [](const PropertyList& p) -> ToolResult {
            cJSON* dados = cJSON_CreateObject();
            cJSON_AddStringToObject(dados, "assunto", p["assunto"].value<std::string>().c_str());
            cJSON_AddStringToObject(dados, "conteudo", p["conteudo"].value<std::string>().c_str());
            return AcaoComTexto("email", dados);
        });

    mcp.AddTool(
        "self.agenda.criar_lembrete",
        "Cria um lembrete ou compromisso na agenda da usuária. Na hora marcada ela recebe um e-mail e o "
        "aparelho avisa com som e voz. Informe 'data_hora' no formato AAAA-MM-DD HH:MM (horário de "
        "Brasília) OU 'daqui_minutos' para pedidos relativos (ex.: 'daqui a meia hora' = 30). Se ela não "
        "disser o horário, pergunte antes de criar. Confirme numa frase curta usando o texto devolvido.",
        PropertyList({Property("descricao", kPropertyTypeString).SetMaxLength(300),
                      Property("data_hora", kPropertyTypeString, std::string("")),
                      Property("daqui_minutos", kPropertyTypeInteger, 0, 0, 525600)}),
        [](const PropertyList& p) -> ToolResult {
            cJSON* dados = cJSON_CreateObject();
            cJSON_AddStringToObject(dados, "descricao", p["descricao"].value<std::string>().c_str());
            cJSON_AddStringToObject(dados, "data_hora", p["data_hora"].value<std::string>().c_str());
            cJSON_AddNumberToObject(dados, "daqui_minutos", p["daqui_minutos"].value<int>());
            return AcaoComTexto("lembrete", dados);
        });

    mcp.AddTool(
        "self.agenda.consultar",
        "Lista os próximos lembretes e compromissos da agenda. Use quando ela perguntar o que tem hoje, "
        "amanhã ou na semana. Resuma em poucas frases; se forem muitos, ofereça mandar por e-mail.",
        PropertyList({Property("dias", kPropertyTypeInteger, 7, 1, 60)}),
        [](const PropertyList& p) -> ToolResult {
            cJSON* dados = cJSON_CreateObject();
            cJSON_AddNumberToObject(dados, "dias", p["dias"].value<int>());
            return AcaoComTexto("agenda", dados);
        });

    mcp.AddTool(
        "self.agenda.cancelar_lembrete",
        "Cancela um lembrete futuro. Informe um trecho da descrição (ex.: 'dentista', 'laboratório'). "
        "Se houver mais de um parecido, o sistema cancela o mais próximo e avisa.",
        PropertyList({Property("trecho", kPropertyTypeString).SetMaxLength(120)}),
        [](const PropertyList& p) -> ToolResult {
            cJSON* dados = cJSON_CreateObject();
            cJSON_AddStringToObject(dados, "trecho", p["trecho"].value<std::string>().c_str());
            return AcaoComTexto("cancelar", dados);
        });
}

void IniciarVigia() {
    // Pilha em RAM interna: a tarefa grava na NVS (memória flash).
    xTaskCreatePinnedToCore(TarefaVigia, "assistente_vigia", 10240, nullptr, 2, nullptr, 0);
}

void EncerrarConversaAgora() {
    Disparar(TarefaEncerrarAgora, "encerrar_agora");
}

// Chamado pela tarefa de áudio a cada leitura do microfone (~10 ms).
// Precisa ser rápido: só contas simples, nada de rede nem memória.
// Nenhum áudio é guardado ou enviado: só o NÍVEL do som.
void MedirSom(const int16_t* amostras, int quantidade) {
    if (amostras == nullptr || quantidade <= 0) {
        return;
    }
    int64_t soma = 0;
    for (int i = 0; i < quantidade; ++i) {
        const int32_t v = amostras[i];
        soma += v * v;
    }
    const float media = static_cast<float>(soma) / static_cast<float>(quantidade);
    const float db = media >= 1.0f ? 10.0f * log10f(media / (32767.0f * 32767.0f)) : -96.0f;
    const int db10 = static_cast<int>(db * 10.0f);
    g_mic_atual.store(db10, std::memory_order_relaxed);
    if (db10 > g_mic_pico.load(std::memory_order_relaxed)) {
        g_mic_pico.store(db10, std::memory_order_relaxed);
    }

    // Ruído de fundo: desce rápido quando fica silêncio e sobe bem devagar,
    // para um ruído contínuo (ventilador, ar-condicionado) virar o "normal".
    static float fundo = 0.0f;
    static bool fundo_iniciado = false;
    if (!fundo_iniciado) {
        fundo = db;
        fundo_iniciado = true;
    } else if (db < fundo) {
        fundo += (db - fundo) * 0.01f;
    } else if (db < fundo + 6.0f) {
        fundo += (db - fundo) * 0.002f;
    } else {
        fundo += (db - fundo) * 0.0002f;
    }
    g_mic_fundo.store(static_cast<int>(fundo * 10.0f), std::memory_order_relaxed);

    // Modo vigia: barulho = som bem acima do fundo somando pelo menos 0,3 s
    // dentro de uma janela de 3 s (vozes, passos, objetos; um estalo isolado não conta).
    constexpr float kMinimoAbsolutoDb = -50.0f;
    constexpr int kQuadrosParaDisparar = 30;
    constexpr uint32_t kJanelaMs = 3000;
    static uint32_t janela_inicio = 0;
    static int quadros_altos = 0;
    static int pico_janela = kSemSom;
    static bool disparou = false;

    const uint32_t agora = static_cast<uint32_t>(AgoraMs());
    const bool em_silencio =
        static_cast<int32_t>(agora - g_silencio_ate_ms.load(std::memory_order_relaxed)) < 0;
    if (!g_vigia_ativo.load(std::memory_order_relaxed) || !g_ocioso.load(std::memory_order_relaxed) ||
        em_silencio) {
        quadros_altos = 0;
        disparou = false;
        return;
    }
    if (agora - janela_inicio > kJanelaMs) {
        janela_inicio = agora;
        quadros_altos = 0;
        pico_janela = kSemSom;
        disparou = false;
    }
    const float limiar = fundo + static_cast<float>(g_vigia_limiar_db.load(std::memory_order_relaxed));
    if (db > limiar && db > kMinimoAbsolutoDb) {
        ++quadros_altos;
        if (db10 > pico_janela) {
            pico_janela = db10;
        }
    }
    if (!disparou && quadros_altos >= kQuadrosParaDisparar) {
        disparou = true;
        g_barulho_pico.store(pico_janela, std::memory_order_relaxed);
        g_barulho_duracao_ms.store(quadros_altos * 10, std::memory_order_relaxed);
        g_barulho_pendente.store(true, std::memory_order_release);
    }
}

}  // namespace assistente
