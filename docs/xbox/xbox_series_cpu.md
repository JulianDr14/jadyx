# Xbox: como exprimir la CPU de la Series

Investigacion sobre la CPU de la Series X|S para el emulador: que hardware tenemos, que nos da
el sistema, en que se va hoy el tiempo de CPU (perfil de 0.3.8.70) y una lista priorizada de
cambios, cada uno con su forma de medirlo en consola. Hermana de `xbox_series_gpu.md`.

## La maquina

- 8 nucleos Zen 2 a 3,8 GHz (3,66 GHz con SMT), 2 hilos por nucleo con SMT.
- Dos CCX de 4 nucleos, cada uno con 4 MB de L3 compartida, como el APU "Renoir" de PC.
- Latencias de nucleo a nucleo medidas en Renoir, la referencia mas cercana:
  - entre los dos hilos SMT de un nucleo: ~7 ns;
  - dentro de un CCX: ~17 ns;
  - entre CCX: ~61-69 ns.
- En Zen 2, PDEP/PEXT son microcodigo y muy lentos. dynarmic ya lo evita (FastBMI2 apagado en
  AMD anterior a Zen 3).
- La Series S tiene la misma CPU a 3,6 / 3,4 GHz.

## Lo que el sistema le da a la app (Dev Mode, UWP en modo Game)

La linea `step: CPU` del diag, agregada en 0.3.8.70, lo midio:

- El sistema ve **8 procesadores logicos en 4 nucleos fisicos**, no 16 en 8.
- A la app le tocan **6**: ids de CPU set 258-263, que son lp2-lp7 en los nucleos fisicos c2, c4
  y c6. El nucleo 0 (lp0/lp1) es del sistema.
- Ninguno esta "parked" y todos tienen la marca de tiempo real.
- Microsoft describe esto como "4 exclusivos + 2 compartidos" para juegos. No sabemos cuales 2
  son los compartidos.

**Consecuencia clave:** tenemos 3 nucleos fisicos, no 6. Cada pareja SMT (lp2/lp3, lp4/lp5,
lp6/lp7) comparte decodificador, ALUs, L1 y L2. Dos hilos pesados en la misma pareja rinden
juntos, segun la carga, entre 1,1 y 1,4 veces lo que rinde uno solo, no el doble.

## A donde se va hoy la CPU (perfil de 0.3.8.70, en juego)

| Hilo | % de un nucleo | En que |
| --- | --- | --- |
| CPUCore_0 | 91% | ver tabla de abajo |
| CPUCore_1 | 91% | ver tabla de abajo |
| CPUCore_2 | 89% | ver tabla de abajo |
| GPU | 46% | grabar draws, cache de texturas y buffers |
| D3D12Recorder | 6% | |
| GPUFencingThread | 4% | |
| CPUCore_3 | ~0% | el juego no lo usa |
| HostTiming | ~0% | ya corregido con `timer_spin` |

Demanda total: unos 3,3-3,4 procesadores logicos de 6. Lo que marca el ritmo son los **tres
nucleos emulados**, cada uno al ~90%.

Reparto de cada nucleo emulado:

| Subsistema | Parte | Que es |
| --- | --- | --- |
| Codigo del juego (JIT) | ~68% | lo que el juego realmente ejecuta |
| "JIT compiler" | ~13% | casi todo son exclusivos: el lambda de `EmitExclusiveReadMemory` (4,9%), `ExclusiveMonitor::CheckAndClear` (3,2%), el lambda de `EmitExclusiveWriteMemory` (0,9%) |
| Acceso a memoria del guest | ~5% | `Memory::Read64/Read16`, vias lentas de la tabla de paginas |
| Sincronizacion | ~4-6% | `CpuWaitForBits` (3,2%, espera ociosa), KSpinLock, scheduler |
| Draw setup (rasterizer) | ~4% | `RasterizerD3D12::OnCPUWrite` -> ntdll (2,7%), `GetFlushArea` |

Otro dato del frame chain (ventanas de 300 frames): el juego espera un framebuffer libre ~440-500
veces por ventana (1,9-3,0 s de 11 s). La composicion llega en promedio 16-22 ms despues del
pedido, y en 160-260 de 300 frames pasa de un frame. Parte del techo de FPS es presentacion y no
CPU; conviene tenerlo en cuenta al leer cualquier A/B de CPU.

## Propuestas, por prioridad

Cada una lleva su ganancia esperada y como medirla. Toda medicion se hace en el mismo lugar del
juego, con el mismo boot.cfg base (el de 0.3.8.2 con `async_record=1`) y comparando el perfil de
hilos y el FPS.

### 1. Exclusivos en linea sin fastmem (lo mas grande y lo mas seguro)

**Problema.** Sin fastmem (que no podemos usar: la Series se queda sin memoria), dynarmic compila
cada LDXR/STXR como una **llamada a un lambda en C++**. Ese lambda toma el spinlock global del
monitor, llama al callback de memoria del core y suelta el lock. Son ~8-13% de cada nucleo
emulado, porque los juegos de Switch usan exclusivos para todos sus mutex y atomicos.

**Lo que encontre en el codigo.** `EmitExclusiveReadMemoryInline` y
`EmitExclusiveWriteMemoryInline` (`src/dynarmic/.../backend/x64/emit_x64_memory.cpp.inc`, lineas
324 y 401):

- ya emiten el lock, la reserva y la comparacion **en linea**;
- solo la direccion host sale de `EmitFastmemVAddr`, y cuando no hay marca de fastmem ya caen a
  `code.call(wrapped_fn)`;
- se exigen `conf.fastmem_pointer` (un ASSERT) y `exception_handler.SupportsFastmem()`, y solo se
  eligen con `conf.fastmem_exclusive_access`.

**Cambio.**

1. Agregar una rama de tabla de paginas: `EmitVAddrLookup` (la misma que usan las lecturas y
   escrituras normales), luego `mov` o `lock cmpxchg` sobre el puntero host. Si la pagina no esta
   mapeada directo (memoria vigilada por el rasterizer, MMIO), saltar al abort que llama a
   `wrapped_fn`, igual que hoy.
2. Quitar la dependencia de fastmem en esa rama: no necesita el manejador de excepciones.
3. Exponerlo en `config` (por ejemplo `page_table_exclusive_access`) y en boot.cfg (por ejemplo
   `inline_exclusives=1`) para hacer A/B.

**Ganancia esperada.**

- Entre un 5 y un 10% de cada nucleo emulado, que vuelve al juego.
- En un juego que esta limitado por la CPU guest, eso son ~1-2 FPS.

**Riesgo y medicion.**

- El riesgo es la semantica de la reserva. El lock y la comparacion de valor ya estan probados en
  la via fastmem; lo nuevo es solo como se obtiene el puntero.
- Medir: perfil (el lambda y `CheckAndClear` deben desaparecer del top), FPS en el mismo lugar, y
  una sesion larga sin cuelgues (los mutex del juego dependen de esto).

### 2. Spinlock del monitor exclusivo menos disputado

**Problema.** `Dynarmic::ExclusiveMonitor` usa **un unico** `SpinLock` para los 4 cores, y cada
par LDXR/STXR lo toma dos veces. Con los cores en distintos nucleos fisicos, la linea de cache
del lock viaja entre nucleos; si cruzan de CCX, cada viaje cuesta ~60-70 ns.

**Cambio.**

- Primero medir: contar las vueltas del spinlock en el perfil.
- Si es contencion, alinear `lock` y los arrays de reservas a 64 bytes, para que no compartan
  linea con datos que se escriben seguido.
- Despues probar un lock por bucket de direccion, con un hash de la direccion de reserva.
  `CheckAndClear` tiene que mirar las reservas de todos, asi que esto pide cuidado.

**Ganancia.** Incierta; depende de lo que muestre la medicion. Va despues de la 1, porque la 1
ya saca la llamada y acorta el tramo con el lock tomado.

### 3. Ubicacion de hilos con CPU sets (A/B barato, potencialmente grande)

**Problema.** Hoy ningun hilo tiene afinidad: Windows reparte los ~5 hilos activos sobre los 6
procesadores logicos como quiere. Si pone dos nucleos emulados en la misma pareja SMT (lp2+lp3),
esos dos van a ~60-70% de velocidad cada uno, mientras otro nucleo fisico atiende hilos livianos.

**Cambio.** Agregar un `cpu_pin=` en boot.cfg que use `SetThreadSelectedCpuSets` (disponible en
UWP; sus ids ya los lee `CpuSetReport`). Plan base:

| Hilo | CPU sets |
| --- | --- |
| CPUCore_0 | lp2 (c2) |
| CPUCore_1 | lp4 (c4) |
| CPUCore_2 | lp6 (c6) |
| GPU | lp3, lp5 o lp7, el hermano menos cargado |
| D3D12Recorder, Fencing, audio, resto | los hermanos SMT restantes |
| CPUCore_3, prewarm, workers | sin pin o en los hermanos |

Notas:

- Usar CPU sets "soft" (`SetThreadSelectedCpuSets`), no `SetThreadAffinityMask`. Asi el sistema
  todavia puede mover el hilo si un procesador logico compartido lo ocupa el SO, en vez de dejarlo
  sin correr.
- Variantes a probar:
  - `cpu_pin=off` (hoy);
  - `cpu_pin=split` (la tabla de arriba);
  - `cpu_pin=split_gpu_own` (GPU en un nucleo fisico propio y los cores 1 y 2 compartiendo
    pareja), por si el hilo GPU es el cuello en algunas escenas.
- Ver con el perfil cuantas veces cada hilo cambia de procesador logico.

**Ganancia esperada.**

- Si el planificador hoy junta nucleos emulados en una pareja SMT en parte del tiempo, el pin lo
  evita: +5-15% en esos tramos.
- Si ya los separa bien, ~0. Hay que medirlo; es barato.

### 4. Encontrar los 2 procesadores logicos "compartidos"

Microsoft dice "4 exclusivos + 2 compartidos". Un procesador compartido puede perder tiempo
cuando el SO (el Dashboard, Game Bar, la captura) lo usa. Si un nucleo emulado cae ahi, esas
pausas se ven como tirones.

**Test.** Un hilo de diag que, por cada lp2-lp7 (pinneado con `SetThreadSelectedCpuSets`), corra
un bucle de 1 s leyendo el TSC y anote los huecos de mas de 50 us (tiempo que el procesador no fue
nuestro). Repetir con el Dashboard abierto y cerrado. Los dos con mas huecos son los compartidos.
El resultado decide la tabla de la propuesta 3: los nucleos emulados van en los exclusivos.

### 5. Verificar que MWAITX despierta con escrituras de otro nucleo

**Duda.** Un reporte en la comunidad de AMD dice que en algunos Zen, MWAITX no despierta cuando
escribe un nucleo que no es el hermano SMT; solo sale por el temporizador. Nuestro
`MonitorXWait` (`src/common/cpu_wait.cpp`) le pasa a `mwaitx` **todo el tiempo restante** como
timeout. Si el reporte aplica a la Series:

- `WakeFlag::Wait` (nucleos emulados ociosos, `idle_spin` adaptativo hasta 200 us) despertaria
  recien al final del presupuesto, y cada wake "spun" tardaria hasta 200 us en vez de <1 us;
- lo mismo para el tramo final de `Event::WaitFor`.

Los numeros del frame chain no lo descartan: hay ~23-25 mil wakes "spun" por ventana, y un wake
tardio igual cuenta como spun.

**Test.** Un microbenchmark en el diag: un hilo espera con `CpuWaitForBits` en lp2, otro escribe
desde lp4 (y desde lp3, el hermano) en un TSC conocido, y se mide la demora. Repetir 1000 veces y
anotar mediana y p99.

**Arreglo si da mal.** Partir la espera en tramos cortos (`mwaitx` con un timeout de ~2-5 us en
un bucle, que es casi tan barato como un tramo largo) o usar `pause` para el tramo final. Si da
bien, no se toca nada.

**Ganancia.** Si hay bug, es grande para la latencia de interrupciones entre nucleos emulados (el
juego despierta hilos en otros cores todo el tiempo).

### 6. Compilar mejor el ejecutable

Hoy: MSVC 14.51, `/O2 /Ob2`, LTO de host. Sin AVX2 ni PGO.

- **`/arch:AVX2`.** Todas las Series tienen AVX2. Ayuda a las partes en C++ que vectorizan
  (decodificacion de texturas en CPU, copias, hashing, el rasterizer). No toca el codigo JIT, que
  ya elige sus instrucciones en tiempo de ejecucion. Riesgo bajo, ganancia chica (unos %) en el
  hilo GPU. Es un cambio de una linea en el toolchain UWP; A/B con perfil del hilo GPU.
- **PGO de MSVC** (`/GENPROFILE` -> correr -> `/USEPROFILE` con `/LTCG`). Mejora inlining, layout
  y prediccion de saltos del codigo C++ caliente: callbacks de memoria, HLE, kernel, rasterizer,
  hilo GPU. No encontre numeros publicados de PGO en emuladores; en codigo C++ grande y con
  muchas ramas suele rendir algunos puntos porcentuales, y hay que medirlo. Problemas:
  - hay que generar el perfil corriendo la build instrumentada, idealmente en la consola (en PC el
    reparto de trabajo es parecido pero no igual);
  - la build instrumentada es lenta;
  - el `.pgd` se vence con cada cambio grande.

  Vale la pena para releases, no para cada build de prueba.

### 7. Opciones de dynarmic

Hoy estan activas todas las optimizaciones seguras (block linking, RSB, fast dispatch, const
prop, get/set elim, misc IR). Faltan dos cosas por revisar:

- **Cycle counting.** Con `m_uses_wall_clock` apagado, `enable_cycle_counting` agrega una resta y
  un salto por bloque, y sale del JIT cuando se acaba el presupuesto. Probar si con
  `use_wall_clock` (CNTPCT leido del reloj real) el juego anda igual y el JIT sale menos.
  Ganancia: unos %.
- **Optimizaciones unsafe** (`Unsafe_UnfuseFMA`, `Unsafe_ReducedErrorFP`, `Unsafe_InaccurateNaN`,
  `Unsafe_IgnoreGlobalMonitor`). Pueden ser grandes en juegos con mucho FP, pero cambian
  resultados. Solo como opcion del usuario y nunca por defecto. `IgnoreGlobalMonitor` en
  particular saltea el lock de la propuesta 2 (riesgo de carreras reales).
- **`JitProfile::Timer::Stop`** aparece con 0,5% en el perfil. Si el perfilado del JIT queda
  prendido en builds normales, apagarlo fuera de las builds de diagnostico.

### 8. Descargar al hilo GPU (46%)

El hilo GPU no marca el ritmo hoy (los nucleos emulados si), pero es el siguiente en la fila
cuando se arreglen la 1 y la 3. Opciones, de lo que ya existe a lo nuevo:

- Mover mas grabacion al D3D12Recorder (hoy al 6%): `async_record=1` ya lo hace en parte.
- Decodificar texturas en GPU (ya hay compute de decode) para no hacerlo en el hilo.
- Con la propuesta 3, darle al hilo GPU un nucleo fisico propio en escenas pesadas.

### 9. Memoria del guest y `OnCPUWrite`

- `RasterizerD3D12::OnCPUWrite -> ntdll` (2,7% de un nucleo emulado) es casi seguro un lock
  (SRWLock o critical section) tomado en cada escritura del guest a paginas vigiladas. Revisar
  si se puede consultar primero sin lock (bitmap atomico de paginas vigiladas) y tomar el lock
  solo si la pagina importa.
- `Memory::Read64/Read16` (~2-3%): son lecturas que no resolvio la tabla de paginas en linea.
  Contar por que tipo de pagina caen; si son paginas vigiladas, sale junto con lo anterior.

### 10. `timer_spin` por defecto

Con `timer_spin=100`, HostTiming bajo de un nucleo entero a ~0%, y el juego paso de 22-25 a
25,5-27 FPS, sin problemas de audio. Pasarlo a valor por defecto en la Series, y hacer un A/B
con 250 us por si mejora la puntualidad de los eventos sin volver a gastar el nucleo.

## Lo descartado (por ahora)

- **Fastmem:** la Series se queda sin memoria con fastmem full/hybrid.
- **Large pages:** no estan disponibles en UWP.
- **Mas hilos para el guest:** el juego usa 3 cores; el cuarto ya esta ocioso. Paralelizar mas no
  ayuda a la CPU guest. Lo que ayuda es que cada uno de esos 3 corra solo en su nucleo fisico y
  sin llamadas lentas.
- **D3D12PipelinePrewarm:** termina a los ~81-91 s y no compite despues.

## Orden recomendado

1. Propuestas 4 y 5 (tests de diag, sin riesgo): definen la 3 y dicen si la 5 hace falta.
2. Propuesta 10 (`timer_spin` por defecto) y propuesta 3 (`cpu_pin` A/B).
3. Propuesta 1 (exclusivos en linea). Es la ganancia mas segura sobre la CPU guest.
4. Propuestas 2, 9 y 7 segun lo que muestre el perfil despues de la 1.
5. Propuesta 6 (AVX2 primero; PGO para releases).

## Resultados en consola (0.3.8.71-0.3.8.76)

Todas las pruebas se hicieron en la misma escena: una sesion Pulse retomada, 150 s, con el
boot.cfg base (`async_record=1`) y sin fastmem.

### Lo que dijeron los tests de diag (`cpu_probe=1`, propuestas 4 y 5)

- TSC a 1745,5 MHz.
- **El sistema no le roba tiempo a ningun procesador logico.** CPU set 258 tuvo un 0,1% (3 huecos,
  el mayor de 372 us); los demas, ~0%. No hay "compartidos" que evitar.
- **MWAITX si despierta con escrituras de otro nucleo.** Mediana de 0,22 us desde el hermano SMT,
  0,41 us desde otro nucleo y 0,42 us desde el segundo hilo de otro nucleo. Ninguna de las 300
  rondas cayo al temporizador.
- Igual quedo el tope de ~3 us por tramo de `mwaitx` (`MonitorXSliceTicks` en `cpu_wait.cpp`),
  como red de seguridad: rearmar el monitor cuesta unas decenas de ciclos.

### La escena de prueba esta limitada por la GPU

- El juego espera a la GPU ~80 ms por frame: 300 esperas, 21 s de cada ventana de 25,6 s.
- Los nucleos emulados corren solo al ~19% de un nucleo cada uno.
- Por eso ningun cambio de CPU sube el FPS aca. La eficiencia se compara con los ms de CPU por
  segundo del perfil.

### Exclusivos en linea (propuesta 1): vale la pena, por defecto

`inline_exclusives=1`, por defecto, usa `page_table_exclusive_access` en dynarmic. A/B con el
mismo binario (0.3.8.73 contra 0.3.8.74 con `inline_exclusives=0`):

| | Inline (por defecto) | Por el monitor |
| --- | --- | --- |
| FPS | 12,24 | 12,61 |
| CPUCore_0 | 19,9% | 24,8% |
| CPUCore_1 | 18,3% | 22,8% |
| CPUCore_2 | 18,6% | 23,5% |
| Lambdas de exclusivos + `CheckAndClear` en CPUCore_0 | no aparecen | ~7,3% |

Son ~18% menos CPU por frame en cada nucleo emulado. En una escena limitada por la CPU guest eso
vuelve al juego. Las sesiones fueron estables, cerraron limpio y no hubo cuelgues de mutex.

### Monitor exclusivo (propuesta 2)

- Cada core tiene su reserva en una linea de cache propia (`alignas(64)`), y el lock va en su
  propia linea. Se elimina el false sharing.
- No se hizo el lock por bucket: con los exclusivos en linea, el monitor ya casi no aparece en el
  perfil.

### `timer_spin` (propuesta 10)

- 100 us queda por defecto. A/B contra 250 us: 12,40 FPS (0.3.8.72) contra 12,63 (0.3.8.75), que
  esta dentro del ruido.
- 100 gasta menos CPU girando, asi que se queda.
- `timer_spin=auto` tambien existe.

### `cpu_pin` (propuesta 3)

`cpu_pin=off|cores|split` existe y loguea `step: CPU pin`. Con `split`, cada nucleo emulado va en
un nucleo fisico y los hilos de servicio en los hermanos SMT. Fue estable, pero dio 12,08 FPS
contra 12,40 sin pin (0.3.8.76 contra 0.3.8.72).

Queda **apagado por defecto**, porque no hay ganancia medible que justifique fijar hilos:

- la escena no esta limitada por la CPU;
- la sonda no encontro procesadores robados.

Hay que repetir el A/B en una escena limitada por la CPU guest.

### Compilacion (propuesta 6)

- `/arch:AVX2` queda activo para el x64 de WindowsStore (`JADYX_HOST_AVX2`, en el CMakeLists
  raiz). Corrio estable en todas las pruebas.
- PGO no se hizo. Requiere una build instrumentada corriendo en consola, que es lenta, y el
  perfil se vence con cada cambio grande. Queda para releases.

### Lo que no se toco, y por que

- **Reloj de pared (propuesta 7).** Ya esta activo en multicore.
- **JitProfile.** No cuesta nada cuando el perfil esta apagado.
- **Optimizaciones unsafe.** Cambian resultados; solo como opcion del usuario.
- **Descarga del hilo GPU (propuesta 8).** Es un refactor grande y el hilo GPU esta al ~13%.

### Siguiente pista: `OnCPUWrite`

`RasterizerD3D12::OnCPUWrite` (`d3d12_rasterizer.cpp`) es ahora el mayor costo de host en los
nucleos emulados: ~12,5% de su tiempo, casi todo en ntdll, girando en el SRWLock / critical
section bajo contencion.

- En cada escritura del guest a una pagina cacheada por la GPU, toma `buffer_cache.mutex` y luego
  `texture_cache.mutex`. Lo llama `Memory::Impl::HandleRasterizerWrite`.
- Arreglarlo pide acortar las secciones criticas del hilo GPU, o un filtro sin lock (un bitmap
  atomico de paginas). Tiene riesgo de carreras, y queda como trabajo aparte con su propio A/B.

## Fuentes

- Microsoft, especificaciones de Xbox Series X (Hot Chips 2020), resumidas en
  [GTPlanet](https://www.gtplanet.net/xbox-series-x-system-specs-revealed-in-full) y
  [Guru3D](https://www.guru3d.com/news-story/xbox-series-x-specs-released-by-microsoft.html).
- Nucleo reservado al SO: [Cowcotland](https://www.cowcotland.com/news/71086/une-vue-eclatee-de-la-console-xbox-series-x-et-des-reponses-sur-l-hardware.html)
  (sin confirmar; nuestra medicion de CPU sets lo confirma en Dev Mode).
- MWAITX y su temporizador: [mwait.h del kernel Linux](https://codebrowser.dev/linux/linux/arch/x86/include/asm/mwait.h.html).
- Latencia de las instrucciones de espera en espacio de usuario:
  [TU Dresden, ICPE 2025](https://fis.tu-dresden.de/portal/de/publications/an-analysis-of-userspace-idle-state-instructions-on-x86-processors(64728c3d-56e5-425f-9c49-ab7327211938).html).
- MWAIT frente a HLT en Zen: [Phoronix](https://www.phoronix.com/news/AMD-MWAIT-Over-HALT-2022).
- PGO en MSVC: [Microsoft Learn, Profile-guided optimizations](https://learn.microsoft.com/cpp/build/profile-guided-optimizations)
  y [/USEPROFILE](https://learn.microsoft.com/en-us/cpp/build/reference/useprofile?view=msvc-170).
- Codigo propio: `emit_x64_memory.cpp.inc` (exclusivos), `interface/exclusive_monitor.h`
  (spinlock global), `common/cpu_wait.cpp` (MWAITX), `common/wake_flag.cpp` (idle_spin),
  `core/arm/dynarmic/arm_dynarmic_64.cpp` (config del JIT), perfil 0.3.8.70.
