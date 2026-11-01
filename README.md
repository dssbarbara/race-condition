# Race Condition no Zephyr: contador de peças de uma esteira

Projeto didático que **mostra e corrige** uma condição de corrida (*race condition*) no Zephyr RTOS 4.2, na placa FRDM-KL25Z (Cortex-M0+, 48 MHz).

## Cenário real

Uma esteira industrial tem dois sensores ópticos (A e B) que contam peças em linhas paralelas. Cada sensor é tratado por uma thread, e as duas incrementam o **mesmo contador global** de peças produzidas. Esse total alimenta o controle de estoque: se peças forem contadas a menos, o estoque fica errado.

## Por que a corrida acontece

`contador++` parece uma operação só, mas são três passos:

1. **ler** o valor da RAM;
2. **somar** 1;
3. **escrever** de volta.

Se o escalonador troca de thread entre o passo 1 e o 3, a outra thread incrementa o contador várias vezes, e depois a primeira grava o valor **antigo** + 1, apagando essas contagens (*lost update*).

Entre a leitura e a escrita o código simula um processamento (`JANELA_NOPS`, a "validação do sensor"), o que aumenta a chance de a troca de thread cair nessa janela.

As duas threads têm a **mesma prioridade (5)**. Sem *time slicing*, cada uma rodaria até o fim sem ser interrompida pela outra, e o erro nunca apareceria. Por isso o `prj.conf` ativa `CONFIG_TIMESLICE_SIZE=1` (fatia de 1 ms).

## O que o programa faz

Para cada um dos 3 modos, executa 5 rodadas. Em cada rodada, duas threads contam `N_CONTAGENS = 500000` peças cada, então o valor esperado é 1.000.000. O contador é zerado a cada rodada. A `main` espera as threads com `k_thread_join` e registra, pelo Logging: valor esperado, valor obtido, peças perdidas, percentual de erro e tempo da rodada (medido com `k_cycle_get_32()`).

| Modo | Técnica | Resultado esperado |
|---|---|---|
| `SEM PROTECAO` | `contador++` sem proteção | Valor obtido menor que o esperado, mudando a cada rodada |
| `MUTEX` | `k_mutex_lock` / `k_mutex_unlock` em volta da seção crítica | Sempre igual ao esperado, porém mais lento |
| `ATOMIC` | `atomic_inc` | Sempre igual ao esperado |

Sobre o modo `ATOMIC`: ele protege só o incremento. O processamento de validação fica fora do `atomic_inc`, porque uma operação atômica não protege um bloco de várias instruções. Para proteger o bloco inteiro, use mutex. No Cortex-M0+ não há instruções exclusivas (LDREX/STREX), então o Zephyr implementa o `atomic` desabilitando interrupções por alguns ciclos.

## Como compilar e gravar

```bash
west build -b frdm_kl25z -p
west flash
```

Para ver a saída (Linux), a placa aparece como porta serial virtual, em 115200 baud:

```bash
sudo stty -F /dev/ttyACM0 115200 raw -echo
sudo cat /dev/ttyACM0
```

O programa espera 5 s antes de começar (`ATRASO_INICIAL_MS`), para dar tempo de abrir o terminal. Aperte o botão **RESET** para repetir o experimento.

## Formato da saída

Os números dependem da execução na placa e **não foram incluídos aqui**. O formato é:

```
<inf> race_demo: === Modo: SEM PROTECAO | 5 rodadas ===
<wrn> race_demo: [SEM PROTECAO] rodada 1: esperado=1000000 obtido=<...> perdidas=<...> erro=<...>% tempo=<...> ms
...
<inf> race_demo: [SEM PROTECAO] RESUMO: <...> de 5 rodadas com perda | ...
<inf> race_demo: === Modo: MUTEX | 5 rodadas ===
<inf> race_demo: [MUTEX] rodada 1: esperado=1000000 obtido=1000000 perdidas=0 erro=0.00% tempo=<...> ms
```

## Ajustes

Todos os parâmetros estão no início de `src/main.c`:

- `JANELA_NOPS`: se nenhuma perda aparecer no modo sem proteção, aumente este valor.
- `N_CONTAGENS` e `N_RODADAS`: tamanho do experimento. Cada rodada precisa durar menos de ~89 s, que é quando `k_cycle_get_32()` estoura a 48 MHz.
- `EXECUTAR_TODOS_OS_MODOS`: coloque `0` para rodar só o `MODO_UNICO`.

## Estrutura

```
race_condition_esteira/
├── CMakeLists.txt
├── prj.conf
├── PROMPT.md        # prompt usado para gerar o projeto
└── src/main.c
```

## Observação

O código foi gerado por IA a partir do prompt em `PROMPT.md`. Os resultados do relatório devem vir da execução real na placa.