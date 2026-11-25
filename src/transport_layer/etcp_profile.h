/* Точки CPU-профиля UDP/ETCP для bench_etcp_packet_cpu. В обычной сборке полностью пустые.
 * ETCP_PACKET_PROFILE включает однопоточный измеритель стенда; реализации находятся в benchmark.
 * SCOPE восстанавливает предыдущую фазу при любом выходе (GCC/Clang cleanup), STAGE меняет текущую.
 * Вложенные фазы учитываются исключительно: шифрование и syscall не входят повторно в стек.
 * Ожидание между callbacks не относится к CPU; MARK отдельно измеряет wall time normalizer. */
#ifndef ETCP_PROFILE_H
#define ETCP_PROFILE_H

#ifdef ETCP_PACKET_PROFILE
enum etcp_profile_stage {
    ETCP_PROF_IDLE, ETCP_PROF_TX_STACK, ETCP_PROF_ENCRYPT, ETCP_PROF_TX_POST,
    ETCP_PROF_SOCKET, ETCP_PROF_TX_COMPLETE, ETCP_PROF_RX_INGRESS, ETCP_PROF_DECRYPT, ETCP_PROF_INIT,
    ETCP_PROF_RX_STACK, ETCP_PROF_APP,
    ETCP_PROF_INIT_CONTEXT, ETCP_PROF_INIT_OBFUSCATE, ETCP_PROF_INIT_VALIDATE,
    ETCP_PROF_INIT_PRIVATE_KEY, ETCP_PROF_INIT_PUBLIC_KEY, ETCP_PROF_INIT_DERIVE_CONTEXT,
    ETCP_PROF_INIT_DERIVE_SETUP, ETCP_PROF_INIT_SET_PEER, ETCP_PROF_INIT_X25519,
    ETCP_PROF_INIT_KDF, ETCP_PROF_INIT_FREE, ETCP_PROF_INIT_AES, ETCP_PROF_INIT_CRC, ETCP_PROF_STAGES
};
enum etcp_profile_event { ETCP_PROF_SUBMIT, ETCP_PROF_NORMALIZED, ETCP_PROF_ENCRYPT_BEGIN, ETCP_PROF_INIT_BEGIN };

struct etcp_profile_scope {
    enum etcp_profile_stage previous; /* Фаза вызывающего кода; восстанавливается на выходе. */
};

/* Войти в фазу stage; вернуть прежнюю фазу для cleanup. Только поток benchmark. */
struct etcp_profile_scope etcp_profile_enter(enum etcp_profile_stage stage);
/* Восстановить фазу из scope; аргумент всегда указывает на живой локальный scope. */
void etcp_profile_leave(struct etcp_profile_scope* scope);
/* Переключить исключительный учёт CPU на stage; при выключенном измерителе ничего не делать. */
void etcp_profile_set(enum etcp_profile_stage stage);
/* Войти/переключить подэтап только внутри RX INIT; остальные вызовы secure_channel не меняют фазу. */
struct etcp_profile_scope etcp_profile_enter_init(enum etcp_profile_stage stage);
void etcp_profile_set_init(enum etcp_profile_stage stage);
/* Отметить событие event объекта owner для отдельного измерения задержки. */
void etcp_profile_mark(enum etcp_profile_event event, const void* owner);

#define ETCP_PROFILE_SCOPE(stage) \
    struct etcp_profile_scope etcp_profile_scope __attribute__((cleanup(etcp_profile_leave))) = etcp_profile_enter(stage)
#define ETCP_PROFILE_STAGE(stage) etcp_profile_set(stage)
#define ETCP_PROFILE_MARK(event, owner) etcp_profile_mark(event, owner)
#define ETCP_PROFILE_INIT_SCOPE(stage) \
    struct etcp_profile_scope etcp_profile_scope __attribute__((cleanup(etcp_profile_leave))) = etcp_profile_enter_init(stage)
#define ETCP_PROFILE_INIT_STAGE(stage) etcp_profile_set_init(stage)
#else
#define ETCP_PROFILE_SCOPE(stage) ((void)0)
#define ETCP_PROFILE_STAGE(stage) ((void)0)
#define ETCP_PROFILE_MARK(event, owner) ((void)0)
#define ETCP_PROFILE_INIT_SCOPE(stage) ((void)0)
#define ETCP_PROFILE_INIT_STAGE(stage) ((void)0)
#endif

#endif
