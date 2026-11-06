# DEVELOPMENT_STATUS — versão nativa Windows + pré-compilação de shaders

Este arquivo registra o trabalho de transformar o fork em um **ZIP Windows x64 pronto para uso**, montado pelo GitHub
Actions, com **pré-compilação de shaders**. Ele é atualizado a cada etapa. O estado do *jogo* (marcos M1…M7) continua em
`docs/STATUS.md`.

Regra deste arquivo: só consta como "feito" o que foi **executado e visto funcionando**. O que foi escrito mas não pôde
ser executado está marcado como **não verificado**, com o motivo.

## Limites do ambiente em que isto foi escrito (leia primeiro)

| Limite | Efeito |
|---|---|
| Sessão sem rede para repositórios de pacotes (apt, PyPI, MSYS2, ziglang.org) | Não dá para instalar SDL3/Vulkan/shaderc/FFmpeg aqui: **o runtime completo não foi compilado nesta sessão** |
| Sandbox é Linux x86-64; não há Windows nem GPU | **O workflow do Actions ainda não rodou.** Nada do que é específico de Windows foi executado |
| `git push` da sessão recusado (o app "Claude" do GitHub não está instalado no fork) | As mudanças ficam numa branch local; ver "Como receber o trabalho" no fim |
| Sem os arquivos do jogo (e eles não devem ser pedidos) | Tudo que toca o jogo real (geração do código, shaders reais, jogabilidade) **não foi testado** |

O que **foi** executado aqui: módulos em C isolados (cache de SPIR-V, corpus/preparação de shaders) compilados com clang
no Linux e exercitados por testes próprios, incluindo o tradutor Maxwell real (`translate.c`) com programas sintéticos.

## Etapa 1 — Análise do repositório

Concluída. Resumo do que existe e do que falta.

### Arquitetura (resumo; detalhes em `docs/ARCHITECTURE.md`)
- Recompilação estática ARM64 → C (`tools/lifter.py`, `tools/gen.py`) do jogo (Switch, title `01008CF01BAAC000`, v1.0.0).
  O C gerado **nunca** vai ao repositório: é produzido na máquina do usuário a partir dos arquivos dele.
- Runtime (`runtime/`): substitui Horizon/NVN. Backend gráfico **Vulkan** (`runtime/nvn/vulkan/`) traduz shaders
  Maxwell → GLSL (`runtime/nvn/maxwell/translate.c`) e compila GLSL → SPIR-V com **shaderc** (`shaders.c`).
- Tela inicial (`runtime/ui/launcher.mm`, ImGui): seleciona XCI/NSP + `prod.keys`, lê o pacote em C
  (`runtime/nxpkg.c`, AES/XTS/LZ4/BucketTree), extrai só o ExeFS e chama `gamebuild_run`
  (`runtime/gamebuild.c`), que executa o gerador (`eowgen`, PyInstaller) e compila o resultado com **zig cc**
  em `eowgame.dll`, com barra de progresso. **Esta parte já existe**; não precisa de terminal.

### O que já funciona (segundo `docs/STATUS.md`)
- Linux/Vulkan: jogo inicia, abertura e primeira cena com input (Intel Iris Xe). macOS/Android: build e tela inicial.
- Windows: `.exe` **cross-compilado do Linux** (zig cc, alvo `x86_64-windows-gnu`) linka. Nunca foi executado no Windows
  (README: "untested").

### Problemas encontrados (a resolver nas etapas seguintes)
1. **Não há `.github/`**: nenhum workflow, nenhuma release no fork.
2. **Alvos `dist` e `deps-windows` não existem no `Makefile`** (a doc `docs/BUILD-windows.md` herdada do LAS os cita).
3. **Pacote Windows incompleto**: `tools/windows/build.sh` copia só `eowrecomp.exe` + DLLs. Faltam o que o
   `gamebuild.c` exige ao lado do `.exe`: `eowgen/`, `zig/`, `gamelib-src/` (cpu.h, simd*.h, gamelib.h, modules.h,
   gamelib_entry.c, hle_symbols.txt) e a import library.
4. **Nome da import library inconsistente**: `Makefile` gera `eowrecomp.lib`; `gamebuild.c` procura `eowrecomp.dll.a`.
5. `tools/test_translate.sh` usa o commit de referência `652e234` (do LAS), **inexistente no fork** (31 commits):
   o script falha em qualquer clone.
6. `tools/setup_windows.ps1` e `docs/BUILD-windows.md` ainda falam de LARecomp / LAS-Randomizer / `rando-gen`.
7. Cache de SPIR-V (`cache/spirv/`) **sem versão, sem checksum, sem invalidação** por GPU/driver/formato; só é
   preenchido durante a jogatina. O cache de pipelines do driver (`cache/vk_pipelines.bin`) não valida o dispositivo.
8. Sem teste de inicialização do `.exe` e sem verificação automática das DLLs do pacote.

### Decisões de arquitetura (e por quê)
- **Build nativo = MSYS2 UCRT64 (clang + lld + make) dentro de um runner Windows do Actions.** É o caminho que o próprio
  `docs/BUILD-windows.md` descreve e o que o `Makefile` já suporta (`PLATFORM=windows`). **Não foi feito um port para
  CMake/MSVC**: o código usa extensões GNU/Clang (`__uint128_t`, atributos, ObjC++-como-C++), o código do jogo é
  compilado na máquina do usuário por zig/clang de qualquer forma, e reescrever o sistema de build arriscaria quebrar o
  que funciona. Isto diverge do pedido original ("CMake, Ninja"); a justificativa é a compatibilidade real do código.
- O executável sai de um ambiente Windows nativo; **sem cross-compilação**. O script cruzado (`tools/windows/build.sh`)
  continua existindo e não foi alterado em comportamento.
- Geração/compilação do código do jogo acontece **na máquina do usuário** (obrigatório: depende dos arquivos dele), já
  com interface de progresso. O ZIP leva `eowgen`, `zig` e os cabeçalhos necessários.

## Etapa 2 — Build nativo Windows + Actions
Pendente. (percentual real: 0 %)

## Etapa 3 — Pré-compilação de shaders
Pendente. (percentual real: 0 %)

## Etapa 4 — Empacotamento e publicação
Pendente. (percentual real: 0 %)

## Erros encontrados e soluções
| Erro | Causa | Solução |
|---|---|---|
| `test_translate.sh` falharia em qualquer clone | commit de referência `652e234` do LAS não existe no fork | ver Etapa 3 (script adaptado) |
