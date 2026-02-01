# PS2Recomp - Plano de Desenvolvimento Completo

## Estado Atual do Projeto (Janeiro 2026)

### O que está funcionando ✅
- Decodificação de instruções MIPS R5900 (completo com extensões PS2)
- Geração de código C++ a partir de MIPS (funcional)
- Parser de ELF com robustez melhorada (commit 22789dc)
- Simulação de memória PS2 (32MB RAM, scratchpad)
- System calls básicas (File I/O, malloc/free)
- Sistema de configuração TOML
- Compilação Linux funcionando

### Crítico: Bloqueio Principal ❌
**O `register_functions.cpp` está VAZIO** - apenas um placeholder de 5 linhas. O recompilador gera código C++ das funções, mas não há mecanismo para registrar essas funções no runtime. Sem isso, o código recompilado não pode ser executado.

### TODOs Críticos Encontrados
1. **SSE4.1 → SSE2** (`code_generator.cpp:1189`) - 2 instruções requerem SSE4.1
2. **System calls** - ~15 funções marcadas como TODO em `ps2_syscalls.cpp`
3. **Stubs** - Funções de printf/scanf ignoram argumentos além do format string
4. **Path translation** - Suporte para paths PS2 (mc0:, host:, cdrom:) não implementado

### Testes: Gaps Significantes
- ✅ Testes unitários de decoder (r5900_decoder_tests.cpp)
- ✅ Testes unitários de code generator (code_generator_tests.cpp)
- ❌ Sem testes de ELF parser
- ❌ Sem testes de integração end-to-end
- ❌ Sem dados de teste (ELFs de exemplo)
- ❌ CI apenas testa Windows (continue-on-error: true)

---

## Plano de Implementação (Reordenado: Testes Primeiro)

### FASE 1: Infraestrutura de Testes (PRIORIDADE MÁXIMA)

**Objetivo:** Criar infraestrutura sólida de testes antes de implementar novas funcionalidades.

#### 1.1 Criar ELF Sintético de Teste
**Novo diretório:** `ps2xTest/test_data/`

- Criar utilitário ou script para gerar ELF MIPS R5900 de teste
- ELF deve conter:
  - Funções simples (aritmética, branches, calls)
  - Tabelas de símbolos válidas
  - Seções de código e dados bem definidas
- Documentar formato esperado para ELFs de teste
- Incluir vários casos: função única, múltiplas funções, com branches, etc.

#### 1.2 Testes de ELF Parser
**Novo arquivo:** `ps2xTest/elf_parser_tests.cpp`

- Testar parsing de ELF válido com símbolos
- Testar ELF sem símbolos (fallback de entry point - do commit 22789dc)
- Testar seções inválidas ou malformadas
- Testar extração de símbolos exportados/importados
- Testar relocations
- Testar ELF sintético criado em 1.1

#### 1.3 Testes de Configuração
**Novo arquivo:** `ps2xTest/config_tests.cpp`

- Testar parsing de TOML válido
- Testar validação de configs inválidas
- Testar processamento de stubs/skip/patches
- Testar caminhos de arquivo relativos/absolutos

#### 1.4 Testes de Code Generator
**Expandir arquivo:** `ps2xTest/code_generator_tests.cpp`

- Adicionar testes para geração de register_functions.cpp
- Testar que todas as funções são registradas corretamente
- Testar mapeamento de endereço → nome de função
- Testar sanitização de identificadores

#### 1.5 Testes de Runtime
**Novo arquivo:** `ps2xTest/runtime_tests.cpp`

- Testar registro de funções (registerFunction)
- Testar lookup de funções (hasFunction, lookupFunction)
- Testar memória (read/write em várias regiões)
- Testar R5900Context (GPRs, COP0, VU0)
- Testar loading de ELFs válidos e inválidos

---

### FASE 2: Tornar o Sistema Executável (CRÍTICA)

**Objetivo:** Permitir que código recompilado seja executado end-to-end.

#### 2.1 Implementar Geração Automática de `register_functions.cpp`
**Arquivos:** `ps2xRecomp/src/lib/code_generator.cpp`, `ps2xRecomp/src/lib/ps2_recompiler.cpp`

- Modificar o code generator para escrever automaticamente o arquivo `register_functions.cpp`
- Cada função recompilada deve gerar uma chamada `runtime.registerFunction(address, function_name)`
- O arquivo gerado deve incluir os headers necessários e implementar `registerAllFunctions()`
- Adicionar opção de config para caminho de saída do registro

**Saída esperada:** Após recompilação, `register_functions.cpp` conterá:
```cpp
void registerAllFunctions(PS2Runtime &runtime) {
    runtime.registerFunction(0x100000, func_00100000);
    runtime.registerFunction(0x100050, func_00100050);
    // ... todas as funções recompiladas
}
```

#### 2.2 Implementar SSE2 Fallback para Instruções SSE4.1
**Arquivo:** `ps2xRecomp/src/lib/code_generator.cpp:1189`

- As 2 instruções que requerem SSE4.1 precisam de implementação SSE2 alternativa
- Permitir execução em CPUs mais antigas
- Adicionar detecção de CPU em runtime (opcional)

#### 2.3 Criar Teste End-to-End (Integração)
**Novo arquivo:** `ps2xTest/integration_tests.cpp`

- Usar ELF sintético de teste da Fase 1
- Teste completo: recompilar → compilar C++ → executar → verificar resultado
- Incluir no CMake como novo target de teste
- Verificar que register_functions.cpp foi gerado corretamente
- Verificar que funções podem ser chamadas no runtime

#### 2.4 Melhorar CI/CD
**Arquivo:** `.github/workflows/build.yml`

- Adicionar job de Linux (GCC/Clang)
- Remover `continue-on-error: true` quando testes passarem
- Adicionar build matrix: Windows MSVC, Linux GCC, Linux Clang
- Upload de artefatos Linux

---

### FASE 3: Completar System Calls Críticas

**Objetivo:** Implementar as system calls mais comuns para executar programas PS2 reais.

#### 3.1 Priorizar System Calls
**Arquivo:** `ps2xRuntime/src/lib/ps2_syscalls.cpp`

Implementar na ordem de prioridade:
1. **Thread management** - CreateThread, StartThread, ExitThread, iTerminateThread
2. **Semaforos/mutex** - CreateSema, SignalSema, WaitSema
3. **Timing** - GetOsdTime, SetOsdTime
4. **Padrão libc** - snprintf, vsnprintf (argumentos beyond format string)
5. **Path translation** - mc0:, host:, cdrom: paths

#### 3.2 Implementar Path Translation
**Arquivo:** `ps2xRuntime/src/lib/ps2_stubs.cpp:713`

- Criar função `translatePS2Path()` para converter paths PS2 → nativos
- Suportar: mc0:, mc1:, host0:, cdrom0:, pfs0:
- Adicionar config para mapeamento de diretórios

---

### FASE 4: Subsistemas de Hardware (ADIADO - FUTURO)

**Objetivo:** Implementar saída de vídeo e áudio básica.
**Nota:** Esta fase foi adiada conforme solicitação do usuário. Runtime funcional pode existir sem gráficos/áudio.

#### 4.1 Graphics Synthesizer (GS) Básico (FUTURO)
**Novo arquivo:** `ps2xRuntime/src/lib/gs_output.cpp`

- Implementar escrita para VRAM
- Salvar framebuffer para arquivo PNG/PPM (inicialmente)
- Renderização via janela pode ser adicionada depois
- Suporte para resoluções comuns (640x480, etc.)

#### 4.2 Áudio Básico (SPU) (FUTURO)
**Novo arquivo:** `ps2xRuntime/src/lib/spu_output.cpp`

- Buffer de áudio SPU2
- Output via biblioteca de áudio ou salvar para arquivo WAV
- Formato 48kHz estéreo (PS2 padrão)

---

### FASE 5: Otimizações e Melhorias de Performance

**Objetivo:** Melhorar desempenho e usabilidade.

#### 5.1 Otimizações
- Cache de lookup de funções
- Otimizações de SSE/AVX onde aplicável
- Profile-guided optimization (PGO)

#### 5.2 Ferramentas de Debug
- Logging configurável
- Debug info nas funções geradas
- Trace de execução

#### 5.3 Documentação
- Atualizar README com status atual
- Adicionar guia de uso completo
- Adicionar exemplos de config para jogos conhecidos

---

## Verificação e Testes

### Fase 1: Testes Unitários (Após Fase 1)
```bash
# 1. Compilar projeto com testes
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
cmake --build .

# 2. Executar testes existentes
./ps2x_tests  # Decoder e code generator tests devem passar

# 3. Executar novos testes (Fase 1)
./ps2x_tests --run=ElfParserTests
./ps2x_tests --run=ConfigTests
./ps2x_tests --run=RuntimeTests
```

### Fase 2: Teste End-to-End (Após Fase 2)
```bash
# 1. Compilar projeto
mkdir build && cd build
cmake .. && cmake --build .

# 2. Criar config de teste usando ELF sintético
cat > test_config.toml << EOF
[general]
input = "ps2xTest/test_data/simple.elf"
output = "output/"
single_file_output = false
EOF

# 3. Executar recompilador
./ps2recomp test_config.toml

# 4. Verificar que register_functions.cpp foi gerado
cat output/register_functions.cpp  # Deve conter chamadas de registerFunction

# 5. Compilar código gerado + runtime
# (automatizado via CMake ou script)

# 6. Executar runtime
./ps2EntryRunner ps2xTest/test_data/simple.elf

# 7. Verificar resultado esperado
./ps2x_tests --run=IntegrationTests
```

### Teste Cross-Platform (Após Fase 2.4)
- Build no Windows (MSVC)
- Build no Linux (GCC)
- Build no Linux (Clang)
- CI/CD deve passar em todos os 3 ambientes

---

## Ordem de Prioridade (Atualizada: Testes Primeiro)

1. **CRÍTICO:** Fase 1.1 - Criar ELF sintético de teste
2. **CRÍTICO:** Fase 1.2-1.5 - Testes de parser/config/runtime/codegen
3. **CRÍTICO:** Fase 2.1 - Geração automática de register_functions.cpp
4. **CRÍTICO:** Fase 2.3 - Teste end-to-end (integração)
5. **ALTA:** Fase 2.2 - SSE2 fallback
6. **ALTA:** Fase 2.4 - CI/CD Linux
7. **MÉDIA:** Fase 3 - System calls críticas
8. **BAIXA:** Fase 4 - Hardware (GS/SPU) - ADIADO
9. **BAIXA:** Fase 5 - Otimizações

---

## Arquivos Críticos a Modificar

### Testes (Fase 1 - PRIORIDADE)
- `ps2xTest/test_data/` - NOVO: Diretório para dados de teste
- `ps2xTest/elf_parser_tests.cpp` - NOVO: Testes de ELF parser
- `ps2xTest/config_tests.cpp` - NOVO: Testes de configuração
- `ps2xTest/runtime_tests.cpp` - NOVO: Testes de runtime
- `ps2xTest/integration_tests.cpp` - NOVO: Testes end-to-end
- `ps2xTest/code_generator_tests.cpp` - EXPANDIR: Adicionar testes de register_functions

### Recompilador (Fase 2)
- `ps2xRecomp/src/lib/code_generator.cpp` - Geração de código + register_functions
- `ps2xRecomp/src/lib/ps2_recompiler.cpp` - Pipeline principal
- `ps2xRecomp/include/ps2recomp/types.h` - Estruturas de dados

### Runtime (Fase 3)
- `ps2xRuntime/src/runner/register_functions.cpp` - Será gerado automaticamente
- `ps2xRuntime/src/lib/ps2_syscalls.cpp` - System calls
- `ps2xRuntime/src/lib/ps2_stubs.cpp` - Stubs de libc

### CI/CD
- `.github/workflows/build.yml` - Adicionar builds Linux

### Hardware (Fase 4 - ADIADO)
- `ps2xRuntime/src/lib/gs_output.cpp` - NOVO FUTURO: Graphics output
- `ps2xRuntime/src/lib/spu_output.cpp` - NOVO FUTURO: Audio output

---

## Estimativa de Esforço (Atualizada: Testes Primeiro)

- Fase 1 (Testes): 2-3 dias
  - Criar ELF sintético: 0.5-1 dia
  - Testes de parser/config/runtime: 1-2 dias
- Fase 2 (Executável): 3-4 dias
  - Geração de register_functions.cpp: 1-2 dias
  - SSE2 fallback: 0.5 dia
  - Teste end-to-end: 1 dia
  - CI/CD: 0.5 dia
- Fase 3 (System calls): 3-5 dias
- Fase 4 (Hardware): 5-7 dias (ADIADO)
- Fase 5 (Otimizações): 2-3 dias

**Total para funcionalidade básica (sem gráficos):** 8-12 dias
**Total para produto completo (com gráficos):** 13-19 dias
