# Xbox: cadena de cuadros, núcleo guest y 30 fps estables

Mecanismos para que el hilo GPU no espere a la TV ni a compilaciones largas, que el vsync del juego
llegue a tiempo y que los núcleos guest no salgan a C++ ni esperen locks en cada acceso a memoria
que la GPU tiene en caché. Sin fastmem: todo cabe en la memoria de la Series.

## Por qué

Los juegos con vsync dinámico y doble buffer piden swap interval 2 (30 fps). Cuando un cuadro llega
tarde suben solos a interval 3 (20 fps) y tardan en bajar. El hilo GPU (~31 ms por cuadro) y el
núcleo guest 2 (~32,6 ms) estaban al borde de los 33,3 ms, así que cualquier espera se volvía un
cuadro tarde. Las esperas venían de:

- `PresentManager::AcquireFrame`, que bloqueaba el hilo GPU dentro de `Composite` esperando un cuadro
  libre, el render anterior del cuadro y su copia a pantalla (espera de CPU).
- `WaitForEarlierComposite`, que frenaba el hilo de vsync antes de `SignalVsync` cuando el hilo GPU
  iba tarde: el juego veía un vsync perdido.
- Las esperas de PSO, que bloqueaban el hilo GPU entero.
- `OnCPUWrite`, que tomaba los mutex del buffer cache y del texture cache desde los núcleos guest.
- Las lecturas de páginas en caché de la GPU, que salían del JIT a C++ (~2 µs cada una).

## Claves de `boot.cfg`

Todo esto, salvo `swap_interval_hold`, viene encendido de serie; un paquete normal no lleva ninguna de estas claves. Cada
mecanismo conserva su clave `=0|1|check` para diagnosticar: en `check` corre la ruta vieja y cuenta
lo que haría la nueva, con las discrepancias aparte.

| Clave | Qué hace con `1` | Qué cuenta con `check` |
|---|---|---|
| `present_mailbox` | `AcquireFrame` no espera: sin cuadro libre reutiliza el cuadro en cola más viejo que aún no se copió. La copia anterior se espera en la cola de la GPU (`QueueWaitBeforeNextSubmit`), no en la CPU | cuadros que habría reutilizado |
| `vsync_nowait` | la espera del composite anterior antes del vsync tiene tope de 2 ms; el fence se revisa en el siguiente tick | esperas que habría cortado |
| `pso_budget` | los draws con pipeline pendiente esperan hasta gastar 6 ms por cuadro y la espera se corta ahí; después rigen las reglas Balanced (pasadas a pantalla completa y primeros draws a un target esperan, el resto se salta) | draws que el presupuesto habría saltado o esperado |
| `pso_loading` | en pantallas de carga reconstruye los pipelines liberados de sesiones anteriores, con el pacer sin descanso | cuántos habría reconstruido |
| `swap_interval_hold` | si el juego pidió interval 2 en los últimos 2 s, un 3 pasajero se muestra a ritmo de 2: el buffer vuelve al juego a tiempo. Si sigue pidiendo 3 más de 2 s, lo recibe | cuadros que habría paceado a 2 |
| `guest_clock_scale` | el contador de ticks del guest avanza a ese % del tiempo real (50-100); los eventos y el vsync siguen en tiempo real. Manda sobre el ajuste por juego «Reloj del juego al 90 %», apagado de serie porque un juego que avanza por ese contador iría más lento | `guest clock` en la línea `graphics:` del arranque |
| `cpu_write_defer` | las escrituras guest en memoria que la GPU no modificó van a una cola por núcleo, sin locks; el hilo GPU las aplica antes de usar las cachés | escrituras que habría encolado; discrepancias si `OnCPUWrite` encontró datos de la GPU |
| `jit_read_table` | las lecturas guest usan una segunda mitad de la tabla del JIT, que solo se borra cuando la GPU escribe la página: leer memoria en caché ya no sale a C++ | lecturas lentas que serían en línea; discrepancias si la GPU pudo haber escrito esa memoria |
| `cpu_pin=topology` | cada hilo con nombre va a su núcleo según el mapa fijo de abajo | — |
| `cpu_pin=map:nombre=núcleo[.lógico],...` | igual, con un mapa propio | — |

Otros valores de serie que forman parte del resultado:

| Clave | De serie | Por qué |
|---|---|---|
| `cpu_pin` | `topology` | el hilo GPU tiene un núcleo físico propio |
| `cpu_accuracy` | `unsafe` | `InaccurateNaN` y `ReducedErrorFP` en A64 son la mayor ganancia del núcleo guest al rotar la cámara. Riesgo: un juego que dependa de NaN o de la precisión exacta de esas operaciones puede verse o comportarse distinto. `cpu_accuracy=auto` lo descarta |
| `guest_swap_watch_mib` / `low` / `high` | 1536 / 1024 / 1280 | antes 1936 / 1424 / 1680: con esos valores Permafrost expulsaba memoria que el juego volvía a pedir |
| `texture_cap_line_mib` | 768 | las texturas pueden pasar del tope de 768 MiB mientras queden 768 MiB libres en la app. Por debajo de la marca baja de Permafrost: primero sale la memoria fría del guest |
| `texture_replay` | `1` | reutiliza los descriptores de textura del draw anterior |
| `astc_mip0` | `drop` | ASTC grandes sin su nivel superior: una cuarta parte de su memoria, a cambio de menos detalle |

`guest_yield_pause=<máx>` agrega `PAUSE` crecientes a los yield vacíos del guest. Viene apagada: con
ella hubo menos cambios de interval, pero el cuadro quieto subió a 35 ms.

`cpu_write_defer` y `jit_read_table` mantienen el bitmap de páginas escritas por la GPU
(`GpuWrittenPages`) aunque `dma_flush_filter=0`.

`cpu_write_defer` viene apagada. Se enciende en Ajustes > Gráficos o en los gráficos de cada juego
(«Cola de escrituras de CPU»), al iniciar el juego; la clave de boot.cfg manda sobre ambos. En
0.3.10.39, un juego con unos 324.000 escrituras por cuadro bajó de ~59 a ~27 FPS con ella, porque la
página sigue vigilada hasta que el hilo GPU vacía la cola y cada escritura va por la ruta lenta.
`cpu_write_test=1` la enciende y compara `current`, `merged` y `off` por ventana.

### Mapa de `cpu_pin=topology`

| Núcleo físico | Lógico 0 | Lógico 1 |
|---|---|---|
| 0 | GPU | D3D12Recorder |
| 1 | CPUCore_2 | CPUCore_0, CPUThread |
| 2 | CPUCore_1 | VSyncThread, D3D12Present, audio, D3D12PipelineBuilder |

El nombre se compara por prefijo y gana el más largo. Los hilos que no aparecen flotan, también el
pool de pipelines del arranque.

El núcleo guest 1 no comparte físico con los otros dos: con 0 y 1 juntos el juego caía a 20 fps al
caminar y rotar la cámara, sin composites tarde ni cuadros perdidos. Los builders de pipelines van al
lógico de vsync con prioridad de fondo: flotando caían en los hermanos libres del hilo GPU y de los
núcleos guest, y el cuadro pasaba de 33 ms mientras se construían los shaders de una zona nueva. A
cambio, los shaders nuevos tardan más en estar listos.

### Fuera del hilo GPU

- `QueryVideoMemoryInfo` corre en el hilo `D3D12VideoMemory` cada 100 ms; el hilo GPU lee el último
  valor. La llamada entra al kernel gráfico y puede tardar decenas de milisegundos.
- Los readbacks se conservan hasta 64 MiB por el pool de staging, en vez de crearse y destruirse.
- Un compute shader que `WorkgroupSplitPass` rechaza usa el pipeline plano del arranque, no construye
  una variante idéntica.

## Qué mirar en el log

### Informe de rendimiento (cada ventana)

La línea de la cadena de cuadros trae:

- `present frame waits: free / render / copy` y `over 1 ms`: el tiempo que el hilo GPU esperó en
  `AcquireFrame`, por causa. Con `present_mailbox=1` debe quedar en ~0.
- `queued frames reused (N would be)`: mailbox real y estimado.
- `vsync waits for the GPU thread cut at 2 ms`, `full waits`.
- `game changed its swap interval N times`.
- `CPU writes queued without the cache locks`: encoladas, estimadas, **mismatches**, cola llena,
  aplicadas y las que la GPU modificó mientras esperaban.
- `JIT load table`: páginas borradas tras escrituras de la GPU, lecturas lentas que serían en línea,
  **mismatches** y `flushes of unmarked memory`.
- `guest JIT runs ended N times`: salidas del JIT por llamadas al sistema (top 5 por número),
  interrupciones, invalidaciones del caché de código y data aborts.

Deben quedar en 0 en el paquete `check`: los dos **mismatches** y `flushes of unmarked memory`.

### Cambio de swap interval

Cada cambio de interval del juego deja una línea `frame chain: the game's swap interval went A -> B`
con los últimos 60 vsyncs: perdidos, espera por el hilo GPU (y cuántas se cortaron), composites
tarde, esperas de `AcquireFrame` por causa, stalls de pipeline y espera del juego por un framebuffer.
Sirve para ver qué disparó cada 2 → 3.

Los saltos cortos 2 → 3 → 2 (~0,4 s) salían con 0 vsyncs perdidos y 0 composites tarde: los decide el
juego. La línea cuenta lo que pide el juego, también con `swap_interval_hold`; los cuadros que siguieron
a ritmo de 2 salen en el informe como `frames of 3 paced at 2`. Un juego que calcule su tiempo de
simulación con el interval que pidió, y no con el reloj, avanza a 1,5x durante esos cuadros: 50 ms de
juego en 33 ms de pantalla. En consola se vio como aceleraciones cada pocos segundos con los fps
quietos, y por eso `swap_interval_hold` viene apagado de serie.

### Pipelines

- `D3D12: draws while their pipeline was built`: saltados, esperados por regla, `within the frame's
  budget`, y con `pso_budget=check` cuántos saltaría o esperaría el presupuesto.
- `D3D12: loading screen N: ... released pipelines of earlier sessions queued to build again`, o
  `would be built again (pso_loading=check)`. Una pantalla de carga son menos de 200 draws por
  cuadro durante 500 ms, o un cuadro con pocos draws después de 300 ms sin cuadros. Un cuadro con
  muchos draws la termina.

### Tirones largos

Con `profile=1`, si un cuadro va más de 120 ms tarde, el perfil guarda cada 32 ms la pila de todos los
hilos con estado `S`. Una captura que tarda más de 50 ms deja `profiler: pausing <hilo> took N ms`.

Ráfagas de ~2,5 s con cuatro cuadros de ~380/500/830/500 ms: el hilo GPU, el recorder y el hilo de
fences están a la vez dentro de `win32u` (kernel gráfico), sin ningún otro hilo con nombre trabajando.
No dependen de la carga, de Permafrost, del recorte del host ni de compilar pipelines, y tampoco de la
presión de memoria: el presupuesto local no cambia y el uso queda muy por debajo de él, mientras
`QueryVideoMemoryInfo` tarda de 250 a 780 ms. En la pantalla de título, con 100 draws y 503 MiB, salen
iguales, así que vienen de fuera de la app. Algunas corridas no tienen ninguna y otras las repiten
cada ~181 s, con la fase distinta en cada corrida. Causa sin identificar: no hay prueba de que sea
la captura del sistema.

`AppCapture` no sirve para probar la captura del sistema en la consola: `GetForCurrentView` devuelve
`80040154` (clase no registrada). `ApplicationView.IsScreenCaptureEnabled(false)` tampoco: con esa llamada la app no
arranca.

## Resultados en consola

Mismo punto del juego, cámara rotando, mismo guardado. "Cambios" cuenta los 2 → 3 y 3 → 2 de toda la
corrida; las cifras de rotación son por ventana de 10 s.

| Corrida | Qué agrega | Media | Peor ventana | Ventanas < 28 | Cambios | Ráfagas |
|---|---|---|---|---|---|---|
| 128 | valores de serie antes del mapa nuevo | | | | 76 | 2 |
| 132 | builders fijados a un lógico | 29,1 | | | 42 | 2 |
| 133 | mapa de núcleos actual | 29,6 | | 1 | 20 | 0 |
| 133 (repetición) | igual | 29,2 | | | 24 | 3 |
| 133 sin Portal | igual, sin consultas al Portal | 29,2 | 23,3 | 5 | 28 | 3 |
| 134 | compute no dividido usa el pipeline plano | 29,4 | 24,4 | 3 | 42 | 2 |
| 135 | igual | 29,7 | 25,4 | 1 | 28 | 1 |
| 136 | igual | 29,7 | 25,7 | 1 | 20 | 1 |
| 137 | igual | 29,4 | 23,6 | 2 | 28 | 2 |
| 139 | 700 s, primera visita a la zona de caminar | 29,3 | 24,5 | 7 | 76 | 5 |
| 140 | builders en el hermano del hilo GPU (descartado) | 29,0 | 24,7 | 5 | 32 | |
| 141 | mapa actual, zonas ya visitadas | 29,4 | 23,4 | 3 | 46 | 2 |
| 142 | `swap_interval_hold` (los fps de estas tres filas no cuentan las aceleraciones) | 29,7 | 24,5 | 1 | 40 | 1 |
| 142 (repetición) | igual | 29,7 | 24,3 | 1 | 56 | 1 |
| 144 | igual, 700 s, zonas nuevas al caminar | 29,4 | 24,1 | 6 | 74 | 3 |

Una corrida sin ráfagas no prueba nada: aparecen o no según la corrida. Fuera de ellas, rotando la
cámara el juego queda entre 29 y 30 fps, con p95 de 34 a 50 ms, y quieto en 30,0 con p99 de 34 ms.

La primera visita a una zona baja a 25-27 fps mientras el driver crea sus pipelines (en la 139, 45
pipelines y 17 s de creación). La lista guardada los precalienta en los arranques siguientes: en la
141 casi no hubo creaciones durante el juego.

## Gates

- `tools/xbox/tests/memory-coordinator.cpp`: marcas, yield, alivio y línea de texturas.
- `tools/xbox/tests/run-jit-read-table.ps1`: corre la biblioteca Dynarmic de producción con la
  configuración de la tabla de producción. En A64 y A32 comprueba que las lecturas (por los thunks de
  la tabla, ordenadas y exclusivas) usan solo la tabla de lectura y las escrituras solo la de
  escritura. Requiere haber compilado `jadyx` en `build-uwp`.

## Pendiente

- Partir el hilo GPU en frontend (DMA, decode de métodos y macros) y backend (`gpu_split`). Primero
  se mide cuánto cuestan el decode, el sink, las macros y los métodos con efectos.
