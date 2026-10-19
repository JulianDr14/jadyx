# Hyrule Warriors: investigación del crash (19 octubre 2026)

Rama: `feature/hyrule-crash-memory`. Investigación de logs, sin corrección funcional
ni cambio de presupuestos. No se ha reproducido todavía bajo depurador.

## Evidencia local

Fuentes: `C:/Users/julia/Downloads/jadyx_diag.txt`, `jadyx_log.txt` y
`jadyx_graphics_bugs.log` para el reporte 0.3.0.0; los correspondientes archivos
en `Downloads/log` para 0.3.4.7. En esta última carpeta el log del crash está en
`jadyx_log.txt.old.txt`: el archivo actual está vacío tras otro arranque.
Los diagnósticos acumulan sesiones y juegos; no atribuir sus tiempos absolutos
a duración de una sola partida.

- 0.3.0.0 contiene al menos dos crashes de Hyrule: a +147766 ms y +137125 ms
  de sus respectivas sesiones. Ambos terminan en spirv_to_dxil.dll (escrituras
  a 0x18 y 0x40), acompañados de bad_alloc en otros hilos y abort.
- Última sesión antigua: +121266 ms, texturas 384 MiB; +131266 ms, texturas
  768 MiB, staging upload dedicado 232 MiB, ring 128 MiB, DRAM 3039 MiB,
  JIT 205 MiB y commit 5066 MiB de 5120. Crash a +137125 ms.
- 0.3.4.7: Hyrule se selecciona desde USB a +1324843 ms, después de sesiones
  de Yo-kai. Crash a +1609296 ms: unos 284,45 s desde la selección de Hyrule.
  A +1604921 ms: DRAM 3063 MiB, JIT 207, texturas 704, buffers 104, ring 128,
  upload dedicado 168 y readback 13 MiB. No sumar esa muestra con otra posterior.
- Log reciente a 1604.812901 s: falla reserva de staging de 67108864 bytes;
  se reclama memoria y el reintento tiene éxito. Continúa compilando pipelines.
- A +1608750 ms se abre menú y pausa guest con commit 5045/5120 MiB;
  546 ms después se registra el crash, escritura a 0x0 en DLL+0x7fc4.
  La proximidad al menú no demuestra causalidad.

## Pila resuelta del ejecutable reciente

Se usa `tools/xbox/pdb_symbols.py`, DbgHelp y el exe/PDB archivado en
`build-uwp/symbols/0.3.4.7`. El log identifica esa versión, pero no incluye hash
del binario del usuario: correspondencia por versión, no identidad criptográfica.
No hay líneas fuente en el resultado; las entradas de cola inferiores pueden
reflejar inlining/optimización y no se interpretan como funciones exactas.

| RVA | Símbolo |
|---|---|
| 0xa72da2 | D3D12::ShaderCompiler::CompilePipeline+0x2c2 |
| 0x9d1eb7 | D3D12::PipelineCache::CreateComputePipeline+0x177 |
| 0x9d400a | D3D12::PipelineCache::CurrentComputePipeline+0x3aa |
| 0xa78868 | D3D12::RasterizerD3D12::DispatchCompute+0x108 |
| 0xa58883 | Tegra::Engines::KeplerCompute::CallMethod+0x163 |
| 0xa5e5a9 | Tegra::DmaPusher::CallMethod+0x289 |

La pila sitúa la llamada en traducción de un pipeline compute. No identifica
el hash del shader que falla: el último pipeline construido es uno que terminó,
no necesariamente el que provoca el crash. No se encontraron PDB/MAP en el build
local de Mesa ni archivo de símbolos 0.3.0.0; no resolver RVAs antiguos con un
PDB reciente. Los offsets internos de Mesa difieren entre capturas.

## Interpretación y siguiente gate

Falta de memoria demostrada por bad_alloc en la versión antigua y por la reserva
D3D12 rechazada en la reciente. Hipótesis principal: una asignación de Mesa falla
y algún consumidor usa el puntero nulo. El código local `util/ralloc.c` puede
devolver NULL al fallar malloc, pero no hay prueba de que la instrucción concreta
del crash corresponda a ese camino. Fuga, corrupción previa y error de shader
no quedan descartados por estos logs. No se ha demostrado device removed.

La ruta actual de staging recupera su propia reserva fallida; eso no garantiza
margen para los temporales de la siguiente traducción. CompilePipeline llama a
Mesa sin una comprobación local de headroom en esa función. Reducir prewarm para
la próxima sesión no libera por sí mismo memoria de la sesión en curso.

Gate siguiente: obtener símbolos del DLL exacto o capturar bajo depurador la
instrucción y asignación que fallan; registrar identidad/hash de la compilación
compute y margen al entrar/salir de Mesa. Evaluar después recuperación/admisión
de compilación y temporales de subida, conservando orden y datos guest. No
tratar una captura SEH y continuación como corrección segura de estado de Mesa.

## Investigación externa y propuesta (19 octubre)

**Prioridad revisada por el usuario:** investigar primero recuperación anticipada
efectiva y reservas ajustadas al tamaño necesario en Xbox UWP/D3D12. Las propuestas
de admisión Mesa y cap de uploads quedan aplazadas; el orden A–D inferior es la
propuesta histórica, no el orden vigente. No se autoriza aquí una reescritura de
sincronización. La revisión específica se recoge al final del documento.

Investigación solicitada por el usuario; propuesta aún sin implementar. Fuentes
primarias consultadas el 19 octubre 2026, junto al backend Vulkan local y
`xbox_texture_memory.md`. Los enlaces master son móviles: fijar revisión al
implementar una adaptación. No copiar valores de otros emuladores como si fueran
presupuestos válidos de Xbox UWP.

### Fuentes y aplicación

1. [Microsoft: Residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency).
   DXGI expone presupuesto/uso dinámicos; excederlos puede provocar pausas y fallos
   de creación. En UMA hay un pool físico; destruir heaps es más fiable para
   recuperar residencia que Evict. La reserva DXGI no aumenta el presupuesto.
   Aplicación: observar tanto límite app como presupuesto GPU, sin sumarlos como
   si fueran RAM independiente, ni confundir huecos del pool con memoria devuelta.
2. [Microsoft: gestión con fences](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management).
   Su ejemplo reutiliza un ring tras consultar/esperar fences, aceptando bloqueo
   para limitar memoria. Aplicación: limitar uploads en vuelo y reutilizar staging;
   hay que enviar primero el trabajo grabado en un punto seguro. Esperar un tick
   todavía no enviado no puede resolver la presión.
3. [Microsoft: MemoryManager](https://learn.microsoft.com/en-us/uwp/api/windows.system.memorymanager.appmemoryusagelimit?view=winrt-26100)
   y [cambios de límite UWP](https://learn.microsoft.com/en-us/windows/uwp/launch-resume/reduce-memory-usage).
   Leer límite real y reaccionar a cambios de límite/presión. Los ejemplos históricos
   de tamaño de memoria de esta página no describen la cuota actual de Series.
   Jadyx ya consulta uso/límite app; auditar eventos de ciclo de vida, sin atribuir
   este crash a background ni al menú sin evidencia.
4. [Xenia: TextureCache::CompletedSubmissionUpdated](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_cache.cc).
   Combina umbrales soft/hard con edad y último submission completado. El umbral
   hard permite expulsar antes, pero no recursos aún usados por la GPU. Aplicación:
   recuperación por bytes y fence, también durante cargas sin presents; sus límites
   numéricos no se trasladan a nuestro guest ni preservan automáticamente GPU-dirty.
5. [RPCS3: handle_memory_pressure y evict_unused](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/texture_cache.h).
   Recupera primero secciones pendientes de liberar y después amplía la purga
   según severidad; protege contra expulsión reentrante desde la propia caché.
   Aplicación: escalera de recuperación con orden de locks explícito y exclusiones.
6. [Dolphin: ShaderCache](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/ShaderCache.cpp)
   y [AsyncShaderCompiler](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/AsyncShaderCompiler.cpp).
   Separa número de workers de precarga y runtime, y dispone de ejecución síncrona
   si no hay workers. Esto demuestra una arquitectura adaptable; no demuestra que
   Dolphin tenga un gobernador de memoria. Propuesta propia: admisión por memoria
   para todas las llamadas Mesa, incluidas compute inline y graphics en workers.
7. [AMD D3D12MA: asignación óptima](https://gpuopen-librariesandsdks.github.io/D3D12MemoryAllocator/html/optimal_allocation.html).
   Usa presupuesto disponible y puede preferir committed al no caber un nuevo
   bloque grande; WITHIN_BUDGET permite rechazar recursos opcionales antes de
   reservar. Distingue fragmentación, alineación y coste de muchos heaps pequeños.
   Aplicación: conservar best-fit existente y añadir decisión por headroom antes
   de abrir otro bloque base64MiB. Migrar a la librería no resuelve por sí solo
   DRAM/JIT/temporales CPU ni prueba ahorro para este caso.
8. [Mesa: depuración de crashes/shaders](https://docs.mesa3d.org/graphics-debugging/debugging-misrenderings-crashes.html).
   Describe captura/reemplazo y reducción de shaders para aislar errores del
   compilador. No asumir que variables de drivers completos funcionen en nuestra
   DLL standalone: usar captura propia SPIR-V+opciones y el DLL exacto. El código
   local ralloc devuelve NULL si malloc falla; auditar consumidores requiere
   símbolos/reproducción, no basta con capturar una access violation.

### Lo que existe y los huecos concretos

- `d3d12_memory_guard.h`: HeadroomTracker reserva64MiB y vuelve a medir bajo
  presión. Es un estimador local al staging: otras asignaciones concurrentes
  pueden consumir margen desde la muestra. No es una reserva real de memoria.
- `CreateStagingBuffer`: si Fits falla intenta RelieveMemoryPressure; si esta
  devuelve nullopt, continúa con CreateMappedBuffer sin imponer Fits de nuevo
  como condición de admisión. La recuperación vuelve a medir, pero no garantiza
  el margen para Mesa. Rechazar sin diseñar propagación/reintento tampoco basta.
- `RelieveMemoryPressure` espera trabajo ya enviado, recoge garbage y recursos
  retirados de su caché; deliberadamente no envía la lista actual ni toma la
  caché contraria, para conservar estado y orden de locks.
- `FlushCommands` ya envía cargas>=8MiB al terminar una lista guest;
  `FlushIfUploadHeavy` usa32MiB entre operaciones. No falta todo el mecanismo:
  hay que comprobar qué bytes quedan pendientes y por qué no se recuperan en
  la carga concreta. Una sola petición observada ya ronda56/64MiB.
- Compute llama Mesa inline en `CreateComputePipeline`; limitar solo los
  cuatro workers graphics no protege esta ruta. `ShaderCompiler::CompilePipeline`
  no tiene admisión local por memoria.
- El guard de frame limpia staging/cachés y aplica GC; una fase larga de carga
  puede crear recursos entre frames. La recuperación debe existir también en
  puntos seguros durante carga, no depender exclusivamente de TickFrame.
- Pool de texturas ya usa best-fit y trim por fence. Un hueco dentro de un heap
  ocupado no reduce el commit del heap. Antes de otro bloque usa base64MiB;
  revisar elección exact-size/committed bajo presión, sin perder su ruta rápida.
- Temporales de textura: swizzle/unswizzle, conversión y repack pueden coexistir
  con staging y destino GPU. Medir capacidad retenida, no solo tamaño lógico.
- Shader::ObjectPool::ReleaseContents conserva/consolida capacidad para reuso;
  no equivale a liberar toda su RAM. Medir pools idle por worker antes de añadir
  una reducción a tamaño base. Tampoco está probada una fuga.

### Orden propuesto y gates verificables

**A. Medición y admisión coordinada antes de Mesa y reservas grandes.**
Registrar hash/stage/origen, margen app/DXGI, compilaciones activas, bytes
staging pendientes enviados/no enviados y capacidades scratch. Muestras de
entrada/salida no capturan necesariamente el pico interior: medirlo en harness.
Diseñar un presupuesto común que descuente temporales admitidos aún no reflejados
en uso real, sin contarlos dos veces. Un intervalo exploratorio de margen
128/192/256MiB debe validarse; ninguno garantiza que cualquier shader quepa.
Reducir a una traducción simultánea bajo presión y detener precarga opcional;
compute inline también obtiene permiso. El coordinador GPU reclama memoria en
puntos seguros; un worker no debe llamar Finish, expulsar texturas ni esperar
que el GPU thread lo rescate mientras este espera ese mismo worker.

Gate: concurrencia inline/worker sin deadlock, cancelación/cierre, fallo de
medición conservador, carga con margen insuficiente sin invocar Mesa a ciegas;
medir pico por shader real. Si ni con una compilación y recuperación cabe, salir
del juego con diagnóstico útil, sin seguir emulando dispatches omitidos.

**B. Acotar staging pendiente, reutilización y temporales de carga.**
Antes de un upload grande, coordinar submit en punto seguro, esperar fence útil,
retirar y reutilizar. El límite debe contar capacidad real y buffers retirados,
no solo bytes solicitados. No hacer Flush a mitad de un draw/dispatch o mientras
el caller espera descriptores/estado de esa lista. Después, evaluar upload por
mip/layer o filas de bloques comprimidos para peticiones mayores que el cupo;
respetar pitch, offsets, BC, arrays, volumen y copias guest preservadas. No basta
con cortar el memcpy en chunks si se reserva igualmente toda la imagen.

Gate: bytes GPU idénticos para RGBA/BC/arrays/mips/volúmenes/repack, ring-wrap y
presión con carga sin presents; cap real observado y número de esperas. Antes y
después de convertir conservar lecturas/invalidation/versionado de la caché.

**C. Recuperación efectiva y pool sensible al presupuesto.**
Primero limpiar staging/scratch idle y cachés opcionales; después recursos
retirados con fence y heaps vacíos. Si hace falta expulsar una imagen GPU-dirty,
preservar datos/readback y versiones existentes; nunca descartar por ser vieja.
No declarar ahorro hasta observar bajar uso app, y registrar bytes efectivamente
liberados frente a lógicos. Para nuevo heap, preferir bloque menor alineado o
committed exacto cuando base64MiB no cabe con margen. Mantener tombstones,
clases/alineación y fences; committed de fallback también necesita presupuesto.

Gate: packing bajo presupuesto, RT/DS/MSAA, retiro diferido y datos; reducción
medida de heaps/commit sin churn repetitivo ni caída de rendimiento sostenido.

**D. Endurecer y aislar Mesa.**
Capturar shader fallido y símbolos del DLL exacto. En harness de compilación
probar asignaciones fallidas controladas, identificar consumidores NULL y rutas
de limpieza. Un simple catch(std::bad_alloc) C++ no cubre AV de código C. Reparar
un NULL concreto no prueba que todo Mesa sea tolerante a OOM. No seguir tras
SEH con estado de compilador potencialmente incompleto. Mantener margen y una
salida limpia como protección aun después de corregir el fallo interno.

### Límites y evaluación manual

Conservar5120MiB medidos y memoria guest; no elevar cuota ni reducir DRAM del
juego como solución. No cambiar presupuesto JIT global sin medir: el crash
reciente tiene207MiB de código frente3063MiB de DRAM y704MiB de heaps en la
muestra previa; prewarm de próxima sesión no recupera ese código en vivo.
Evict, defragmentación, tiled resources o cambio a otro allocator no son primer
parche: añaden sincronización y la mudanza de recursos necesita destino temporal.
No usar compresión con pérdida ni eliminar los bloques BC preservados.

PC: harness de presión y luego juego con Job5120, play1, usuario cierra conQ,
sin timeout ni inputs programados. Comparar la misma carga con caches frías y
calientes, primera sesión y tras cambiar de juego; registrar mínimo margen,
pico de memoria, compilaciones, staging, heaps, OOM/reintentos y p99/maxgap.
Series: repetir hasta superar la transición del crash y continuar gameplay;
visual/audio/cierre y retorno biblioteca correctos. PC con VRAM dedicada no
certifica RAM unificada. Sin crash en una prueba no garantiza todos los juegos
ni supone mejora FPS. No se ha implementado ni validado aún ningún candidato.

## Revisión específica Xbox: recuperación y tamaños de reserva

Decisión del usuario19oct: estos dos frentes primero. Solo investigación/diseño
en esta revisión, sin build, cambio funcional ni paquete.

### Capacidades verificadas y fuentes adicionales

El log reciente, `jadyx_log.txt.old.txt:4155`, declara `UMA yes, cache-coherent
UMA yes`; a4158 DXGI informa4147MiB de presupuesto local al arrancar Hyrule.
El límite app registrado es5120MiB. Son dominios de contabilidad diferentes,
no dos bancos que sumen9267MiB; consultar ambos en tiempo de ejecución.

- [Microsoft: D3D12_FEATURE_DATA_ARCHITECTURE](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture):
  UMA no implica por sí sola que cualquier textura admita acceso CPU directo;
  CacheCoherentUMA abre oportunidades de evitar copias, con reglas de layout y
  caché que deben validarse. Aquí no se propone cambiar DEFAULT a UPLOAD ni usar
  zero-copy indiscriminadamente. Liberar duplicados realmente innecesarios tiene
  interés en UMA, pero los bloques BC preservados tienen función de coherencia.
- [Microsoft: estrategias de memoria](https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-management-strategies):
  clasificar recursos, presupuestar y gestionar su permanencia. Aplicación:
  diferenciar datos activos, reconstruibles, GPU-dirty y retirados antes de
  decidir qué recuperar; no purgar por tamaño sin mirar uso y propiedad.
- [Microsoft: tamaños/alineación](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_allocation_info):
  usar SizeInBytes y Alignment reportados, no width*height*bpp ni asumir64KiB para
  todo. MSAA y clasesRT/DS siguen sus requisitos. El alineamiento pequeño4KiB
  es una posibilidad condicionada, no cambiar todos los descriptores a4KiB.
- [Microsoft ATG](https://github.com/microsoft/Xbox-ATG-Samples): separa UWPSamples
  de XDKSamples. Los ejemplos ESRAM son XDK-only y no solucionan memoria de esta
  app UWP en Series. No trasladar APIs GDK/XDK a este backend.
- [Asignación UWP Xbox histórica](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation):
  contexto XboxOne y distinción app/game. Para Series manda la cuota medida por
  MemoryManager, no reinterpretar documentación histórica como acceso a toda la
  RAM física. Tampoco se requiere Agility/tight-alignment para los dos candidatos.

### 1. Recuperar antes, y verificar que la memoria vuelve

El controller actual YA inicia Pressure con768MiB libres, Critical384 y
Emergency192, con histéresis. No basta proponer subir umbrales: hay que localizar
por qué aun así el proceso acaba con75MiB de margen.

Ruta actual: TickFrame -> GC -> DeleteImage -> sentenced_images -> destructor de
Image -> retiro por scheduler/fence -> Allocation::Free -> TrimEmptyHeaps.
El ring genérico mantiene imágenes/vistas/framebuffers8ticks (6 con macro Android).
Una eliminación lógica puede esperar esos ticks antes de llegar siquiera al
retiro D3D12. scheduler.CollectGarbage no vacía ese ring genérico. Una carga con
pocos presents puede retrasar ambas observaciones de presión y destrucciones.
Esto es un candidato explicativo, no prueba de cuántos MiB retenía ese ring en
la captura: falta contabilizarlo.

Primer diseño: una recuperación D3D12 en puntos seguros de carga, solicitada
ANTES de abrir un heap, que recoja recursos ya retirados y libere heaps vacíos,
y que pueda ejecutar mantenimiento de caché sin esperar otro present. Si no
basta, seleccionar imágenes reconstruibles no activas, completar preservación
de GPU-dirty y evaluar sustitución D3D12 del retardo por frames por lifetime
demostrado por fence. No vaciar el ring a ciegas: también protege referencias
host, vistas, framebuffers y descriptores. No cambiar Vulkan ni otros backends.

La recuperación devuelve un resultado medible: bytes lógicos expulsados, bytes
en ring, bytes en retire/fence, heaps vacíos liberados y uso app antes/después.
Reconsultar el presupuesto tras recuperar, en vez de descontar una textura como
si todo su heap hubiera desaparecido. Mantener caliente al menos la política
normal existente: ya hay evidencia local de churn al vaciar pools cada frame.
Bajo presión real puede liberarse todo heap vacío, preservando recursos vivos.

Gate: retirados con fence pendiente/completo, referencias host, cambio de juego,
carga sin presents, datos GPU-dirty y readback/versiones; probar que baja el uso
app sin usar recursos destruidos ni expulsar/recrear lo mismo continuamente.

### 2. Reservar lo necesario, sin abrir siempre64MiB

`TextureResourceAllocator::Create` usa best-fit, luego
BlockSize(SizeInBytes,Alignment,64MiB). Solo pasa a committed tras fallar el heap
o placed. Los recursos mayores ya se alinean sin redondearlos a potencia de dos;
ese ahorro existe y no debe presentarse como cambio nuevo.

Diseño de decisión, después del mantenimiento anterior y una nueva muestra:

1. Reutilizar un hueco compatible de un heap existente; no aumenta su capacidad.
2. Con margen normal, conservar bloques64MiB para amortizar creación.
3. Bajo presión, calcular la capacidad alineada requerida por la asignación.
   Si abrir64MiB desperdicia demasiado o consume el margen de trabajo, usar un
   bloque menor válido o committed exacto. Evaluar los dos en gate GPU: un bloque
   menor aún puede reutilizarse; committed evita mantener huecos de ese recurso.
4. Registrar tamaño pedido, SizeInBytes, capacidad nueva, desperdicio y motivo de
   elección. El fallback committed no se considera gratuito ni se reserva sin
   mirar presupuesto. No fijar tamaños16/32MiB sin medir distribución y churn.

Ejemplo aritmético, no predicción de ahorro real: si SizeInBytes=12MiB y la
alineación permite12MiB, evitar un bloque64MiB ahorra52MiB de capacidad inicial.
Un hueco12MiB dentro de64MiB ya existentes, en cambio, no devuelve52MiB al OS.
El coste real del driver y la variación app se miden; no prometer ese delta exacto.

También revisar `DedicatedStagingSize`: ya redondea reservas grandes por pasos
de1/16 de la siguiente potencia de dos (no al doble). Para la petición observada
55923840B reserva58720256B: excedente2796416B, unos2,67MiB por buffer. Se puede
comparar alineación exacta bajo presión conservando buckets y selección por
capacidad real, sin introducir aquí límite de uploads. No confundir ese ahorro
con los56MiB completos del buffer, que contienen datos necesarios.

Gate: selección por presupuesto/alineación, overflow, clasesRT/DS/textura, MSAA,
best-fit/tombstones/fence; GPUdebug y datos exactos, mismo escenarioSeries con
menor heap/commit máximo y sin regresión visual, churn ni hitches excesivos.

### Orden vigente

R1: medir y recuperar retirados/heaps antes de crecer, incluir carga sin presents
y resolver lifetime del ring genérico si demuestra retención importante.
R2: elegir reservas ajustadas bajo presión, conservando pooling normal.
Separar los cambios para atribuir memoria y coste. No afirmar todavía que alguno
evite el crash de Mesa: el objetivo inicial es recuperar margen real en la carga
que lo provoca, manteniendo datos y comportamiento del juego.

## Primer candidato implementado — 19 octubre

Cambios en `d3d12_resource_allocator.cpp` y política pura
`d3d12_heap_growth_policy.h`:

- Cuando no hay hueco, recoge retiros completados y vuelve a buscar antes de
  crecer. Bajo presión y con allocations pendientes, espera como máximo una vez
  por tick al tick anterior, nunca al de la lista actual; luego recoge otra vez.
  La espera solo se permite en el recording thread, fuera del mutex del pool.
- Reserva el hueco bajo el mutex antes de hacer trim; así el trim no destruye el
  heap elegido. Libera todos los heaps vacíos bajo presión, uno caliente por
  clase en política normal. No vacía el ring genérico ni ejecuta GC reentrante.
- Presión de crecimiento: margen app proyectado<=768MiB o margen GPU
  proyectado<=20% del presupuesto, dominios independientes. Solo se consulta al
  necesitar crecimiento. Tras recuperar vuelve a medir; si falla la medida
  conserva la elección compacta en lugar de volver a64MiB sin información.
- Si persiste presión usa capacidad mínima alineada de la textura; normal
  conserva64MiB. Heaps MSAA usan alineación4MiB cuando la pide el device; no se
  colocan en heaps cuya alineación base sea insuficiente. Sin Agility ni APIsXDK.
- Logs registran app/heaps/pending antes/después, reutilización, bytes requeridos
  y elección compacta. Report añade recuperaciones, heaps compactos y capacidad
  evitada acumulada; esta última NO es delta de commit ni ahorro neto residente.

No cambia límite5120, DRAM, JIT, política de Mesa, staging ni Vulkan. Es una
primera mejora de recursos YA retirados y crecimiento; no certifica recuperar
las imágenes todavía retenidas por el ring genérico de8frames. Medir ese tramo
y resolver referencias host sigue pendiente, junto a scratch y reservas staging
ajustadas. No intentar un drenaje ciego para completar ese pendiente.

Verificación ejecutada:

`powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-heap-growth.ps1`

-38136 combinaciones tamaño/alineación y casos app/GPU, igualdad de umbral,
  desconocido, sobrelímite y overflow.
-1248000 placements contra referencia exhaustiva del gate packing previo.
-GPU desktop debug:6 recursos compactos RGBA/BC1/3D/RT/depth/MSAA;31052bytes de
  contenido de RGBA/BC1/3D/RT exactos tras upload/readback, incluyendo arrays/mips.
  Depth/MSAA validan creación legal, no equivalencia de contenido. Cero mensajes
  D3D12 error/corruption. Capacidad sintética384MiB->4,375MiB, NO gameplay/commit.
-ObjetoUWP `d3d12_resource_allocator.cpp.obj` compilado correctamente.
  El gate GPU usa las funciones de tamaño compartidas y D3D12 real, no instancia
  el allocator completo ni certifica sus waits/callbacks integrados.
-`git diff --check` correcto. Evidencia en `test-results/hyrule-memory/gates.txt`.

Link integrado, gate GPU dentro del runtime, cargaHyrule, estabilidad de heaps,
churn, delta real de memoria y Series pendientes. No paquete ni commit.
Build amplio lo ejecuta el usuario según reglas del proyecto:

`tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx`

### Entrega solicitada0.3.4.8

Build incremental/link UWP correcto el19oct2026 (37operaciones anunciadas por
Ninja; última salida36/37). Warning preexistente BUILD_ID redefinido en scm_rev.
No errores de compilación/enlace. Sin procesos Jadyx activos antes del reemplazo.
`package-appx.ps1 -Release` produce entrega biblioteca normal, sin payload de
usuario ni instrumentos, `library=1/play=1`; manifest0.3.4.8 sin BOM.

- `build-uwp/package/jadyx.appx`:15410353bytes.
- CRCZIP,34archivos/735bloques SHA256 BlockMap y firma matemática CMS correctos.
  FirmanteCN=JadyxDev; CheckSignature(true) valida firma, no confianza de cadena
  en el dispositivo. DependenciaVCLibs/cert presentes en la carpeta de entrega.
- Exe de APPX idéntico al build y al archivado con PDB en `symbols/0.3.4.8`.
- SHA256exe:baabccc1ff9dd2697c394c103926fe9b7745ca0c12e6d1ed6c4726506fc354ca.
- SHA256APPX:9b16f056b25ff47427d644f3b66eff463a15f8780367ea6424eb099676817d81.
- Verificación actualizada en `build-uwp/package/verification.json`, evidencia
  y verificador local en `test-results/hyrule-memory`.

Esta entrega cierra el link/paquete pendiente anterior. Gameplay integrado,
recuperación real/margen/churn y pruebaHyrule enSeries siguen pendientes. No
certifica crash resuelto. No se ejecutó juego ni se hizo commit/push.

### LogsSeries0.3.4.8 revisados19oct

Evidencia copiada sin modificar originales desdeDownloads/log a
`test-results/hyrule-memory/series-0.3.4.8-20261019`. DiagSHA256
38A1A77AE7C9CF139C080EA70A2F9946175849C8B94BC51314119C5DAE771AF4,
logSHA256B6A57BE4DFBD969A7C16FF43CCBB0D3FF043C34DC2265A9C393589C6EDC45C71.
El diag acumula versiones: el crash a+1609296ms pertenece a0.3.4.7, NO a.8.
La parte.8 empieza en selección+4250ms/manifest+4250ms (líneas493/494).

Tres sesionesHyrule.8 vuelven a biblioteca con retorno10: +53078,+224766 y
+435860ms; máximos app muestreados3974/3068/4741MiB. Tercera selección+226328
y cierre+435860:209,532s incluyendo carga, pausa/guardado y shutdown, no todo
gameplay. SinCRASH/bad_alloc/FIRST-CHANCE en tramo.8. Log actual tiene cero
RenderError/Critical/OOM/AssertionFailed. Identidad por versión y mensajes del
candidato, no hash remoto de exe. No gameplayvisual certificado por logs.

Última sesión activa el candidato:19growthrecoveries,19heaps ajustados,
0reused,0trimmed y todas las muestras recovery conpending0/app/heaps sin variar.
Por tanto reserva ajustada ejercitada; liberación efectiva por esa ruta NO
demostrada: no había retiros contabilizados recuperables en esos intentos.
No atribuir reducción global de memoria al reclaimer.

19recursos compactos suman38862848B(37,0625MiB), desde128KiB hasta9,8125MiB
por recurso. Cierre:933MiB en33heaps, pico895MiB live,1195placed/0fallback.
Contador avoidedcapacity1178MiB es suma de diferencia por decisión frentebase64,
NOahorro neto contra baseline: un allocator64MiB puede empaquetar varios recursos
en un mismo bloque. No proyectar+1178MiB al consumo antiguo como contrafactual.
El log redondea heaps pequeños a0MiB; requiredbytes revela tamaño real positivo.

Appmáxima muestreada4741/5120:379MiB de margen, sin prueba de pico absoluto.
Stagingdedicado llega302MiB/38buffers y luego0; heaps permanecen933MiB al final.
La sesión reciente reanudaPulse y tiene DRAM2153MiB al final frente3063previos:
escenario/residencia/estado diferentes, no A/B causal ni misma transición probada.

Otros errores:86FFmpegSendPacket InvalidData en tercera sesión (aprox239,9–242,9s
del proceso),9BSDnetwork notinitialized y3252InputUnknownEngine. No son OOM.
Pulse lanza excepciones controladas por estado/textura no soportados, pero cada
guardado termina registrado como saved; último11,333s, retorno biblioteca limpio.
No tratar esos throws como crash. FFmpeg/visuales/Pulse requieren seguimiento
separado si usuario observa fallo; no modificar funcionalidad ajena en revisión.

Resultado: no reproduce crash en estas sesiones y valida heaps pequeños enSeries;
recuperación real/ring8frames/estabilidad prolongada ymisma transición pendientes.

### Confirmación de la prueba en Series

El usuario aclara19oct que guardó conPulse antes de la zona que conocía como
reproductora del fallo, reanudó desde ese punto y pudo atravesarla correctamente
en.8. Confirma funcionamiento y menor RAM observada en su comparación manual.
Esto valida pasar el punto conocido en esa prueba; no es un A/B automatizado
de RAM ni garantiza todos los escenarios. Se conserva el candidato de memoria.
