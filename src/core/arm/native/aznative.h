#pragma once

/*
 * Ejecucion nativa del codigo del 3DS en la Vita (diseno propio).
 *
 * El codigo del juego corre directamente en el Cortex-A9, en modo usuario,
 * dentro del proceso del emulador. El modulo de kernel solo hace de puerta:
 * guarda lo de Sony, pone nuestros vectores de excepcion y salta al invitado.
 * El invitado vuelve por cualquier excepcion suya (SVC, instruccion indefinida,
 * fallo de memoria) o por una interrupcion de la Vita, que se le devuelve a
 * Sony y despues se reanuda el invitado donde estaba.
 */

#include <stdint.h>

/* El emulador es C++ y el modulo exporta simbolos C: sin esto, el enlazador
 * busca nombres decorados y no encuentra ninguno. El spike es C y no se
 * enteraba, asi que esto salio al enchufar Azahar. */
#ifdef __cplusplus
extern "C" {
#endif

#define AZ_NATIVE_VERSION 0x00130000u

/* Por que volvio el invitado (el vector que salto, mas AZ_REASON_BUDGET). */
enum {
    AZ_REASON_RESET = 0,
    AZ_REASON_UNDEF = 1,
    AZ_REASON_SVC = 2,
    AZ_REASON_PABT = 3,
    AZ_REASON_DABT = 4,
    AZ_REASON_UNUSED = 5,
    AZ_REASON_IRQ = 6,
    AZ_REASON_FIQ = 7,
    /* Se acabo el tiempo pedido: pc es donde hay que seguir. */
    AZ_REASON_BUDGET = 8,
    /* La llamada no entro en modo System y az_enter no intento nada: ni un
     * registro del sistema tocado. Ver la cabecera de vectors.S. */
    AZ_REASON_MODE = 9,
};

/* Opciones de AzNativeRun. */
enum {
    /* El invitado corre con las interrupciones de la Vita ACTIVAS: cada una le
     * saca, Sony la atiende y se vuelve a entrar. Sin esto el invitado corre
     * con ellas cortadas (solo vale para tramos de microsegundos). */
    AZ_RUN_IRQ = 1u << 0,
    /* El invitado corre con NUESTRA tabla TTBR0 (ver AzNativeWorldCreate). Sin
     * esto corre con la de Sony, como en la fase 1. */
    AZ_RUN_WORLD = 1u << 1,
    /* Transferir el contexto de coma flotante (ver AzGuestCtx.vfp). */
    AZ_RUN_VFP = 1u << 2,
};

/* vectors.S usa estos desplazamientos a mano: no reordenar. */
typedef struct AzGuestCtx {
    uint32_t r[16];  /* 0-60: r0-r12, sp, lr, pc */
    uint32_t cpsr;   /* 64: entrada: flags del invitado; salida: SPSR de la excepcion */
    uint32_t reason; /* 68 */
    uint32_t dfsr;   /* 72 */
    uint32_t dfar;   /* 76 */
    uint32_t ifsr;   /* 80 */
    uint32_t ifar;   /* 84 */
    uint32_t tls;    /* 88: TPIDRURO que ve el invitado (c13, c0, 3) */
    uint32_t irq;    /* 92: 1 = interrupciones activas en el invitado */
    /* 96: la tabla TTBR0 con la que corre el invitado, 0 = la de Sony. Va aqui
     * y no en una global para que dos hilos nativos a la vez, uno con mundo y
     * otro sin el, no se pisen. Lo rellena AzNativeRun segun AZ_RUN_WORLD. */
    uint32_t ttbr0;
    /**
     * 100: 1 = se transfiere el contexto de COMA FLOTANTE.
     *
     * El invitado y el emulador comparten los registros VFP del chip, asi que
     * sin esto se pisan mutuamente EN SILENCIO en cuanto el juego usa un float
     * -- y los juegos del 3DS los usan constantemente. az_enter guarda los 32
     * dobles y el FPSCR del emulador, pone los del invitado, y az_exit hace lo
     * contrario.
     *
     * Cuesta 260 bytes de ida y 260 de vuelta por salida del mundo, asi que es
     * opcional: un hilo que se sepa que no toca coma flotante puede ahorrarselo.
     */
    uint32_t vfp;
    uint32_t fpscr;  /* 104 */
    uint32_t _pad;   /* 108: los dobles necesitan alineacion de 8 */
    uint64_t d[32];  /* 112..367 */
} AzGuestCtx;

#ifdef __cplusplus
static_assert(sizeof(AzGuestCtx) == 368, "vectors.S depende de este tamano");
#else
_Static_assert(sizeof(AzGuestCtx) == 368, "vectors.S depende de este tamano");
#endif

typedef struct AzCpuRegs {
    uint32_t version;
    uint32_t midr;
    uint32_t mpidr;
    uint32_t sctlr;
    uint32_t vbar;
    uint32_t dacr;
    uint32_t ttbcr;
    uint32_t ttbr0;
    uint32_t ttbr1;
    uint32_t cpacr;
    uint32_t contextidr;
    uint32_t tpidruro;
    uint32_t tpidrurw;
    uint32_t cpsr;
    uint32_t cpu_id;
} AzCpuRegs;

/* Lo que devuelve una llamada a AzNativeRun ademas del contexto. */
typedef struct AzRunInfo {
    uint32_t entries;   /* veces que se entro al invitado */
    uint32_t irq_exits; /* veces que le saco una interrupcion */
    uint32_t us;        /* tiempo total dentro de la llamada */
    uint32_t cpu_first; /* nucleo de la primera entrada y de la ultima */
    uint32_t cpu_last;
} AzRunInfo;

#define AZ_MAX_ENTRIES 1000000u

/* Resultado de UNA tecnica de mapeo (ver AzNativeTryMap). */
typedef struct AzMapInfo {
    uint32_t tecnica;
    int32_t rc;      /* lo que devolvio ksceKernelAllocMemBlock */
    int32_t blockid; /* >= 0 si quedo un bloque reservado */
    uint32_t base;   /* direccion virtual que dio */
    int32_t rc_base; /* rc de GetMemBlockBase */
    uint32_t paddr;  /* direccion fisica de esa base */
    int32_t rc_pa;   /* rc de MMUVAtoPA */
    uint32_t pid;
    uint32_t ttbr0;
} AzMapInfo;

/* FASE 1. Intenta reservar size bytes EN LA DIRECCION VIRTUAL va dentro del
 * proceso que llama, que es lo que necesita el invitado: el 3DS pone su heap
 * en 0x08000000 y su memoria lineal en 0x14000000, y el codigo del juego usa
 * esas direcciones tal cual.
 *
 * El SDK no documenta ninguna forma de pedir una direccion virtual concreta:
 * el struct de opciones tiene un hueco sin nombre en el desplazamiento 0x0C y
 * el bit 0x1 de attr no esta documentado, asi que son el candidato. Se prueba
 * UNA tecnica POR LLAMADA a proposito: asi el log tiene el resultado de cada
 * una antes de probar la siguiente, y si una mata la consola sabemos cual.
 *   0 = control, sin opciones (para ver donde cae por defecto)
 *   1 = attr 0x1 + el hueco de 0x0C = va
 *   2 = lo mismo diciendo tambien el pid
 *   3 = control con pid
 * Si acierta, DEJA el bloque reservado; se suelta con AzNativeUnmap. */
int AzNativeTryMap(uint32_t tecnica, uint32_t va, uint32_t size, AzMapInfo *out);
int AzNativeUnmap(int32_t blockid);

#define AZ_TABLA_VAS 8

/* Lo que se lee de las tablas de paginas para unas VA dadas. */
typedef struct AzTablaInfo {
    uint32_t ttbr0, ttbr1, ttbcr;
    uint32_t l1_pa;    /* base FISICA de la tabla de primer nivel de TTBR0 */
    int32_t rc_vent;   /* rc al mapear esa pagina fisica (o el uid) */
    uint32_t vent;     /* VA donde quedo la ventana */
    uint32_t vent_tipo;/* tipo de bloque que acepto el mapeo */
    uint32_t l1_usadas;/* cuantas de las 1024 entradas no son cero */
    uint32_t count;
    uint32_t va[AZ_TABLA_VAS];
    uint32_t l1[AZ_TABLA_VAS];    /* descriptor de primer nivel */
    uint32_t l2[AZ_TABLA_VAS];    /* descriptor de segundo nivel, si lo hay */
    int32_t rc_l2[AZ_TABLA_VAS];  /* 0 = leido, -1 = el L1 no apunta a tabla */
    uint32_t prueba_va;           /* una reserva normal... */
    uint32_t prueba_pa;           /* ...y su direccion fisica */
    int32_t rc_prueba;
} AzTablaInfo;

/* FASE 1b. Lee (SOLO LEE) los descriptores de las tablas de paginas para las
 * VA pedidas. La tabla de TTBR0 esta en una direccion fisica fija, asi que se
 * mapea esa pagina en el kernel con HAS_PADDR, que si esta documentado.
 *
 * Importa saber que el contexto de proceso de la Vita es {TTBR1, DACR,
 * CONTEXTIDR}: la tabla POR PROCESO es la de TTBR1, y la de TTBR0 -- el primer
 * giga, donde el 3DS tiene todas sus direcciones -- es GLOBAL, compartida por
 * todos los procesos. Escribir ahi afecta a todo el sistema, de modo que antes
 * de escribir nada hay que saber exactamente lo que hay. */
int AzNativeReadTables(const uint32_t *vas, uint32_t count, AzTablaInfo *out);

/* Lee hasta 64 palabras de una direccion FISICA. Solo lectura; sirve para
 * identificar que hay en las paginas que ocupa Sony. */
#define AZ_PHYS_PALABRAS 64
int AzNativeReadPhys(uint32_t pa, uint32_t palabras, uint32_t *out);

/* Hasta donde llego AzNativeMapSectionPrepare/Commit. */
enum {
    AZ_SEC_NADA = 0,
    AZ_SEC_RESERVADO = 1,  /* 1 MB fisicamente contiguo reservado */
    AZ_SEC_FISICA = 2,     /* se sabe su direccion fisica */
    AZ_SEC_ALINEADA = 3,   /* y esta alineada a 1 MB */
    AZ_SEC_VENTANA = 4,    /* la tabla L1 esta mapeada para escribirla */
    AZ_SEC_COMPROBADO = 5, /* la entrada estaba LIBRE: se puede escribir */
    AZ_SEC_ESCRITO = 6,
    AZ_SEC_TLB = 7,        /* y la TLB invalidada en todos los nucleos */
};

typedef struct AzSectionInfo {
    int32_t paso; /* uno de los AZ_SEC_*: dice exactamente donde se quedo */
    int32_t rc;
    uint32_t va;
    uint32_t idx;
    uint32_t pa;
    uint32_t desc_viejo;
    uint32_t desc_nuevo;
    int32_t blockid;
} AzSectionInfo;

/* FASE 1c. Mapear 1 MB de memoria de verdad EN la direccion del 3DS,
 * escribiendo el descriptor de seccion a mano. Va en DOS llamadas a proposito:
 *
 *   Prepare: reserva el megabyte fisicamente contiguo y alineado, averigua su
 *     direccion fisica, abre la ventana a la tabla L1 y COMPRUEBA que la
 *     entrada esta libre. Si no lo esta, aborta sin escribir nada. No toca
 *     ninguna tabla.
 *   Commit: escribe el descriptor e invalida la TLB. Es lo unico peligroso de
 *     todo el proyecto, y asi la app puede dejar escrito en el log el
 *     resultado del Prepare ANTES de intentarlo: si la consola se muere en el
 *     Commit, sabremos que fue exactamente eso.
 *
 * Ojo: la tabla de TTBR0 es global. Mientras la seccion este puesta, esa
 * direccion existe para TODO el sistema, y si la app muere sin llamar a
 * Unmap se queda puesta hasta que reinicies. */
int AzNativeMapSectionPrepare(uint32_t va, AzSectionInfo *out);
int AzNativeMapSectionCommit(AzSectionInfo *out);
int AzNativeMapSectionUnmap(AzSectionInfo *out);

/*
 * FASE 2. EL MUNDO DEL INVITADO: UNA TABLA TTBR0 NUESTRA.
 *
 * Fase 1 dejo mapear memoria en las direcciones del 3DS escribiendo a mano los
 * descriptores de la tabla L1 de Sony. Funciona, pero tiene dos problemas: la
 * tabla es del sistema entero (lo que pongamos lo ve todo el mundo, y si la app
 * muere se queda puesto hasta reiniciar) y, sobre todo, 0x00100000 -- donde el
 * 3DS pone el codigo del juego -- NO esta libre: hay 128 KB de objetos vivos
 * del kernel de la Vita en 0x0011E000, con referencias desde fuera. No se
 * pueden ni mover ni pisar.
 *
 * La solucion es no tocar la tabla de Sony. TTBR0 es un registro de CP15 POR
 * NUCLEO: se puede cambiar entero. Hacemos una tabla L1 nuestra (copia de la de
 * Sony para que el primer giga siga teniendo lo que Sony necesita) y encima le
 * escribimos las secciones del invitado, pisando lo que haga falta. az_enter la
 * instala justo antes de saltar al invitado y az_exit devuelve la de Sony antes
 * de tocar nada mas. Fuera de esos microsegundos el sistema no se entera de
 * nada: los objetos del kernel en 0x0011E000 siguen exactamente donde estaban,
 * en la tabla de Sony, y reaparecen en cuanto volvemos.
 *
 * Con esto el invitado tiene el primer giga entero para el.
 */

/* Hasta donde llego la creacion del mundo. */
enum {
    AZ_MUNDO_NADA = 0,
    AZ_MUNDO_RESERVADO = 1, /* la tabla L1 nuestra, 4 KB contiguos */
    AZ_MUNDO_FISICA = 2,    /* se sabe su direccion fisica y esta alineada */
    AZ_MUNDO_COPIADA = 3,   /* con la tabla de Sony dentro */
    AZ_MUNDO_LISTO = 4,     /* az_world_ttbr0 publicado: ya se puede usar */
};

/* Cuantas regiones de 1 MB puede reclamar el invitado. */
#define AZ_MUNDO_REGIONES 16

typedef struct AzWorldInfo {
    int32_t paso;
    int32_t rc;
    uint32_t ttbr0_sony; /* el que tiene Sony ahora mismo */
    uint32_t ttbr0_az;   /* el nuestro, el que instala az_enter */
    uint32_t l1_pa;      /* fisica de nuestra tabla */
    uint32_t l1_va;      /* donde la vemos desde el kernel */
    uint32_t regiones;   /* cuantas hay mapeadas */
    uint32_t copiadas;   /* entradas no vacias copiadas de la de Sony */
    /* De la ultima region mapeada. */
    uint32_t ult_va;
    uint32_t ult_pa;
    uint32_t ult_mb;
    uint32_t ult_kva;       /* con que direccion lo ve el kernel */
    uint32_t ult_desc_sony; /* lo que Sony tenia ahi: 0x1e0 = libre */
    uint32_t ult_desc_az;
} AzWorldInfo;

/*
 * LA COMPROBACION QUE DECIDE SI ESTO ES VIABLE (fase 2b).
 *
 * El emulador no puede hablar con la memoria del invitado a traves de llamadas
 * al sistema: la GPU, el DSP y los servicios la leen y la escriben todo el
 * rato, y una llamada por acceso seria absurdo. Necesita verla DIRECTAMENTE,
 * con un puntero normal de su proceso.
 *
 * O sea que las mismas paginas fisicas tienen que estar mapeadas dos veces: en
 * la direccion del 3DS (para el invitado, via nuestra tabla) y en el espacio
 * de usuario del emulador (para el codigo C++). Si la Vita no lo permite hay
 * que replantear el reparto de memoria entero, asi que conviene saberlo ANTES
 * de escribir la cirugia del emulador y no despues.
 *
 * Se reserva un bloque de usuario con HAS_PADDR apuntando a la fisica que ya
 * tiene la region, y con HAS_PID al proceso que llama. Devuelve la direccion
 * con la que el emulador la veria.
 */
typedef struct AzShareInfo {
    int32_t rc;
    uint32_t va;        /* la del 3DS que se pidio compartir */
    uint32_t pa;        /* su fisica */
    uint32_t user_va;   /* con que direccion la ve el proceso de usuario */
    uint32_t mb;
    int32_t blockid;
    uint32_t pid;
} AzShareInfo;

int AzNativeWorldShare(uint32_t va, AzShareInfo *out);
int AzNativeWorldUnshare(AzShareInfo *out);

/* Crea la tabla y la deja lista (no la instala: eso lo hace az_enter). */
int AzNativeWorldCreate(AzWorldInfo *out);
/*
 * Reclama 'mb' megabytes a partir de 'va' para el invitado, EN TRES LLAMADAS.
 * La version de una sola colgo la consola en seco al pedir 0x00100000 y sin
 * volcado no habia forma de saber en cual de los cuatro pasos. Ahora cada paso
 * deja su linea en el log antes del siguiente:
 *
 *   Prepare: reserva el megabyte contiguo y alineado y averigua su fisica.
 *            No toca ninguna tabla.
 *   Clear:   lo pone a ceros (un megabyte: iba dentro y era sospechoso).
 *   Commit:  escribe las secciones EN NUESTRA tabla, pisando lo que Sony
 *            tuviera ahi, que es justamente de lo que se trata.
 */
int AzNativeWorldMapPrepare(uint32_t va, uint32_t mb, AzWorldInfo *out);
int AzNativeWorldMapClear(AzWorldInfo *out);
int AzNativeWorldMapCommit(AzWorldInfo *out);
/*
 * Apunta 'mb' megabytes desde 'va_alias' a la misma memoria fisica que 'va':
 * asi se hace el alias del heap lineal del 3DS (0x14000000 sobre 0x08000000).
 */
int AzNativeWorldAlias(uint32_t va, uint32_t va_alias, uint32_t mb, AzWorldInfo *out);
/*
 * Escribe en la memoria del invitado desde el espacio de usuario. Hace falta
 * porque esa memoria solo esta mapeada en la direccion del 3DS, y esa
 * direccion solo existe mientras corre el invitado. Deja la cache de datos
 * volcada y la de instrucciones invalidada: lo escrito se puede EJECUTAR.
 */
int AzNativeWorldWrite(uint32_t va, const void *src, uint32_t len);
/* Y al reves, para comprobar lo que el invitado dejo escrito. */
int AzNativeWorldRead(uint32_t va, void *dst, uint32_t len);
/* Suelta todo: deja de publicar la tabla y libera la memoria. */
int AzNativeWorldDestroy(AzWorldInfo *out);

/*
 * EL ID DEL MODULO, PARA PODER DESCARGARLO (fase 2b).
 *
 * Un modulo de kernel se queda cargado hasta que la consola se apaga de
 * verdad, asi que para probar una version nueva hay que poder quitar la vieja.
 * El id que devuelve taiLoadStartKernelModule NO sirve para eso: la app se
 * relanza despues de cargar (el cargador resuelve las importaciones al
 * arrancar el ejecutable, no despues) y ese id no vale en el proceso nuevo --
 * medido el 24-10, taiStopUnloadKernelModule devolvio SCE_KERNEL_ERROR_
 * INVALID_UID (0x80024501) con el id guardado en la tarjeta.
 *
 * Asi que lo pregunta el propio modulo, que si puede: ksceKernelSearchModuleByName
 * se lo da desde dentro del kernel y el valor es bueno en cualquier proceso.
 */
/*
 * LIMPIAR LA CACHE DE DATOS DE UN TRAMO DEL PROCESO DE USUARIO (fase 3).
 *
 * Esto no es para el invitado, es para el EMULADOR. Ahora mismo cada lote
 * copia ~11 KB de vertices a memoria sin cache porque la GPU no ve la cache de
 * la CPU: 38 us por lote, 4,4 ms por fotograma, y ya esta comprobado que ni
 * NEON ni prefetch la mueven porque el muro esta en la escritura.
 *
 * Si la GPU pudiera leer la memoria del invitado TAL CUAL, no habria copia
 * ninguna. Lo que lo impide es que desde usuario no se puede limpiar la cache
 * (SCTLR.UCI = 0) y sceKernelSyncVMDomain cuesta 51 us por 4 KB, mas que la
 * copia entera. Pero un modulo de kernel si puede limpiarla, y el nuestro ya
 * esta ahi.
 *
 * La pregunta es cuanto cuesta la llamada. Si limpiar 11 KB sale por unos
 * pocos microsegundos, se puede quitar la copia del todo. Si sale por 30,
 * no. Se mide antes de disenar nada.
 */
int AzNativeCleanRange(void *ptr, uint32_t bytes);
/*
 * Lo mismo pero por la vista del KERNEL de una region del mundo, que es como
 * lo haria el diseno de verdad: la memoria del 3DS la reservaria el modulo
 * (asi tiene su propia direccion de kernel) y se compartiria con el emulador.
 * Asi no hay que preguntarse si el kernel puede tocar una direccion de usuario.
 */
int AzNativeCleanGuest(uint32_t va, uint32_t bytes);
/*
 * Igual pero SOLO la L1 (ksceKernelDcacheCleanRangeForL1WBWA). Medido el
 * 24-10, limpiar L1+L2 cuesta ~220 ns por linea de 32 bytes: 78 us por 11 KB,
 * el doble que copiarlos (38 us). Eso mata la idea de que la GPU lea la memoria
 * del 3DS... salvo que baste con la L1, que es lo que esto mide. Si la GPU de
 * la Vita lee a traves de la L2, limpiar solo la L1 deberia ser varias veces
 * mas barato y la idea revive; si tampoco, queda cerrada con datos.
 */
int AzNativeCleanGuestL1(uint32_t va, uint32_t bytes);

/*
 * TIRAR LA CACHE DE INSTRUCCIONES DEL CORTEX-A9.
 *
 * Hace falta cada vez que el EMULADOR cambia codigo del invitado: cargar un
 * modulo CRO, aplicar un parche, cualquier cosa. El emulador escribe esas
 * instrucciones por su propia direccion y la cache de instrucciones del chip
 * puede seguir teniendo las lineas viejas de la direccion del 3DS, de modo que
 * el invitado ejecutaria codigo caducado -- y eso acaba escribiendo ordenes de
 * GPU invalidas, que es como se cuelga el driver grafico.
 *
 * Se invalida ENTERA y en todos los nucleos, no por direccion: nosotros
 * escribimos por una direccion y el invitado ejecuta por otra, asi que
 * invalidar por direccion no serviria. Es una operacion barata (ICIALLUIS) y
 * ocurre pocas veces.
 */
int AzNativeIcacheAll(void);

/*
 * FASE 3. MAPEAR LA MEMORIA QUE EL EMULADOR YA TIENE EN LA DIRECCION DEL 3DS.
 *
 * La fase 2 mapeaba secciones de 1 MB y para eso hace falta memoria
 * fisicamente contigua. FCRAM son 128 MB (256 en la New 3DS): imposible en una
 * Vita. Y tampoco es lo que se quiere. Lo que se quiere es que el invitado vea
 * en la direccion del 3DS la memoria que el emulador YA tiene, esten donde
 * esten sus paginas fisicas, y eso son tablas de segundo nivel: 256 entradas
 * de 4 KB por megabyte.
 *
 * Si esto funciona, el emulador no tiene que cambiar como reserva su memoria
 * -- que era el cambio mas invasivo de todos -- y tampoco hay que compartir
 * nada despues, porque la memoria ya es suya.
 */
#define AZ_USERMAP_MUESTRAS 8

typedef struct AzUserMapInfo {
    int32_t rc;
    uint32_t va;       /* la del 3DS */
    uint32_t user_va;  /* la del emulador */
    uint32_t bytes;
    uint32_t paginas;
    uint32_t l2_pa;    /* fisica de la tabla de segundo nivel */
    uint32_t l2_kva;
    uint32_t desc_sony; /* lo que habia en la entrada L1 */
    uint32_t desc_az;
    uint32_t fallo_pagina;
    /* Las fisicas de las primeras paginas. Si NO son consecutivas, queda
     * demostrado que esto hace algo que las secciones no podian hacer. */
    uint32_t pa_muestra[AZ_USERMAP_MUESTRAS];
} AzUserMapInfo;

int AzNativeMapUser(uint32_t va, void *user_ptr, uint32_t bytes, AzUserMapInfo *out);

/*
 * FASE 3b. REFLEJAR LA TABLA DE PAGINAS DEL EMULADOR ENTERA.
 *
 * El emulador ya mantiene un array de 2^20 punteros, uno por pagina de 4 KB
 * del 3DS: `PageTable::pointers.raw[pagina]` dice donde vive esa pagina en su
 * memoria, o NULL. Esta llamada lo traduce de golpe a entradas de MMU, para
 * que el invitado vea el espacio de direcciones del 3DS tal como lo ve el
 * emulador, sin que el emulador cambie nada de como reserva su memoria.
 *
 * Una sola llamada para toda la tabla: a 1,05 us el syscall, una por pagina
 * serian mas de mil segundos.
 */
#define AZ_TABLA_MUESTRAS 6

typedef struct AzMapTableInfo {
    int32_t rc;
    uint32_t primera;     /* primera pagina pedida */
    uint32_t pedidas;
    uint32_t mapeadas;    /* punteros no nulos que se pudieron traducir */
    uint32_t sin_fisica;  /* no nulos cuya fisica no se pudo averiguar */
    /* Punteros cuya fisica NO esta alineada a 4 KB: la MMU no puede mapear eso,
     * asi que se cuentan y se dejan sin mapear en vez de enmascarar los bits
     * bajos, que mapearia la pagina DESPLAZADA. Paso de verdad: el emulador
     * reserva FCRAM con make_unique<u8[]>, que alinea a 8 bytes, y la primera
     * medicion dio 00100000 -> 4d063e68. */
    uint32_t sin_alinear;
    uint32_t primera_sin_alinear;
    uint32_t megas;       /* entradas L1 nuevas que hubo que poner */
    uint32_t limpiadas;   /* tablas L2 que hubo que sacar de la cache */
    uint32_t borradas;    /* entradas quitadas porque el emulador las desmapeo */
    uint32_t l2pool_pa;
    uint32_t us;          /* lo que costo, medido dentro del kernel */
    uint32_t fallo_pagina;
    uint32_t muestra_va[AZ_TABLA_MUESTRAS];
    uint32_t muestra_pa[AZ_TABLA_MUESTRAS];
} AzMapTableInfo;

int AzNativeMapTable(const void *raw, uint32_t primera, uint32_t paginas, AzMapTableInfo *out);
int AzNativeUnmapTable(AzMapTableInfo *out);
int AzNativeUnmapUser(AzUserMapInfo *out);

int AzNativeModuleId(void);

int AzNativeVersion(void);
int AzNativeReadRegs(AzCpuRegs *out);
/* Corre el invitado hasta una excepcion suya (que no sea una interrupcion) o
 * hasta budget_us microseconds (0 = sin limite de tiempo, solo con AZ_RUN_IRQ
 * tiene sentido limitarlo). */
int AzNativeRun(AzGuestCtx *ctx, uint32_t flags, uint32_t budget_us, AzRunInfo *info);

#ifdef __cplusplus
}
#endif
