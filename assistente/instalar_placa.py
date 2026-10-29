#!/usr/bin/env python3
"""
Prepara o repositório xiaozhi-esp32 para compilar a placa "assistente-amiga".

Uso (na pasta raiz do repositório xiaozhi-esp32):

  python assistente/instalar_placa.py --registrar
      Registra a placa no main/CMakeLists.txt e no main/Kconfig.projbuild.
      Pode rodar quantas vezes quiser (não duplica nada).

  python assistente/instalar_placa.py --gerar
      Cria main/boards/assistente-amiga/gerado.h a partir de variáveis de
      ambiente (ou dos valores vazios, se não existirem):

        ASSISTENTE_VERSAO   número da versão (ex.: 3)            [obrigatório]
        WIFI_1_SSID / WIFI_1_SENHA   rede para testar na sua casa
        WIFI_2_SSID / WIFI_2_SENHA   rede da casa/consultório dela
        APPS_SCRIPT_URL     URL /exec do Google Apps Script
        APPS_SCRIPT_TOKEN   senha combinada com o Apps Script
        OTA_MANIFESTO_URL   URL do manifest.json da atualização remota
        LEMBRETE_FALADO     1 = a IA fala o lembrete em voz alta, 0 = só tela e som

  python assistente/instalar_placa.py --gerar --sem-segredos
      Igual, mas deixa Wi-Fi, URL e token vazios. Usado no firmware de
      ATUALIZAÇÃO REMOTA: o aparelho continua usando o que já guardou na
      memória, e nenhum segredo vai parar num arquivo público.
"""

import argparse
import os
import re
import sys
from pathlib import Path

PLACA = "assistente-amiga"
SIMBOLO = "BOARD_TYPE_ASSISTENTE_AMIGA"
ROTULO_KCONFIG = "Assistente Amiga (ESP32-S3 N16R8 + GC9A01 redondo)"

BLOCO_CMAKE = f"""elseif(CONFIG_{SIMBOLO})
    set(BOARD_DIR "{PLACA}")
    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)
    set(BUILTIN_ICON_FONT font_material_symbols_16_4)
    set(DEFAULT_EMOJI_COLLECTION noto-color-emoji_64)
"""

BLOCO_KCONFIG = f"""    config {SIMBOLO}
        bool "{ROTULO_KCONFIG}"
        depends on IDF_TARGET_ESP32S3
"""



def chave_natural(rotulo: str):
    """Mesma ordenação usada nos testes do projeto (menu de placas em ordem alfabética)."""
    rotulo = re.sub(r"\s+\([^)]*[\u3400-\u9fff][^)]*\)$", "", rotulo.casefold())
    return tuple(
        (1, float(parte)) if re.fullmatch(r"\d+(?:\.\d+)?", parte) else (0, parte)
        for parte in re.split(r"(\d+(?:\.\d+)?)", rotulo)
    )


def raiz() -> Path:
    r = Path.cwd()
    if not (r / "main" / "CMakeLists.txt").exists():
        sys.exit("Rode este script na pasta raiz do repositório xiaozhi-esp32.")
    if not (r / "main" / "boards" / PLACA / "config.h").exists():
        sys.exit(f"Copie antes a pasta main/boards/{PLACA} para o repositório.")
    return r


def registrar(r: Path) -> None:
    cmake = r / "main" / "CMakeLists.txt"
    texto = cmake.read_text(encoding="utf-8")
    if f"CONFIG_{SIMBOLO}" in texto:
        print("CMakeLists.txt: placa já registrada.")
    else:
        m = re.search(r"^elseif\(CONFIG_BOARD_TYPE_", texto, re.MULTILINE)
        if not m:
            sys.exit("Não achei onde inserir no main/CMakeLists.txt (o projeto mudou?).")
        texto = texto[:m.start()] + BLOCO_CMAKE + texto[m.start():]
        cmake.write_text(texto, encoding="utf-8")
        print("CMakeLists.txt: placa registrada.")

    kconfig = r / "main" / "Kconfig.projbuild"
    texto = kconfig.read_text(encoding="utf-8")
    if f"config {SIMBOLO}" in texto:
        print("Kconfig.projbuild: placa já registrada.")
    else:
        ini = texto.find("choice BOARD_TYPE")
        fim = texto.find("endchoice", ini)
        if ini < 0 or fim < 0:
            sys.exit("Não achei o menu de placas no main/Kconfig.projbuild (o projeto mudou?).")
        posicao = fim
        entrada = re.compile(r'^    config BOARD_TYPE_\w+\n        bool "([^"]+)"\n', re.MULTILINE)
        for m in entrada.finditer(texto, ini, fim):
            if chave_natural(m.group(1)) > chave_natural(ROTULO_KCONFIG):
                posicao = m.start()
                break
        texto = texto[:posicao] + BLOCO_KCONFIG + texto[posicao:]
        kconfig.write_text(texto, encoding="utf-8")
        print("Kconfig.projbuild: placa registrada.")

    gitignore = r / ".gitignore"
    linha = f"main/boards/{PLACA}/gerado.h"
    atual = gitignore.read_text(encoding="utf-8") if gitignore.exists() else ""
    if linha not in atual:
        with gitignore.open("a", encoding="utf-8") as f:
            f.write(("\n" if atual and not atual.endswith("\n") else "") + linha + "\n")
        print(".gitignore: gerado.h nunca será enviado ao GitHub.")


def c_str(valor: str) -> str:
    """Converte para literal C seguro (aspas, barras e acentos)."""
    saida = []
    for byte in valor.encode("utf-8"):
        ch = chr(byte)
        if ch in '"\\':
            saida.append("\\" + ch)
        elif 32 <= byte < 127:
            saida.append(ch)
        else:
            saida.append(f'\\{byte:03o}')
    return '"' + "".join(saida) + '"'


def gerar(r: Path, sem_segredos: bool) -> None:
    def env(nome: str, padrao: str = "") -> str:
        return os.environ.get(nome, padrao).strip()

    versao = env("ASSISTENTE_VERSAO")
    if not versao:
        sys.exit("Defina ASSISTENTE_VERSAO (ex.: export ASSISTENTE_VERSAO=1).")

    valores = {
        "WIFI_1_SSID": env("WIFI_1_SSID"),
        "WIFI_1_SENHA": env("WIFI_1_SENHA"),
        "WIFI_2_SSID": env("WIFI_2_SSID"),
        "WIFI_2_SENHA": env("WIFI_2_SENHA"),
        "APPS_SCRIPT_URL": env("APPS_SCRIPT_URL"),
        "APPS_SCRIPT_TOKEN": env("APPS_SCRIPT_TOKEN"),
        "OTA_MANIFESTO_URL": env("OTA_MANIFESTO_URL"),
    }
    if sem_segredos:
        valores = {k: "" for k in valores}

    falado = env("LEMBRETE_FALADO", "1")
    falado = "0" if falado.lower() in ("0", "false", "nao", "não", "no") else "1"

    linhas = [
        "// ARQUIVO GERADO por assistente/instalar_placa.py — NÃO envie ao GitHub.",
        "#pragma once",
        f"#define ASSISTENTE_VERSAO {c_str(versao)}",
    ]
    linhas += [f"#define {k} {c_str(v)}" for k, v in valores.items()]
    linhas += [
        f"#define LEMBRETE_FALADO {falado}",
        "#define INTERVALO_VERIFICACAO_MIN 2",
        "",
    ]
    destino = r / "main" / "boards" / PLACA / "gerado.h"
    destino.write_text("\n".join(linhas), encoding="utf-8")

    print(f"gerado.h criado (versão {versao}"
          f"{', SEM segredos — firmware de atualização remota' if sem_segredos else ''}).")
    if not sem_segredos:
        faltando = [k for k, v in valores.items() if not v and not k.startswith("WIFI_1")]
        if faltando:
            print("  Atenção, vazios:", ", ".join(faltando))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--registrar", action="store_true", help="registra a placa no projeto")
    p.add_argument("--gerar", action="store_true", help="gera main/boards/assistente-amiga/gerado.h")
    p.add_argument("--sem-segredos", action="store_true", help="firmware de atualização remota")
    a = p.parse_args()
    if not (a.registrar or a.gerar):
        p.print_help()
        return
    r = raiz()
    if a.registrar:
        registrar(r)
    if a.gerar:
        gerar(r, a.sem_segredos)


if __name__ == "__main__":
    main()
