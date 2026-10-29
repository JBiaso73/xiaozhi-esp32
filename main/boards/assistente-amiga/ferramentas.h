#ifndef _ASSISTENTE_FERRAMENTAS_H_
#define _ASSISTENTE_FERRAMENTAS_H_

// Recursos extras da assistente, além do firmware xiaozhi padrão:
//  - e-mail para a dona (receitas, listas, textos para imprimir)
//  - lembretes/agenda guardados numa planilha do Google (Apps Script)
//  - lembrete aparece na tela, toca um aviso e (opcional) é falado
//  - ferramenta para a própria IA encerrar a conversa ("não é com você")
//  - atualização remota do firmware a partir do GitHub do dono
//  - "sinal de vida" a cada poucos minutos, para o dono saber se está online

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

}  // namespace assistente

#endif
