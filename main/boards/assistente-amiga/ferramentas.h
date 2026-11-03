#ifndef _ASSISTENTE_FERRAMENTAS_H_
#define _ASSISTENTE_FERRAMENTAS_H_

// Recursos extras da assistente, além do firmware xiaozhi padrão:
//  - e-mail para a dona (receitas, listas, textos para imprimir)
//  - lembretes/agenda guardados numa planilha do Google (Apps Script)
//  - lembrete aparece na tela, toca um aviso e (opcional) é falado
//  - ferramenta para a própria IA encerrar a conversa ("não é com você")
//  - atualização remota do firmware a partir do GitHub do dono
//  - "sinal de vida" a cada poucos minutos, para o dono saber se está online
//  - nível do microfone no painel (diagnóstico à distância)
//  - modo vigia: fora do horário, barulho forte gera um e-mail de aviso

#include <cstdint>

namespace assistente {

// Chamar no construtor da placa, ANTES da rede iniciar.
// Grava na memória (NVS) os segredos que vieram no firmware de instalação
// e cadastra as redes Wi-Fi pré-configuradas.
void Inicializar();

// Registra as ferramentas MCP que a IA pode usar.
void RegistrarFerramentas();

// Tarefa de fundo: lembretes pendentes, sinal de vida e atualização remota.
void IniciarVigia();

// Encerra a conversa imediatamente (usado ao segurar o sensor de toque).
void EncerrarConversaAgora();

// Conta uma mudança no sensor de toque (diagnóstico mostrado no painel).
void RegistrarToque();

// Mede o nível do microfone (chamado pelo codec a cada leitura, ~10 ms).
// Alimenta o diagnóstico do painel e o modo vigia. Não grava áudio.
// "cortes" = amostras que saturaram (som alto demais para o ganho atual).
void MedirSom(const int16_t* amostras, int quantidade, int cortes);

// Quantos bits o som do microfone é deslocado (12 = padrão do XiaoZhi).
// Maior = menos ganho. Ajustado à distância pelo MIC_AJUSTE_DB do Apps Script.
int DeslocamentoMic();

// O codec informa a energia média de cada canal do microfone e qual está em uso.
void InformarCanais(float energia_esquerda, float energia_direita, bool usando_direito);

}  // namespace assistente

#endif
