/*
 * Condicao de corrida (race condition) no Zephyr RTOS
 *
 * CENARIO REAL: contador de pecas de uma esteira industrial.
 * Dois sensores opticos (A e B) contam pecas em linhas paralelas. Cada sensor
 * e tratado por uma thread, e as duas incrementam o MESMO contador global
 * (total de pecas produzidas), que alimenta o controle de estoque.
 * Pecas contadas a menos = estoque errado.
 *
 * O programa roda 3 modos, 5 rodadas cada, e registra pelo Logging:
 *   1) SEM PROTECAO  -> mostra o problema (pecas perdidas)
 *   2) MUTEX         -> correcao com k_mutex
 *   3) ATOMIC        -> correcao com atomic_inc
 *
 * Placa: FRDM-KL25Z (Cortex-M0+, 48 MHz) | Zephyr 4.2
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(race_demo, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Parametros do experimento                                          */
/* ------------------------------------------------------------------ */
#define N_CONTAGENS        500000U  /* contagens por thread, por rodada      */
#define N_RODADAS          5U       /* rodadas por modo                      */
#define PRIO_SENSOR        5        /* MESMA prioridade nas duas threads     */
#define STACK_SIZE         1024
#define JANELA_NOPS        20       /* "validacao do sensor": aumenta a janela
                                       entre a leitura e a escrita; se nenhuma
                                       perda aparecer, aumente este valor     */
#define ATRASO_INICIAL_MS  5000     /* tempo para abrir o terminal serial    */

/* 1 = roda os tres modos em sequencia; 0 = roda so o MODO_UNICO */
#define EXECUTAR_TODOS_OS_MODOS 1
#define MODO_UNICO              MODO_SEM_PROTECAO

/* Sem time slicing, as duas threads (mesma prioridade) rodariam uma de cada
 * vez, cada uma ate o fim, e a corrida nunca apareceria. */
BUILD_ASSERT(CONFIG_TIMESLICE_SIZE > 0,
	     "Ative CONFIG_TIMESLICE_SIZE no prj.conf (time slicing)");

enum modo {
	MODO_SEM_PROTECAO = 0,
	MODO_MUTEX,
	MODO_ATOMIC,
	MODO_TOTAL
};

static const char *const nome_modo[MODO_TOTAL] = {
	"SEM PROTECAO", "MUTEX", "ATOMIC"
};

/* ------------------------------------------------------------------ */
/* Recurso compartilhado                                              */
/* ------------------------------------------------------------------ */
static volatile uint32_t contador;                 /* modos 0 e 1 */
static atomic_t contador_atomico = ATOMIC_INIT(0); /* modo 2      */
static K_MUTEX_DEFINE(mtx_contador);

/* Simula o processamento entre ler o contador e gravar o novo valor
 * (por exemplo, validar a leitura do sensor). Nao muda o contador. */
static inline void processa_leitura(void)
{
	for (int i = 0; i < JANELA_NOPS; i++) {
		__asm__ volatile("nop");
	}
}

/* ------------------------------------------------------------------ */
/* Tres formas de incrementar                                         */
/* ------------------------------------------------------------------ */

/* PROBLEMA: contador++ NAO e atomico. Sao tres passos:
 *   1. le o valor da RAM
 *   2. calcula valor + 1 (aqui, com a janela de processamento)
 *   3. escreve de volta
 * Se o escalonador troca de thread entre 1 e 3, a outra thread incrementa
 * o contador varias vezes e depois esta thread grava o valor ANTIGO + 1,
 * apagando todas essas contagens (lost update). */
static void inc_sem_protecao(void)
{
	/* ===== INICIO DA SECAO CRITICA (sem protecao) ===== */
	uint32_t tmp = contador;     /* 1. le                 */
	processa_leitura();          /*    janela de corrida  */
	contador = tmp + 1U;         /* 3. escreve            */
	/* ===== FIM DA SECAO CRITICA ===== */
}

/* CORRECAO 1: o mutex garante que so uma thread por vez executa a secao
 * critica. A outra bloqueia no lock ate o unlock. */
static void inc_mutex(void)
{
	k_mutex_lock(&mtx_contador, K_FOREVER);
	/* ===== SECAO CRITICA protegida ===== */
	uint32_t tmp = contador;
	processa_leitura();
	contador = tmp + 1U;
	/* ===== FIM DA SECAO CRITICA ===== */
	k_mutex_unlock(&mtx_contador);
}

/* CORRECAO 2: atomic_inc faz ler-somar-escrever como UMA operacao
 * indivisivel. Basta quando a secao critica e so o incremento. O processamento
 * de validacao fica FORA do atomic_inc: operacao atomica nao protege um bloco
 * de varias instrucoes (para isso, use mutex).
 * No Cortex-M0+ nao ha instrucoes exclusivas (LDREX/STREX), entao o Zephyr
 * implementa o atomic desabilitando interrupcoes por alguns ciclos. */
static void inc_atomic(void)
{
	processa_leitura();
	atomic_inc(&contador_atomico);
}

static void (*const incremento[MODO_TOTAL])(void) = {
	inc_sem_protecao, inc_mutex, inc_atomic
};

/* ------------------------------------------------------------------ */
/* Threads dos sensores                                               */
/* ------------------------------------------------------------------ */
K_THREAD_STACK_DEFINE(stack_sensor_a, STACK_SIZE);
K_THREAD_STACK_DEFINE(stack_sensor_b, STACK_SIZE);
static struct k_thread thr_sensor_a;
static struct k_thread thr_sensor_b;

/* p1 = modo de execucao (indice na tabela "incremento") */
static void sensor_thread(void *p1, void *p2, void *p3)
{
	const enum modo modo = (enum modo)(uintptr_t)p1;
	void (*inc)(void) = incremento[modo];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (uint32_t i = 0U; i < N_CONTAGENS; i++) {
		inc();   /* "uma peca passou pelo sensor" */
	}
}

/* ------------------------------------------------------------------ */
/* Rodadas e relatorio                                                */
/* ------------------------------------------------------------------ */
static uint32_t executa_rodada(enum modo modo, uint32_t *tempo_ms)
{
	/* zera o contador a cada rodada */
	contador = 0U;
	atomic_set(&contador_atomico, 0);

	uint32_t c_ini = k_cycle_get_32();

	k_thread_create(&thr_sensor_a, stack_sensor_a,
			K_THREAD_STACK_SIZEOF(stack_sensor_a), sensor_thread,
			(void *)(uintptr_t)modo, NULL, NULL,
			PRIO_SENSOR, 0, K_NO_WAIT);
	k_thread_create(&thr_sensor_b, stack_sensor_b,
			K_THREAD_STACK_SIZEOF(stack_sensor_b), sensor_thread,
			(void *)(uintptr_t)modo, NULL, NULL,
			PRIO_SENSOR, 0, K_NO_WAIT);

	/* main espera as duas threads terminarem */
	k_thread_join(&thr_sensor_a, K_FOREVER);
	k_thread_join(&thr_sensor_b, K_FOREVER);

	/* 32 bits a 48 MHz estouram em ~89 s: cada rodada precisa ser menor */
	*tempo_ms = k_cyc_to_ms_near32(k_cycle_get_32() - c_ini);

	return (modo == MODO_ATOMIC) ? (uint32_t)atomic_get(&contador_atomico)
				     : contador;
}

static void executa_modo(enum modo modo)
{
	const uint32_t esperado = 2U * N_CONTAGENS;
	uint32_t rodadas_com_perda = 0U;
	uint32_t perdidas_total = 0U;
	uint32_t tempo_total_ms = 0U;

	LOG_INF("=== Modo: %s | %u rodadas ===", nome_modo[modo], N_RODADAS);

	for (uint32_t r = 1U; r <= N_RODADAS; r++) {
		uint32_t tempo_ms = 0U;
		uint32_t obtido = executa_rodada(modo, &tempo_ms);
		uint32_t perdidas = (obtido <= esperado) ? (esperado - obtido) : 0U;
		/* erro em centesimos de percentual, sem usar float */
		uint32_t erro100 =
			(uint32_t)(((uint64_t)perdidas * 10000U) / esperado);

		if (perdidas > 0U) {
			rodadas_com_perda++;
			LOG_WRN("[%s] rodada %u: esperado=%u obtido=%u perdidas=%u erro=%u.%02u%% tempo=%u ms",
				nome_modo[modo], r, esperado, obtido, perdidas,
				erro100 / 100U, erro100 % 100U, tempo_ms);
		} else {
			LOG_INF("[%s] rodada %u: esperado=%u obtido=%u perdidas=0 erro=0.00%% tempo=%u ms",
				nome_modo[modo], r, esperado, obtido, tempo_ms);
		}

		perdidas_total += perdidas;
		tempo_total_ms += tempo_ms;
		k_msleep(100);   /* deixa o Logging esvaziar o buffer */
	}

	LOG_INF("[%s] RESUMO: %u de %u rodadas com perda | perdidas (soma)=%u | tempo medio=%u ms",
		nome_modo[modo], rodadas_com_perda, N_RODADAS, perdidas_total,
		tempo_total_ms / N_RODADAS);
}

int main(void)
{
	/* tempo para voce abrir o terminal serial antes de comecar */
	k_msleep(ATRASO_INICIAL_MS);

	LOG_INF("Esteira: 2 sensores (threads, prio %d), %u contagens cada",
		PRIO_SENSOR, N_CONTAGENS);
	LOG_INF("Esperado por rodada = %u pecas | time slice = %d ms",
		2U * N_CONTAGENS, CONFIG_TIMESLICE_SIZE);

#if EXECUTAR_TODOS_OS_MODOS
	for (int m = 0; m < MODO_TOTAL; m++) {
		executa_modo((enum modo)m);
	}
#else
	executa_modo(MODO_UNICO);
#endif

	LOG_INF("Fim. Aperte RESET para repetir.");
	return 0;
}