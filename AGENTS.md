# AGENTS.md: guía para agentes de IA en eden-xbox (fork de JulianDr14)

Léelo entero antes de tocar nada. Resume qué hacemos, en qué punto estamos y cómo trabajamos. El
detalle técnico está en [`docs/xbox_internal.md`](docs/xbox_internal.md) (cuaderno del proyecto) y en
[`docs/xbox_d3d12_phase3.md`](docs/xbox_d3d12_phase3.md) (diseño de la fase actual).

> `CLAUDE.md` viene del fork original (juanresendiz813) y apunta a rutas `C:\Users\juanr\...` y a un
> tablero de coordinación que **aquí no existe**. Para este fork, la referencia es este archivo.
>
> **Eden prohíbe el uso de IA** en su proyecto y su comunidad (su `AGENTS.md`/`CONTRIBUTING.md`).
> Este fork es independiente: nada de lo que hacemos aquí se envía a Eden (ni PRs, ni issues, ni
> comentarios). Al fusionar Eden, conservamos nuestros `AGENTS.md` y `CLAUDE.md`.

## Qué estamos haciendo
- Portamos **Eden**, un emulador de Nintendo Switch (C++20, GPLv3), a **UWP** para **Xbox Series X|S
  en Dev Mode**.
- En Xbox UWP no hay Vulkan ni OpenGL, así que estamos escribiendo un **renderer Direct3D 12** nativo
  en `src/video_core/renderer_d3d12/`.
- Los shaders se traducen así: SPIR-V de Eden → Mesa `spirv_to_dxil` → DXIL firmado con `dxil.dll`.
- Probamos con un homebrew propio (`tools/xbox/boot_nro/`) que dibuja un patrón de test durante
  600 frames. Nada de juegos comerciales.

## En qué punto estamos (actualízalo al avanzar)
| Fase | Estado |
|---|---|
| Boot en consola (JIT con W^X, AppContainer) | ✅ Gate 2 |
| 1: device, swapchain y framebuffer por CPU | ✅ Gate 3, commit `cc2ea38d9` |
| 2: shaders SPIR-V→DXIL, blit por GPU | ✅ Gate 4 en la Series (0.2.7.0); **sin commit** |
| 3a.1: scheduler | ✅ PC y Series (0.2.8.0); sin commit |
| 3a.2: staging pool | ✅ PC; logs correctos en Series (0.2.9.0), confirmación visual pendiente |
| 3a.3: descriptor heaps | ✅ PC; logs correctos en Series (0.2.10.0), confirmación visual pendiente |
| 3b: runtime de la caché de buffers | ✅ PC y Series (0.2.11.0); round-trip GPU de 4096 bytes |
| 3c: runtime de la caché de texturas | ✅ PC y Series (0.2.12.0); gate 13×7 RGBA8 y vistas completas |
| 3d: fences, queries y `RasterizerD3D12` sustituyendo al nulo | ✅ PC y Series (0.2.13.0) tras la revisión de la fase 3; **fase 3 cerrada** |
| Mudanza a Eden actual (sep 2026): fork de `eden-emulator/mirror`, rama `xbox` | ✅ PC (merge `b18b22a674`); la Series se da por buena (el usuario no la repitió) |
| 4: pipelines, root signature y shaders del guest | 🔨 4.0–4.3 ✅ en PC y Series (0.2.15.0); 4.4 ✅ en PC (compute en ex09, blits y clears con máscara en ex10, dispatch indirecto con ExecuteIndirect en ex11; limpio con la capa de debug); ex11 ✅ en la Series (0.2.17.0); primer juego (Game C) hasta la pantalla de título en PC (0.2.18.0: W^X por páginas en dynarmic, applet Application, ManualContentProvider, SRVs incompatibles); título ✅ en la Series (0.2.19.0, sampler con MaxLOD ≥ MinLOD); siguiente: pasar del título (entrada) y rendimiento. Diseño en `docs/xbox_d3d12_phase4.md` |
| 5: paridad (ASTC, stream output, quads, etc.) | 🔨 ASTC GPU + BC3 por defecto (Series OK en 0.2.58.0); ~9 us por draw y DXIL en workers (0.2.60.0); XAudio2 2.9 nativo ✅ Game C PC 92 s, 0 glitches/starvations/fallos, gate prolongado Series pendiente; fastmem hibrido descartado; page-table JIT limpia mejora en PC y funcionamiento observado en Series 0.2.66.0 (~124 s, gameplay ~27--43 FPS; mejora sugerida, A/B y cierre pendientes); pool de placed textures reduce 648 creaciones de 438,7 a 12,4 ms, gate de Series pendiente; PSO RGBA8 offset 14 y MIN/MAX puntual exacto corregidos, cache PSO graphics/compute compartida: PC Game C 75 s con debug limpio, gate Series 0.2.67.0 pendiente; depth feedback GPU y deswizzle por sectores (RGBA8 2,66--3,07x aislado, 5.400 casos), traduccion redundante de memoria CPU eliminada y perfil por core: PC Game C 75 s con debug limpio y cierre 0; Series 0.2.68.0 pendiente |

La rama de trabajo es `xbox`. `master` del fork es Eden tal cual.

Objetivo FPS/estabilidad (30 sep 2026): pruebas manuales hasta Q; 60 FPS sostenidos aun
no demostrados. Perfil JIT PC: protecciones de pagina suman 43,6% del tiempo agregado de
compilacion. Candidato sin commit: tablas de handlers estaticas, eliminar nombres perf-map
no-op en Windows e invalidar FastDispatch sin ejecutar lookup JIT ni cambiar RX/RW por bloque.
Build UWP y 262144 casos de hash correctos; candidato PC 97 s manuales, retorno 0,
sin errores Render y dos asserts BufferQueue; coste Compile agregado 1,6% menor observado,
sin mejora FPS sostenida demostrada (ultimas ventanas 51--54 FPS); A/B y Series pendientes.
Diseno y evidencia en [`docs/xbox_performance.md`](docs/xbox_performance.md).
Fastmem Full PC: 84 s manuales, retorno 0; ultimas 900 presents 51,82 FPS frente a
51,17 sin fastmem, callbacks reads ~95,7% menos; sin mejora sostenida demostrada ni
60 FPS. Ocho asserts BufferQueue; Full Series/default Xbox no certificados ni cambiados.
Diagnostico siguiente: T registra esperas IPC/SVC, guest Runnable y signal VSync;
release->fin dequeue host observado 40--48 us mediana, max 112 us, no explica solo
los tirones largos. Asserts BufferQueue ahora tienen contexto de slots/limites.
T a 240 vsyncs (~4 s) validado PC en tres capturas completas; ocho asserts identifican
slot 2 Queued/Acquired con limite 2. HUD superior derecho reutiliza texto/panel de shaders:
FPS, FRAME/MAX, CPU por core y GPUQ de timestamps; build correcto, gate visual pendiente.
Cierre de pruebas solo con Q.
Candidato posterior zona nueva/vieja: evitar entradas de parche JIT vacias y agrupar
restauracion RX de paginas contiguas ya modificadas. Build y 24573 casos de rangos
con gate real de protecciones Windows correctos; mejora FPS y Series pendientes.
Gate manual candidato 340 s, Q y retorno 0, cero errores Render, ocho asserts
BufferQueue previos a T; Compile/bloque ~2,3% menor observado, sin mejora clara
de tirones (nueva28,25 FPS, vieja53,75; muestras distintas). Sin commit.
Perfil siguiente separa fases de emision A64 y registra totales JIT propios de cada T
antes de volcar el log, sin consumir contadores de ventanas. Build correcto; gate pendiente.
Gate desglose PC: nueva23874 bloques/1529ms Compile, recorridas334/363 bloques y~24ms;
Protect42,25% de Compile agregado, instrucciones456ms. T2/3 truncadas~3,9s por
~17k eventos/s; capacidad aumentada131072, build correcto. Foco proteccion/instrucciones.
Candidato CFG preserva metadatos solo en paginas previamente RX, inicializa nuevas
normal y conserva fallback FromApp. Gate desktop /guard:cf+80000 pares correcto;
build UWP correcto, AppContainer/Series y mejora FPS pendientes. Sin commit.
Gate CFG AppContainer315s/Q/retorno0, cero errores Render, fallback0, tres asserts
previos aT; nueva~31FPS/vieja~58,75 y sin ahorro demostrado (Compile/bloque sube).
T completas con131072eventos de capacidad. Candidato no validado para FPS/Series.
Nuevo candidato: perfil A64 persistente+prewarm antes de Run, `jit_prewarm=record/1/0`
(default0), por title/BuildId/core, hash/longitud y RX iniciales validados,64MiB
emitidos/core. Build UWP y gate standalone con Dynarmic real correctos; corrida
de aprendizaje/Q, segunda corrida prewarm/FPS y Series pendientes. Sin commit.
Gate prewarm PC cerrado: aprendizaje guarda695927descriptores, warm prepara464635
y omite231292 por presupuesto. Nueva~27,5->38,5FPS yCompile42,1% menos observado;
recorrida~56,25->47,25FPS. Ambas Q/retorno0/Render0, asserts2/5 previos aT.
No60 sostenidos ni mejora general demostrados; revisar seleccion/cobertura antes
de PPTC host. Detalles en xbox_performance.md; Series pendiente, sin commit.

## Reglas duras
1. **Solo nuestro fork.** Los commits y pushes van únicamente a `origin` (`JulianDr14/eden-xbox`,
   fork de `eden-emulator/mirror`).
   - `eden` (el espejo oficial de Eden) y `upstream` (juanresendiz813/eden-xbox, el fork original, ya
     abandonado) son de **solo lectura**: tienen el push deshabilitado y así deben seguir.
   - `legacy` (`JulianDr14/eden-xbox-legacy`) es el repo anterior, que queda como archivo.
   - **Nunca abras PRs ni issues contra Eden.** En un fork, GitHub y `gh pr create` proponen por
     defecto el repo padre: indica siempre `--repo JulianDr14/eden-xbox` y comprueba el destino.
2. **Commit solo cuando el usuario lo pida.** Nunca por iniciativa propia. Nunca añadir a Codex, OpenAI
   ni al asistente como autor/coautor o mediante un trailer `Co-Authored-By`.
   Los commits del usuario conservan únicamente su identidad; preferencia global explícita.
3. **Nada de keys, firmware, juegos ni binarios de Mesa** (`spirv_to_dxil.dll`) en el repo.
4. **GPLv3:** el código queda abierto y se mantienen las cabeceras SPDX y las atribuciones.

## Sincronizar con Eden
1. El usuario pulsa "Sync fork" en GitHub, sobre `master`. También vale
   `git fetch eden && git push origin eden/master:master`.
2. Se fusiona en la rama de trabajo: `git switch xbox && git merge eden/master`.
3. Se resuelven los conflictos. Los que ya salieron y cómo se resolvieron están en
   `docs/xbox_internal.md`.
4. Se adaptan los runtimes D3D12 a los cambios de API de las cachés genéricas.
5. Se compila y se repite el ciclo de prueba en el PC y en la Series.

## Cómo trabajamos
- **Idioma:** el usuario escribe en español, a veces rápido y con erratas. Responde en español,
  directo y sin relleno.
- **Pruebas de FPS y estabilidad:** usar `play=1`, sin limite de tiempo ni entradas programadas; el usuario prueba gameplay y cierra con Q cuando lo considere suficiente. No usar una sesion corta para certificar rendimiento.
- **Operaciones pesadas, las ejecuta el usuario.** Un build completo, descargas grandes o extraer
  archivos: dale el comando para que lo corra él y vea el progreso. Tú sí puedes hacer builds
  incrementales cortos (unos pocos `.obj` o el link) y las pruebas en el PC.
- **Antes de una acción no obvia, di qué hace y para qué.** El usuario corta los comandos que no
  entiende ("¿pero qué hace?").
- **No pierdas tiempo en lo accesorio.** Si algo no bloquea (por ejemplo, verificar un script
  auxiliar), anótalo como pendiente y sigue con lo importante.
- **Investiga antes de diseñar:** documentación de Microsoft, backends D3D12 de otros emuladores
  (Dolphin, Xenia) y el backend Vulkan de Eden como modelo. Cita las fuentes en el documento de
  diseño.
- **Documenta todo lo que aprendas** en `docs/xbox_internal.md` (hechos, trampas, comandos) y el
  diseño de cada fase en su propio `docs/xbox_*.md`. Si un error costó tiempo, va al documento.
- **Cada fase se parte en sub-fases con un gate verificable.** Cada sub-fase se integra de modo que
  algo visible la ejercite; por ejemplo, el present usa el scheduler antes de que exista el
  rasterizador.
- **El renderer nunca tumba la app:** los fallos se registran en el log y se cae a una ruta más
  simple (por ejemplo, el present por CPU).

## Ciclo de prueba
1. **Compilar** (ver `docs/xbox_internal.md`):
   `tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp`.
   Para cazar errores rápido, compila objetos sueltos con `ninja -C build-uwp <ruta>.cpp.obj`.
2. **Probar en el PC:** `powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild`.
   - Imprime el diag.
   - El log está en `%LOCALAPPDATA%\Packages\EdenEmuProject.EdenXbox_4qge6yz81zw0w\LocalState\eden\log\eden_log.txt`.
3. **Preparar la prueba en consola:**
   - Sube `Version` en `dist/uwp/AppxManifest.xml` (sin BOM; el original no lo lleva).
   - Empaqueta con `tools\xbox\package-appx.ps1 -BootNro tools\xbox\boot_nro\boot.nro`.
   - Archiva `eden-uwp.exe` y `eden-uwp.pdb` en `build-uwp\symbols\<versión>\`.
4. **El usuario instala en la Series:**
   - Instala `build-uwp\package\eden-xbox.appx` por el Device Portal y pone la app en modo **Game**.
   - Después deja `eden_log.txt` y `eden_uwp_diag.txt` en su carpeta **Descargas**
     (`C:\Users\julia\Downloads`) y te dice cómo se vio.
5. **Tú revisas los archivos:**
   - En el diag: `RunHeadlessBoot returned 0`.
   - En el log: las líneas `D3D12: ...`, sin `Critical` ni errores de `Render`.
   - Actualiza la tabla de fases en este archivo y en `docs/xbox_internal.md`.

## Trampas conocidas (resumen; el detalle está en docs/xbox_internal.md)
- **PATH con devkitPro:** desde PowerShell, `cmd`, `tar` y `make` pueden resolver a sus versiones de
  devkitPro. Usa `$env:SystemRoot\System32\cmd.exe` y `...\tar.exe`.
- **Git Bash:** no compiles desde ahí.
- **Nuevo `.cpp`:** añádelo a `src/video_core/CMakeLists.txt`, en el bloque `ENABLE_D3D12`.
- **Mesa:** `spirv_to_dxil` desreferencia siempre `debug_options` y `logger`, así que no pases
  `nullptr`.
- **Descriptor heaps:** `SetDescriptorHeaps` se pierde con cada `Reset()` de la command list.
- **Capacidades de la consola:** FL 11.0, SM 6.4, sin Agility SDK, sin enhanced barriers y sin logic
  op. Nunca uses `GetCachedBlob` (provoca device removed).
- **Mismas versiones:** al re-registrar en el PC la misma versión, se pierde el permiso de lectura
  de la app. `local-run.ps1` ya lo corrige con `icacls`.

## Mapa rápido
- `src/video_core/renderer_d3d12/`: device, swapchain, compilador de shaders, scheduler, staging y
  descriptores, más el renderer.
- `src/audio_core/sink/xaudio2_sink.*`: salida PCM XAudio2 2.9 para Windows UWP/Xbox; diseño y
  gates en `docs/xbox_audio.md`.
- `src/eden_uwp/`: frontend UWP (arranque headless y diagnóstico).
- `tools/xbox/`:
  - `build-env.bat`, `package-appx.ps1`, `local-run.ps1` y `build-spirv-to-dxil.ps1` (este último no
    se ha ejecutado nunca: revisa `tar` y los parches antes de usarlo).
  - `boot_nro/` (el homebrew de prueba).
- `externals/spirv-to-dxil/include/`: headers de Mesa (MIT).
- **Fuera del repo:** `..\mesa-build\`, con las fuentes, el build de Mesa y la DLL.
- **Modelo a imitar:** `src/video_core/renderer_vulkan/` (`vk_scheduler`, `vk_staging_buffer_pool`,
  `vk_buffer_cache`, `vk_texture_cache`, `vk_fence_manager`, `vk_rasterizer`).


Actualizacion candidato FPS (30 sep 2026): prewarm prioriza compilaciones observadas
durante T, conserva perfiles v1 y guarda v2; cupo de observaciones reservado para T,
mismo presupuesto64MiB/core. Contadores T clasifican misses por cobertura, presupuesto,
core/FPCR, codigo distinto o recompilacion. Harness y build incremental UWP correctos;
primera corrida aprende prioridad, segunda valida seleccion/FPS. Gate manual y Series
pendientes; sin commit. Detalle en `docs/xbox_performance.md`.


Gate PC prioridad JIT (30 sep 2026): Q71s, retorno0, dos T completas. Nueva43FPS
con16414 compilaciones:69,75% presupuesto y28,80% perfil solo en otro core;
recorrida59,25FPS/559 compilaciones.16973 registros T guardados en v2 y checksum
verificado. Esta corrida aun cargo v1 sin prioridad; siguiente compara prewarm
priorizado,64MiB/core. Cinco asserts BufferQueue antes de T, Render sin errores.
Arranque tuvo pausa larga compatible con suspension host, causa sin confirmar.
Detalle/evidencia en `docs/xbox_performance.md`; Series pendiente, sin commit.


Gate PC prewarm priorizado (30 sep 2026):16973 prioritarios aceptados sin rechazos,
64MiB/core; Q67s/retorno0, dos T completas. Nueva43->51,5FPS, Compile16414->5672,
p99gap71,290->34,780ms y max200,573->50,043ms; recorrida59,25->58FPS con mas
compilacion y peor p99. Mejora parcial observada, no60 sostenidos ni estabilidad
general certificada.25157 registros prioritarios persistidos/checksum correcto;
Render sin errores, dos asserts BufferQueue antes de T. Cobertura por presupuesto
y perfil en otro core sigue pendiente; siguiente candidato prioridad entre cores
con validacion de codigo. Series pendiente, sin commit; detalle en rendimiento.


Decision usuario memoria: caches por juego, pruebas PC con limite5120MiB (Series
medido,5GiB). local-run.ps1 aplica por defecto Job process-commit cap, verifica
API y conserva limite tras cerrar launcher; MemoryLimitMiB0 opt-out. Frontend
memory_limit_mib refleja presupuesto de caches/diag sin falsear limite OS.
No aumenta prewarm64MiB/core ni cambia memoria guest: medir gameplay primero.
Probe asignacion real rechazado al limite y build UWP correctos; app PC limitada
lanzada, gate manual pendiente. GPU dedicada PC no reproduce RAM unificada Xbox.
Detalles/fuentes/trampa de permisos en xbox_performance.md; sin commit.


Gate PC5120MiB: Q84s/retorno0, T completas38FPS nueva/53 recorrida; maximo
muestreado4653MiB commit con466MiB margen, sin errores Render/asignacion, dos
asserts BufferQueue previos.25157 prioritarios precalentados,64MiB/core. Ventanas
muestran mas recreacion/decodificacion de texturas y uso GPU; posible presion GC
con politica5120, sin thrashing probado ni A/B determinista. Antes de ampliar JIT,
medir evictions/hits y proteger conjunto de trabajo de gameplay. Job PC no equivale
a RAM unificadaSeries. Detalle en xbox_performance.md, sin commit.


Candidato autorizado usuario: prewarm100MiB por core emulado (antes64), hasta
400MiB en cuatro instancias; con core3 sin perfil hasta300MiB. JIT capacidad512MiB
se mantiene, PC cap5120MiB/caches por juego. Build incremental pasa; prueba manual
T/Q, pico memoria, FPS y Series pendientes. Sin commit; detalle en rendimiento.


Gate PC100MiB/core con limite5120: Q64s/retorno0, T completas; nueva38->57,25FPS
yCompile6725->598, recorrida53->59,25FPS/637->251. Maximo muestreado4685MiB,
434MiB margen; Render/asignacion sin errores, cinco asserts BufferQueue antes deT.
Precarga40,4s frente27,9 con64MiB. Presupuesto ya solo45/1 misses; otro core533/249
domina restantes. Mejora parcial manual, no60 sostenidos ni Series certificados.
Mantener100, siguiente foco cobertura entre cores/precarga/GC; sin commit.


Usuario autoriza candidato150MiB de prewarm por core (antes100), PC cap5120MiB
y perfiles por juego. Build incremental correcto; gate manual T/Q y Series
pendientes. Sin commit; evidencia/diseno en xbox_performance.md.


Gate150 PC: Q67s/retorno0, T completas56/57,5FPS frente57,25/59,25 con100.
Techo+50% pero codigo real+5,97% (317,37MiB), bloques+6,11%; p99 empeora22,38/25,75%.
Maximo commit muestreado4814MiB/margen305, precarga43,8s. Perfil completo sin
omitidos por presupuesto, misses otro core/nunca aprendido.100 mejor balance
observado, A/B causal/Series no certificados. Codigo sigue150 autorizado, sin
reversion automatica ni commit; detalle en xbox_performance.md.


Candidato autorizado115MiB/core: comparte perfiles prioritarios entre cores del
mismo juego/BuildId, valida codigo y emite por JIT; perfiles contradictorios no se
comparten. Precarga paralela con un worker por JIT detenido y progreso solo en
coordinador, join antes de Run/fallback. PC cap5120MiB conservado. Harness Dynarmic
real pasa concurrencia/estado/hash y unwind, build incremental correcto. Gate
manual tiempo/RAM/T/Q ySeries pendiente; sin commit, detalle en rendimiento.


Arranque115 compartido/paralelo PC confirmado:4344 bloques de otros cores
aceptados,777750 preparados total, cero rechazados/omitidos, codigo321,14MiB
total(113,64/105,58/101,92). Precarga completa21,9s frente43,8 serial observados
con perfiles distintos; workers21,39s wall. Commit2251MiB tras carga, cap5120
verificado. Gameplay/T/Q, margen/FPS ySeries pendientes, sin commit.


Gate115 compartido/paralelo PC: Q58s/retorno0, T completas56,25/57FPS;
Compile334/20 frente797/140 con150(-58,09%/-85,71%), pero FPS sin mejora general.
Precarga21,9s vs43,8;4344 compartidos aceptados y cero omitidos/rechazados.
Commit maximo muestreado4811MiB/margen308; Render/asignacion sin errores,
tres asserts BufferQueue previos aT. T1miss181otro-core/153nuevos, T2 10/10;
solo compartimos prioritarios. CPU ejecucion/sync/pacing/caches siguiente diagnostico,
no mas presupuesto ni60FPS certificados. Error playtime al cierre registrado,
retorno0; Series pendiente, sin commit. Evidencia en xbox_performance.md.


Commit7eeb775e7 guarda perfilesJIT/prewarm115 compartido/paralelo y diagnosticos
previos. Siguiente candidatoT: RunThread>=200us porguest/core, dispatchscheduler,
GCpresion/evictions/creacion y cargastextura>=200us. SoloT, misma politicaLRU/JIT.
Analizador de intervalos largos recorta/une solapes; no confundir elapsed con CPUbusy
ni correlacion con causalidad. Fixtures y compatibilidad logsprevios correctos;
build/gateTcapacidad/manual/Series pendientes; detalle en xbox_performance.md.

Gate build del diagnosticoT: incremental UWP pasa (incluye objetos D3D12/Vulkan
por header comun). Analizador pasa fixtures y dosTprevias. Prueba manual lanzada
con115/core,play1,fastmem0,cpu_profile1,jit_prewarm1; Job5120 verificadoPID3436.
PendienteT completas/capacidad, overhead y lectura del cuello restante; Series
pendiente. Instrumentacion posterior al commit7eeb775e7 queda sin commit.


Usuario ampliaT a8s: frontend arma480vsyncs. Nuevos eventos llenaron131072 entradas
 a3,337/3,094s (~39-42k/s); ambas T truncadas, no comparacion FPS. Evidencia archivada
pc-frame-stalls-truncated{,-diag}.txt. Proceso ausente, diag sinQ/retorno confirmado.
Capacidad pasa524288 (~16MiB,+12MiB respecto131072), almacenamiento fijo y sin
alloc duranteT; mantiene dump fuera de medicion. HUD calcula tiempo porvsync restante
sin constantes4s. Analizador usa480 por defecto, --vsyncs240 para logsanteriores;
FPS solo cuando ventana completa, rechaza interpretar truncada como8s completa.
Incremental/manual por registrar; cambios posteriores7eeb775e7 sin commit.

Gate8s: build incremental UWP correcto(5operaciones); analizador pasa captura480
completa, compatibilidad240, unionrecortada y rechaza FPS de ambasTtruncadas reales.
Relanzado manual conplay1/115MiBcore/fastmem0/prewarm1, Job5120 verificadoPID18888.
Capturas480/HUD/capacidad ySeries pendientes; cierre usuarioQ, sin timeout gameplay.


GateT8PC: Q66s/retorno0,480vsyncs completas303166/331495 eventos. FPS52,625/58,375,
p99gap41,476/32,684,max144,634/43,036ms. GC315/311evictions,206/235recreaciones
mismaaddr; proxy3004--3238MiB siempre sobrecritical2758. Gaps41/34ms coinciden
GC30/20ms; peor145ms incluyeRun105,5ms guest125/core0 yGPUidle119ms (causaCPU
no separada). JIT4433/1501Compile,418,6/107ms; no missesbudget. Commitmax4873,
margen246; RenderError/Critical0,8unmappedDeviceReadBlock antesT. Dump provoca
pausa3,79/4,14s fueraT: corregir guardado antesde evaluar estabilidadvisual;
GC/presion yRun siguiente foco. Sin60sostenidos/Series, sin nuevo commit.


CandidatoGC autorizado: guardado debug fueraalcance. PoolD3D12 trimheapstotalmente
vacios trasfence, conserva1warm/clase normal yliberatodosbajopresion, tombstones
indicesestables. TextureGC usaheadroomapp/DXGIactualconhisteresis, age120normal/
60critico/10emergencia, trabajoincremental1ms salvoemergency; downloadindividual
puedeexceder1ms. BufferGC/Vulkanfallbackoriginal. T8desglosaRunlargoCompile yCPU
flushthreadlocal; noCPUbusy. TestsMSVCpolicy/harnessDynarmicrealTLS/parser pasan,
buildUWPcorrecto; manualchurn/RAM/FPS/Seriespendientes, sincommit. Detallefuentes
encuaderno rendimiento; 115/core y5120proceso mantenidos.

PruebaPC candidataGC/Runlargo lanzadaPID19476, Job5120MiB verificado,play1,
fastmem0,cpu_profile1,jit_prewarm1,115/core,T480. Sinlimitegameplay; usuarioQ.
Arranque/precargaenprogreso, validarTchurn/maxGC/trimreal/headroomyCPUdetalle;
no concluirmejoraFPSporbuild. Sincommit.

Nota arranque: LruCache.ForEachItemBelow incluye tickigual al corte; clamp0 admitia
imagestick0 antesdeedad minima. Fuente ahora omitepasada si frame_tick<age (sin
unsignedwrap ni expulsarfirstframe). TrialPID19476 ya lanzado con clamp0 previo;
susTgameplay despues120frames ejercenmisma politica steady-state. Correccion de
arranque pendiente siguiente link/lanzamiento, no certificar gateearlyaging con
ese proceso. No interrumpir corrida manual para substituirbinario.


GatecandidatoGC: Q94s/retorno0,T8completas53,875/50,25FPS. No mejora validada:
evictions395/429,recreaciones258/250; levelEmergency todaT,margen120,66--162,37/
134,55--146,40MiB. Trim0,pool512MiB peak416(fragmentacion/ocupacionporinvestigar).
Run34,431ms contieneFlush34,402 ycoincideGC35,796; Run33,929 contieneCompile33,223:
CPUflush yJIT son ambosfocos. Parserempatestimestamp corregido ordenestable yfixture
pasa. RenderError0,4BQassertantesT. Guardagingarranque buildincremental pasa trasQ;
no relanzado,sincommit. Guardadodebug fueraalcance; siguienteGCpacking/descargas
/margenreal,115/core y5120 intactos,Series y60sostenidos pendientes.

Investigación packing/readback contrastada con Vulkan en docs/xbox_texture_memory.md:
VMA/budget/async flush son modelos útiles, pero GC GPU-dirty sigue síncrono en
ambos backends y Vulkan no activa defrag. Usuario autoriza ring upload256MiB,
cutoff32MiB y16regiones conservados; JIT115/core,Job5120,T8 intactos. Build
incremental correcto; prueba manual ySeries pendientes, mejora no validada,
sin nuevo commit. Guardado de trazas debug fuera del alcance.

Gate staging256 PC: Q74s/retorno0,T480 completas58/59FPS,p99 33,615/29,294ms;
mejora observada pero Compile1261/258 frente3171/5447 yescenas/perfiles distintos
impiden atribuirla a staging. Margen mínimo71,945/107,301MiB,RenderError0,
5BQasserts antesT. Pooltrim2heaps/128MiB confirmado; gapT2 40,081ms conGC23,544.
Mantener256 autorizado,115/core y5120. ReadbackGC/packing próximos candidatos;
60sostenidos/Series pendientes,sin nuevo commit. Evidencia en rendimiento.

Candidato autorizadoGC diferido/best-fit: readbackmaintenance conFence yversiones
GPU/CPUvalidadas, cap8MiBpinned, recovery síncronoappfree<64MiB/copiasgrandes.
Poolbase64MiB, menorhuecocompatible yheapsgrandesalineados sinpotenciadedos.
BuildUWP y1.248.000 casospacking pasan; gateGPU AppContainerdebug pasa moves,
datos/stale/cap/descarte/emergency,RenderError0/retorno0,pending0/peak8MiB.
T incluyeheapfree/reserved/pending yestadosreadback. Staging256,JIT115/core,
Job5120,T8 mantenidos; gateFPS/memoriagameplay ySeries pendientes,sin commit.

GateGCdiferido/best-fit PC: Q67s/retorno0,T480 completas58,25/57FPS, sin mejora
general frente58/59. GCmax9,312/6,812ms(-48,73/-71,07% observado); Tqueued/ready
11/11 y6/6,sync/stale0. Margen98,617/126,891MiB,RenderError0,8BQassertantesT.
Heap384MiB/libre118–170/mayorhueco50,875,trim64MiB; packingventajano certificada.
GapsrestantesGPUidle27–30ms/uploads/Run sinCompile/Flush. Desglosarguest/esperas
siguiente; staging256/JIT115/core/Job5120/T8conservados. Sin commit,Seriespendiente.

DesgloseCPU implementado: TPCfinal/stop/SVC, clocks/icache ymemorycallbackcounts;
sleep/locks/condition/address trazados. CPUhostventanas100ms GetThreadTimesUWP,
sin atribuirCPU porRun: harnessobserva15,625mscuantización/query551ns. Fixtures
migración/unavailable/endpoint/oldlog pasan; buildincremental pasa. GateTmanual,
overhead/capacidad ySeries pendientes; 256staging/115core/5120/T8,sin commit.


Corrección capacidad T CPU/SVC (30 sep 2026): Q131s/retorno0; primera T llena
524288 a5,382s/323vsyncs, segunda llena a~5,2s y volcado solo261vsyncs por
límite100MiB del logger (archivo105830342bytes). No comparar FPS de estasT.
Evidencia pc-cpu-run-detail-truncated{,-diag}.txt. Sleep/locks/address dominan
nuevos eventos (126492 comienzos enT1). Se conservan edgesIPC/WaitSynchronization;
SleepThread/ArbitrateLock/WaitProcessWideKeyAtomic/WaitForAddress se emiten como
un único guest-svc-long al terminar>=200us, mismo captureID yguestoriginal aunque
migre dehost. FueraT/sinSVCseleccionado no reloj ni eventos. Spans menores200us o
cruzando bordes omitidos explícitamente; solape no prueba causalidad. Memoriafija
524288(~16MiB),T480,staging256,JIT115/core,Job5120 intactos. Replay aproximadoT1:
282195eventos frente524288,proyección8s419496 (no garantía de carga futura).
Parser fixtures migratedIPC/longlock/border pasan; incrementalUWP22operaciones y
gitdiffcheck pasan. Gate manual completitud deambasT ySeries pendientes,sincommit.


GateTCPUcompactaPC: Q72s/retorno0,T480 completas415931/420795eventos.
FPS58,625/56,375,p99gap33,377/33,540ms; Compile10,954/34,245ms no dominante.
CPUhostpromedio41--52% porcore OSuser+kernel, no guest puro/sin saturacion global
probada. Run17,797ms PC0x81239648 coincideuploadsOTROhilo16,731ms; otrasgaps
hiloGPUesperatrabajo23--28ms,IPCguest83~32--36ms causa sin confirmar.
Prioridades: fases refresh/upload; servicio/comandoIPC ycadena de wakeups;
flushpuntual10,494ms. ReadbacksT9/9 y7/7 sinfallback, margen84/119MiB,
RenderError0,5BQasserts antesT. 256/115/5120/T8 mantenidos, sincommit/Series/60.
Evidencia y límites en xbox_performance.md.


Diagnósticoupload/IPCimplementado: Tstaging/read/deswizzle/convert/backend/repack
>=200us,bytes/formato; service/command/type/guest ymutex/handler, nombres24bytes
truncamientoexplícito. ParserasociaSVCguestmigrado, no confundir handlerdeferred
concompletion; repackcontenidoenbackend. Runcontadores0usomitidos preservan
capacidad16MiB/T8 sinRAMextra. Build23+4ops/fixtures/oldlogs pasan; manual/Series
pendientes. Staging256/prewarm115core/Job5120 intactos, sincommit.


Gateupload/IPCPC: Q76s/retorno0,T480 completas397947/409314events,FPS55,25/
58,375,maxgap41,439/61,397ms. BC5_UNORM/SNORM arrays dominan conversion3--3,6ms
porimagen+backend2--4,9ms; optimizar decodeCPU/upload preservandofallbackSeries.
IPCguest83IHOSBinderDrivercmd3 TransactParcelAuto: handler32,332/36,197ms incluye
32,319/36,185ms DequeueWait, espera framebufferlibre, noCPUcomputa30ms. Otra
ruta peor61ms incluyeguestWaitForAddress52ms, causaporconfirmar. SinmutexIPC>=200us,
margen55/90MiB,GCsync1T1(<64MiB),RenderError0/BQassert0. Mantener256/115/5120/T8,
Series/60sostenidospendientes, sincommit; evidenciaylimitesenrendimiento.


CandidatoBC4/5CPU: decoderbloques4x4 completos con storefila4/8bytes e interleaveRG,
UNORM/SNORM especializados; mipparciales fallbackanterior, arraysR8/RG8Series
preservados. SinRAMextra/ISA específica.262144endpointcases+108image/pitch/
unalignment casosbytesidénticos, BC5aislado2,46/2,39x, incrementalUWP4ops pasa.
GameplayT/Q/FPS/visual/Series pendientes; 256/115/5120/T8,sincommit. Fuentes y
trampaStoreCRT harnessdesktop enrendimiento; nadaenviadoEden.


GateBC45packedPC: Q75s/retorno0,T480 completas399390/414750events,FPS58,875/
59,25 frente55,25/58,375;p99gap30,883/25,898,max41,840/33,014ms.
BC5conversion1,475--2,415ms frente~3--3,6; ahorro coherente conharness2,4x,
peroJIT22/30 ychurnmenor impidenatribuir todoFPS. SinRAMextra, margen90/117MiB,
RenderError0/BQassert0,readbacks4/4 y6/6 sinfallbackT. Cuellosrestantes gaps38/
31ms upload/GC0 yGPUthreadespera29/25ms conBinderdequeue36,8/30,2: siguiente
cadenaBufferQueue/VSync/composición/release. 256/115/5120/T8,sincommit; visual/
Series/ABy60sostenidospendientes. Evidencia enrendimiento.


Commitb7aa63d14 solicitado: GC/staging/diagnósticoT8+BC45packed. BufferQueue:
logsdemuestranhold2 solicitado porguest,vsyncticks puntuales yrelease->dequeue37/
43us enejemplos38/31msgap; solicitudesinterval2bajan34->5/7->3trasBC45.
Siguiente diagnósticoleasereleaseporconsumer/frame/ticks ycallbackCoreTiminglate,
Composecontainerlock/elapsed yDXGI Present. Preserva sincronización/ordenSet->
GetNextTicks/intervalos. Fixturesmulticonsumer/hold2/extra/present/oldlogs pasan,
build26+26ops correcto; T8manual/Seriespendientes. 256/115/5120sinRAMextra.
Cambios posteriorescommitsincomitear; docfuentes/limitaciones enrendimiento.


GatePCframelease: Q89s/retorno0,T480 completas394720/395684,FPS59,125/59,375.
Ticklate max0,509/0,539ms, cero leases con ticks extra, Presentmax0,224/0,252ms;
no bloqueo largo del compositor demostrado. T1maxgap39,181ms: Dequeue termina
y siguienteQueue llega23,961ms después; WaitForAddress guest83 dura23,359ms en
ese tramo. T2max33,702ms coincideleaseinterval2 legal. Siguiente foco cadena
WaitForAddress/wakeups delproductor, no liberacióntemprana. RenderError0,8BQasserts
antesT; margen83/111MiB, readbacks4/4 y8/8 sinfallbackT. Diagnóstico sincommit,
Series/60sostenidos pendientes; staging256/JIT115/core/Job5120/T8 conservados.
Detalle/evidencia en xbox_performance.md.


Candidato1oct: diagnósticoT concentrado en guest83, scheduler/Run/SVC/IPC otros
guests ydetalleCPU/upload/packing/Present/tick desactivados; autotestsboot opt-infalse.
HUD/T8/framechain/fences/elapseduploadGC/headroom y errores conservados. Árbitro
registra address/observed/expected/type/timeout real, sourceguest enEndWait, cancel
yresult alresume mismacapture; sinreads guestextra nisemántica modificada. Parser
separa wake->dispatch/resume ybordes, fixtures yoldlogs pasan, build27+4ops pasa;
manual/Series pendientes. Próxima corrida sincpu_profile1 (default0), prewarm115
funcional/staging256/Job5120/T8 mantenidos. Sincommit; detalle enrendimiento.

Gatebuild final scheduler4ops pasa; gitdiffchecklimpio. Trialmanual concentrado
lanzadoPID11468, Job5120MiB verificado, play1/fastmem0/jit_prewarm1; cpu_profile
omitido(defaultfalse), sin timeout de gameplay ni entradas programadas. Arranque
shadercache yprewarm enprogreso. T8 completas/overhead/dirección/waker/Series
pendientes; cierre usuarioQ. Sincommit.


GatePCfocus83: Q71s/retorno0,T480 completas27831/27911 (eventos-92,95%),
FPS58,75/58,75; no mejoraFPSdemostrada.940waits address0x210a010120, value1/
expected1 WaitIfEqual indefinido, todasSignalID79/Result0. Wake->resume max33/70us,
esperasmáximas27,426/26,789ms ocurrenantesSignal; no schedulerpostwake largo.
Host79 distintoGPU, rolpendiente. Mayoreswaits sinupload/GC largo, otrasgaps
correlacionanuploads/GC; siguiente seguirPC/Run/IPC/esperasdel79 ySignalargs.
RenderError0,4BQassertantesT; margen58/74MiB, métricasapagadas desconocidas.
256/115/5120/T8 intactos, Series/60pendientes, sincommitnuevo; detalle rendimiento.


Candidato79chain1oct: TracksGuest79/83 solamente, PC/LRentradaSVC yargs/Result
SignalToAddress válidoconwakeefectivo;waits79 registranquiénlodespierta. Parser
recorta/unificaRun>=200us/esperasdel79 durantewait83 sinetiquetarCPUbusy. Fixtures
83<-79<-77, metadata/signed/bordes yoldlogs pasan; incremental28ops pasa.
CPU/JITdetalle0 yautotestsbootfalse conservados;256/115/5120/T8 intactos.
ManualT/PCsource/overhead/capacidad/Seriespendientes, sincommit niFPScertificados.

Trial79chain lanzadoPID10568, Job5120MiB verificado;play1/fastmem0/prewarm1,
CPUprofile omitido0. Shadercache/prewarm enprogreso; usuarioT8 yQ sinlimitetime
ni entradasprogramadas. Gate79sourcePC/args/Run/esperas/volumenpendiente. Sincommit.


Gate79chain: Q123s/retorno0,T480completas253950/245393,FPS55,375/52,75.
83wait37ms incluye79WaitSynchronization36,119 ysyncpoint1wait36,110; otraT
syncpoint1wait31,411. Otraswait79 despiertan123/124/125 (21,368 y6,341ms): dos
familiasGPUevent/CPUworkers. SourcePCviejo inválido:ExitContext solo guarda con
debugger, ahoraGetContextliveantesSVC,nombreslivepc/lr yparserdescartaantiguos.
Fixtures/build6ops/diffcheck pasan, manualcorrecciónpendiente. RenderError0,6BQ
assertantesT,margen50/74MiB;noFPSmejora. SiguientecompletionFence/asyncflush/event
ID yworkers, no asumirsinpolling(HAS_ASYNC_CHECKtrue).256/115/5120/T8 conservados,
sincommit/Series/60pendientes;detalle rendimiento.


Candidato1oct completionchain: fencequeued/dequeued/wait/flushlock/texture/buffer/
query/callbacks enlazadoscontickD3D12; WaitSubmission separadoGPUeventwait.
WaitSynchronization79/83 objetoexacto conNVevent syncpoint/value ybatchfence;
sourceworkers123--125 sóloRun>=200us/longSVC, sinedgesglobales. MetadataPCcorta
sleep/lock/address desactivada, PCvivoIPC/signalretenido. Fixtureslifetime/object
incorrecto/nesting/oldlogs pasan, build38ops pasa,diffchecklimpio. Manualchain/
volumen/overhead/Series pendientes, sincommit/FPSmejora;256/115/5120/T8 intactos.

Trialcompletionchain lanzadoPID19100, Job5120MiB verificado;play1/fastmem0/
prewarm1 yCPUprofile0. Shadercache/prewarm enprogreso. UsuarioT8manual yQ, sin
timeoutgameplay/entradasprogramadas. Chain/volumen/overhead/Series pendientes.


Gate PC completion-object (1 oct): Q139s/retorno0; T1/T2 completas181785/186445 eventos, FPS56,25/57,875, p99gap39,721/32,416,max96,803/51,917ms; T3 accidental excluida. Objeto kernel confirma316/316 y263/263 waits79 ligados a NV syncpoint1. T1wait83 93,193ms incluye wait79 84,658:69,548ms antesenqueuefence+14,918ms esperaD3D12host; pipelinebuilt termina7,841ms antesenqueue y ventana perf registra3stalls/101,5ms, candidatoWaitBuilt sincausalidadindividualconfirmada. T2wait79address33,381ms wake125; Run123~35ms elapsed, noCPUbusy. Wake83mediana5us; flushmax0,901/1,049ms, nocuello largo. Margen83/103MiB,commitmuestreado5031MiB; RenderError0,5BQassertantesT, leasesextraticks0. SiguienteWaitBuilt/DXIL/PSO ycadena workers.256/115/5120/T8, sincommit; Series/60pendientes. Detalleenxbox_performance.md.


Candidato1oct pipelines: DXIL firmado enlazado reutilizado por inputSPIRV completo+opciones/stages/longitudes, por juego, LRU4MiB contabilizados/max128; concurrentes compartencompilacion sinmutex duranteMesa/firma, failedretry/bypassoversize preservados. PSOs siguenindependientes, noGetCachedBlob/noskipdrawspequenos. PublicacionPSO IsBuilt release/acquire. Tfrontend/worker/DXIL/Mesa/validatorlock/sign/PSO/WaitBuilt+cacheoutcome porpipeline, noCPUglobal/norelojfueraT; fasesanidadas nosumar. Harness16concurrentes/collision/content/options/retry/1000evictions/LRU yparserlifetime/borders/nesting pasan, build44ops UWP correcto. Gameplayhits/WaitBuilt/FPS/Series pendientes, sincommit;256/115/5120/T8 conservados. InvestigacionMicrosoft/Dolphin/Vulkan ylimites en rendimiento.


Gate final candidato shaders: DXIL immutable compartido por shared_ptr entre PSOs, sin copias en hits; consumidores sobreviven a eviction. Harness actualizado valida eviction real y bypass al llenarse cupo de compilaciones en vuelo. Build10ops+4ops final correcto; parser pipeline/address y regresionT1/T2 previa correctos, gitdiffcheck limpio. Correccion include frame_trace omitido al ordenar includes: link final recompilado, no afecta corrida anterior. Trial manual lanzadoPID14184, Job5120MiB verificado;play1/fastmem0/prewarm1/CPUprofile0,115core/staging256/T480. T1/T2 hits/esperas/FPS/memoria ySeries pendientes; usuarioQ, sinlimitegameplay, sincommit.


GatePC DXILcompartido: Q76s/retorno0; T480completas182760/175587events,FPS56,5/55,p9939,721->41,761 y32,416->40,624,max56,828/48,932ms. Sinmejorageneral.1718PSOsprecargados; cero creacion/WaitBuilt/cacheoutcome enT, ahorroDXIL/hits no medidos. Margen25,918/61,996MiB,GCmax19,251/23,958ms; gapT2GC18,653 conappfree62,070<64MiB compatible recoverysincrono (contadorreadbackoff, no conteo confirmado). OtragapT1WaitD3D12async43,212ms. RenderError0,5BQassert/8unmappedantesT. Parser corrigeforegroundwaitmismotick: host/dequeue necesarios parafaseanidada, fixturepasa. Siguiente margen/GCsync, despuesD3D12/CPUworkers.256/115/5120/T8 intactos, sincommit/Series/60pendientes; detalle enrendimiento.


Candidato guard memoria1oct: app-free<=128/64/10MiB reduce staging128/64/0; Finish solo emergencia con ring vivo, fence retirement sin solapar rings. Recupera64 tras120 frames>=256MiB; presupuestos reducidos no regrow256. DXILtrim0/pinned readbacks preservados; staging sweep acotado yswap/pop porIDs estables, heaps vacios/GC existentes. Reutiliza muestra memoriaframe, sinthread/reloj normal; JIT115/core yJob5120 intactos. Harnesspolicy1Mframes/10000buffers yDXILinflight pasan; buildfinal/gate manual pendientes,sincommit. Detalle/fuentes/limites enrendimiento.


Gate guard memoria1oct: build incremental UWP final pasa15 operaciones, gitdiffcheck limpio. Harness policy/bounded staging yDXIL trimming pasan. Prueba manual lanzadaPID11496, Job5120MiB verificado,play1/fastmem0/jit_prewarm1/CPUprofile0,T480 yJIT115core. No limite de gameplay; usuarioT/Q. Reclamacionreal/coste/FPS/Series pendientes, sincommit.


GatePC memoryguard: Q231s/retorno0,T480 completas181034/186029. Guardactua127,969MiBfree antesT: ring256 retiradoy128 repuesto, capacidad-128MiB real. MargenT93,344/136,520 vs25,918/61,996; FPS56,5/58,25 vs56,5/55,p9938,598/30,355, max63,871/40,592. T1GC43,977ms empeora peortiron, noestabilidadgeneral ni causalidadFPS. Commitmuestreado4986MiBmax,RenderError0,1BQassert+4unmappedantesT. Rama10MiB/Finish/recovery64/overhead/Series pendientes. SiguienteGC largo yD3D12waits;115core/5120/T8conservados,sincommit. Evidencia enrendimiento.


Cierre autorizado usuario1oct: pendientes porprioridad documentados en xbox_performance.md: GC/readback43,977ms; cadena D3D12/CPUworkers fueraGC; overhead/ramas64/10/recovery delguard; gateSeries/admissioncontrol/eventoslimite/JITdecommit real. No iniciar nuevo candidato hasta siguiente indicacion.


ComparacionVulkanPC autorizada1oct: UWPsoloCoreWindow, VulkanWindows requiereHWND; no exe desktop existente. Preparados build-vulkan-pc.bat (buildcompleto usuario yautolaunch) yvulkan-run.ps1 (Job5120/T8/Q/fastmem0/prewarm115/mismosdatos, logsseparados). FrontendSDLcorrigeestado/AppResult/entrada lifetime/callback dangling, agregaManualContentProvider ystatusT/FPS/commit. /ZsMSVC yparserPowerShell pasan; configure/link/gameplay pendientes. LoaderPCpresente; diferenciasAppContainer/GC/audio/VRAM documentadas, noA/Bcausalcertificado. Sincommit;detalle rendimiento.


BuildVulkanPC: corregido PATH devkitPro/MSYS2, CMake/Ninja nativos VS explicitos y --fresh solo cacheMSYS. Probe ABI C/C++/link/ejecucion pasa; fullbuild/manual pendientes, sincommit. Detalle cuaderno/rendimiento.


VulkanPC enlace corregido SDL3Unicode/CRT, launcher eden-cli yerrornegativo delbatch. Incremental7ops pasa; lanzadoPID18060 Job5120,Render.Vulkan/cargaperfiles confirmados. T8/manual/Q/FPS pendientes,sincommit. Detalle rendimiento.


CandidatoGC44ms1oct: Emergencypreventivo1ms/1intento; recovery<64MiB/GPUbudget agotado conservaunbounded. Tfases/readbackreason/headroom, pinned8MiB/115JIT/guardstaging intactos. IncrementalUWP/harnesspolicy/parserpasan; lanzadoPID2920 Job5120 play1/prewarm1/cpuprofile0/T480,Qmanual. FPS/GC/margen/Seriespendientes,sincommit;detalle rendimiento.


GateGCacotadoPC: Q632s/retorno0,T480completas188588/183709;FPS58,25/57,p9934,332/35,337,GCmax5,588/17,390vs43,977/24,235. Sinmejorageneral. T2copyrecord16,703ms/staging0,677,formats21 B10G11R11_FLOAT; GCwait0/sync0toda corrida, pinnedpeak8MiB. Margen91,797/126,406,RenderError0,5BQassert/8unmappedantesT. Siguiente temporalesDEFAULT/footprints/record, despuesD3D12fences. Churn no medido(createoff),Series/60 pendientes,sincommit; evidencia rendimiento.


CandidatoGCfootprints1oct: directtexture->READBACK porfullsubresource, compactionin-placepostfence/Mapcoherencia, sinDEFAULTtemporal/rowGPUcopies enfastpath. Capreal8MiB incluyepadding/bitceil, formatos/layouts noaptosfallback. CPU20kcasos yGPUdebugB10arrays/mips/RGBA8volume/BC1native/stale/move/cap/discard/emergency pasan,RenderError0/retorno0. Bootselftestsfalse/buildnormal4ops correcto. TrialPID24052 Job5120 play1/115JIT/prewarm1/CPUprofile0/T480/Qmanual; FPS/GC/margen/Seriespendientes,sincommit.

GatefootprintsPC: Q82s/retorno0,T480completas180353/189541. Fastpath210/5,
GCmax3,041/1,111ms(-45,58/-93,61% vsanterior), compactmax0,947ms, copyrecord
sin eventos>=200us. FPS55,5/59 vs58,25/57, mejora general no validada. Margen
116,121/155,723MiB; guard staging256→128, commitmaxmuestreado4978MiB. RenderError0,
5BQassert/31unmapped antesT; sync/stale0,pending0 al cierre. Peoresgaps48,509/
39,626ms conGC0,572/0ms: siguiente fence/flush/worker yentregaframes. 115/5120/T8
conservados,Series/60sostenidospendientes,sincommit;detalle rendimiento.

Plan pendiente1oct documentado: P0 BQassert/unmapped previosT; P1 fences/flush/
worker yentrega frames con dependencia concreta; P2 churn/fragmentación/swizzle;
P3 CPU residual/coberturaJIT/precarga. SeriesUMA/visual/guard/fallbacks y60FPS
sostenidos pendientes; Vulkan compila/arranca, comparación gameplay pendiente.
Mantener115/core/Job5120/T8 ydiagnóstico selectivo. Criterios de cierre y evidencia
en docs/xbox_performance.md, Prioridades pendientes después del GC directo.

Frontend biblioteca1oct: `-Library` en local-run/package o`library=1`, raízLocalState/games
compartidaPC/Series; scannerNSP/XCI/NRO/nesting sinabrir contenedores, cap10000/depth5.
UI temporalD3D11/D2D/DWrite destruida antesguestD3D12, inputGamepad+teclado.
Ajustesbásicos letra/posición ydeadzone persistidosLocalSettings. Buildincremental/
harnessscanner correctos; PCPID8952 Job5120 UIvisible confirmada(36MiBreposo),
usuario sinmando. Gate transición/gameplay/persistencia/mando/Series pendientes,
juego pesado aún en dump. Doc docs/xbox_frontend.md, sincommit.

Rediseño frontend: usuario rechazó lista básica; carátulas/títulos con loadersEden,
tarjetadestacada+fila5/focoTV/panelF1-View/ratón. Metadataworker sinDRAM/JITguest,
cacheartacotada/WIC256. Build4ops/scanner pasan; PCPID20692 títuloWonder/icon93583B,
44MiBreposo/Job5120, selecciónmanual→D3D12→Load0. Proceso luegoausente durante
cache sinQ/retorno confirmado; visualrediseño/persistencia/mando/gameplay/Series
pendientes. Docs/xbox_frontend.md ypc-library-cards evidencia; sincommit.

Usuario confirma UIrediseñada/carátula/título. LogoEden originalPNG ahoraincluido
enAssets/EdenLogo.png porpackager; carátulasrounded12 conmaskD2D antialiascacheada.
Build3ops/parserPS/diffcheckpasan; visualajuste/panel/mando/Series pendientes.
Sincommit; docs/xbox_frontend.md conservaestado yfuentes.

UIprompts1oct: detecciónWindows.Xbox víaAnalyticsInfo, ayudas exclusivasmandoSeries;
PC ayudas teclado, sinleyendas mezcladas. KenneyInputPrompts1.5A CC0/subset11PNG
enAssets/InputPrompts conlicense/README, cached porcanvas. Build3ops/diffcheck pasan,
visualPC ySeries pendientes,sincommit; fuentes docs/xbox_frontend.md.

SelectorPC jugador1: automático/teclado/mando porID persistido; catálogo Windows
raw+Gamepad estándar compartido con gameplay, hotplug500ms. Series conservaauto.
Raw sinmapeo aparece deshabilitado; no soporte arbitrarioBluetooth ni multijugador
certificado. Frontend separado navegación/canvas/entrada, metadata porpágina sin
recorrerbiblioteca cada16ms, assetshelper/caches. Harnessselección pasa ybuild
incremental pasa; últimoajuste/visual/hardware/Series pendientes. Sincommit;
detalle yfuentes en docs/xbox_frontend.md.

Gate selectorPC: build finalincremental3ops/harness selección ybiblioteca PASS,
diffcheck limpio. Biblioteca abiertaPID15052 Job5120 verificado, sin cierre
observado. Panelvisual/mandofísico/Series pendientes. First-chance0x6d9 a8s,
procesoactivo; causasinconfirmar. C++/WinRT Path requiereincludeStorageexplícito.
Sincommit; fuentes/diseño/evidencia en xbox_frontend.md.

TecladoPC implementado: editor click/captura4s/Esc/borrar/restaurar, guardado
ParamPackage v1 poracción. Reutiliza Keyboard/InputFactory/GenerateKeyboardParam;
callbacks->máscaraatómica->virtualpad, no polling28bindings. Ambossticks/cruceta/
triggers/clicks/home/captura, foco reset. Q/T defaultlibres, developer_hotkeys1opt-in;
local-run directo conservaopt-in/Qmanual, Library/release no. GateAppContainer
selftestreal PASS PID13988 ydesktopbinding PASS/build7ops. Usuario rechazódibujo
inicial; rediseño usa contornos/posicionesoriginales QtPro, pathsD2Dcached,
3grupos/2columnas/keycaps/centrado. Build7ops/harness3pagescorrectos; linkfinal,
visual/persistenciareinicio/gameplay/Series pendientes. Sincommit; xbox_frontend.md.

Gate rediseño final: linkincremental3ops correcto trascorregir cachealign/font.
RelanzadoPID12596 Job5120verificado ykeyboardselftestAppContainer PASS; no inputs
programados nigameplaypor tiempo. Previewvisual final ypersistencia/manual
pendientes delusuario, appse dejaabierta. Sincommit.

Usuario confirma que cerró manualmentePID12596; no atribuir ausencia de proceso
acrash. Reporta aviso de guardadoMando encimaJugar: bugestado compartido notice.
Corregido separando avisos biblioteca/controller/keyboard; confirmaciones controles
solo en panelrespectivo, caducan3s; errorespersisten mientraspanelabierto, limpieza
al cerrar/cambiarcaptura. Sin recorrido/alloc adicional porframe salvoclearalexpirar.
No crear tests que reflejen solo etiquetas; buildincremental yvisual son gate.
RediseñoPro permanece, sincommit.

Gateavisos: incremental7ops correcto, diffchecklimpio. Relanzado biblioteca
PID23500, Job5120verificado ykeyboardselftestPASS. Avisos fuera delpanel ycaducidad
visual pendientes; se dejaappabierta, cierremanualusuario. Sincommit.

ProUSB1oct: Nintendo057e2009 RawGameController no mapea correctamente informes30.
HidDevice ReadWrite=null peseAllowed, Read abre/200report64B por2s confirmado.
AdapterPC asíncrono/eventos reutilizaJoyconPoller, límitespaquete ytimeoutneutro250ms,
IDHID compartidoUI/gameplay, XboxGamepad intacto. Nominalcalibration, sinoutput/gyro/
Bluetooth certificado. Build/selección/selftestAppContainer PASS; hardwaredecoder
ready1/buttons0/axes~0.001724. PID17396 biblioteca/Job5120, selección/navegación
manual/gameplay/replug/Series pendientes; manifiesto0.2.70.0/HIDcap. Sincommit;
detalle/fuentes ytrampaReadWrite en docs/xbox_frontend.md.

GateProUSB: usuario confirma selección y navegación cruceta/A/B. Prompts ahora
siguen dispositivo elegido (Nintendo A/B/-/+/cruceta, XboxGamepad oTeclado),
Series siempreXbox; formatosD2Dcached/noassets nuevos. Buildfinal3ops/diffcheck
pasan, PID16884 bibliotecaJob5120 sinpro_hid_probe. Visualprompts/gameplay/replug/
Series pendientes,sincommit; detalle xbox_frontend.md.


Corrección editor1oct: lectura de mando continúa en modal teclado, actualizando
edges; solo B produce Volver mientras está abierto, incluso durante captura.
La misma rama de cierre cancela captura y consume Quit, evitando cerrar biblioteca
con ese mismo edge. A/cruceta/otros botones no se traducen a teclas ni acciones
de biblioteca dentro del editor. Ayuda B/Volver visible cuando hay mando activo.
Cruceta roja era el PNG Kenney de color, no indicador de error: reemplazada por
glyph D2D blanco compartido para Nintendo/Xbox, sin assets/alloc nuevos.
Buildincremental pasa; comprobación manual B/abierto/captura y visual pendientes.
Sincommit.


Stick navegación1oct: biblioteca y menús aceptan stick izquierdo del mando
seleccionado (Pro USB/Gamepad PC/Series). Helper puro StickNavigation, sinalloc:
entrada>=0.55/salida<0.35, eje dominante con histéresis diagonal, primer paso
inmediato, repetición tras350ms ycada120ms; no catch-up trasstall. Cruceta tiene
prioridad. Cambio de dispositivo/contexto/errores requieren recentrar antes
de nuevo movimiento; editor teclado mantiene solo B/Volver y no captura mando.
AyudaExplorar/Elegir muestra cruceta ystick L monocromos. Gameplay no cambiado.
Harnessdesktop drift/histéresis/diagonal/repetición/inversión/modal/stall/NaN PASS,
buildincremental4ops/diffcheckcorrectos. Usuario cerró anterior, relanzado
PID14084 biblioteca/Job5120; gate navegaciónmanual ySeries pendientes,sincommit.


ASTC sampler original2oct: prototipo opt-in `astc_sampling_probe=1`, aislado antes
de Core::System, sin integración guest. SPIR-V->DXIL SM6.4, buffers raw agrupados,
texel solicitado/point/bilinear/mips, arrays/cube y3D mediante slices ASTC2D.
PC AppContainer AMD5300M:60717 consultas CPU/direct/raw compute+fragment y22369
nativeRGBA pasan, errores máximos0,000000298/0,005099639 (precisión filtros native),
14footprints/particiones/dual-plane/sRGB/invalidos/mips/costuras, retorno0 yRender0.
Payload717328bytes=×1, buffersorigen+metadata786432=×1,096335; esto NO incluye
PSO/driver/scratch ni certifica sampler total≤×2. Timestamps4096queries muestran
16,547–369,313x coste frenteRGBA nativo en corpus sintético: no integrable default,
noFPS gameplay. Pico proceso normal732,875MiB; arranque más lento1797,4MiB.
Buildincremental ySPIRV normal idéntico sinrecordsdebug pasan. Consola0.2.71.0
empaquetada en build-uwp/astc-probe-series, símbolos archivados; Series pendiente.
JIT115/core,Job5120 ypolíticasguest intactas. Sincommit. Diseño/fuentes/resultados
en [`docs/xbox_astc_sampling.md`](docs/xbox_astc_sampling.md). GBV/smoke autónomos
se registran al cerrar el gate; no certificarlos sin PASS.


Cierre GBV ASTC2oct: real900s agotado, ambas validaciones60717/22369 pasan sin
mismatches/RenderError, pero solo20/32 benchmarks ysin cierre/retorno0/PASS: gate
incompleto, NO certificadoGBV. Pico5116,48MiB casiJob5120 sin cambiarpresupuesto.
Ensayo240s también timeout. Datos archivados194240-gbv/193600-gbv; tiempos
instrumentados no son evidenciaFPS. El gate completo debug sinGBV sí pasa.


Smoke normal ASTC2oct: opción ausente, homebrew/PID23340/Job5120/debug, sentinel,
framebuffer y cierreautomático13,203s/retorno0. Renderer/bootPASS, RenderError y
Critical0; logglobal tiene1084Input+3Core metadatosNRO+3BSD, ruidoheadless conocido
yarchivado sin ocultar. No gameplay/FPS ni gatevisualmanual. Appx no importa en
pwsh7 de esta shell; repetir scripts con WindowsPowerShell5/powershell.exe.
DiseñoASTC actualizado, paqueteSeries0.2.71.0 separado ysincommit. Sinapp viva.


ASTC guest experimental2oct: usuario autoriza integración y Game D con Job5120.
Gate PC integrado debug pasa60717+22369 consultas anteriores y7168 por etapa
compute/fragment,7mips/8capas/GOB0--3/SRV RGBA nulo,0discrepancias. astc_guest_sampling=1
opt-in;float2D/arrays PS/CS, fallback nativo para operaciones incompatibles. Conserva
padding guest y buffers committed: no hereda x1,096 del gate agrupado. Cuenta
raw/metadatos/compañeras ybaseline contrafactual, no ahorro total ni Series.
Smoke default retorna0/Render0. Importador omite BuiltInWorkgroupSize dummy que
sustituía LocalSize. Primer Game D abortado antesRun:13256pipelines antiguas alcanzan
cap5120 al recompilar; experimento ahora omite solo precarga shader y compila a
demanda. RelanzadoPID19896/play1,Job5120 verificado,prewarm115/staging256/T480 intactos,
monitor RAM activo. Calidadguest general/LOD implícito/gameplay/Q/Series pendientes.
Sincommit; fuentes,diseño,evidencia y límites en docs/xbox_astc_sampling.md.
Cierre ASTC guest2oct: última Game D PID9520/Job5120/play1 duró348,782s, salida menú
volver biblioteca, retorno10 (noQ/0), RenderError/Critical0. Array de carga raw40,6875
frente650,375MiB nativo contrafactual evitó609,6875MiB; tras su GC predominó fallback
con raw+compañera:1--1,4375MiB EXTRA en recursos vivos. No ahorro RAM total demostrado,
sin A/B/Series/FPS/visual certificados. Diag muestra hasta5063MiB commit; monitor
externo incompleto tras1muestra, no pico global. Perfil shader experimental acotado,
<=4samples/fetch y margen1GiB al preparar; fallback preservado. Prewarm115/staging256/
Job5120/T480 intactos. Corrige propiedad descriptor/compañera al mover Image y
File.Replace PowerShell NullString; build incremental pasa, smoke normal debug
retorno0/Render0, monitor22muestras/182lecturas concurrentes y running=false final.
Último cambio ownership aún sin nuevo Game D; sinapp viva/sincommit. Estas notas
sustituyen estado previo PID19896 pendiente. Informe completo y evidencia en
`docs/xbox_astc_sampling.md`, `build-uwp/astc-game-d-results/20261002-222748-bounded/`.
Comparación ASTC solicitada2oct sin cap: predeterminada peakcommit7341,371MiB vs
primer prototipo7658,629 (+317,258/+4,32% observado), no ahorroRAMtotal. Últimas
ventanas27,86/29,99 vs12,68/13,81FPS; prototipo258,761s pipeline stalls/386,323s
ventanas, duración/caches/compilación impiden atribuir causalidad a ASTC puro.
Último conjuntoASTC raw+compañera49MiB extra vsbaseline, ahorro609,6875 transitorio
no sostenido. DefaultQ/retorno0; prototipo sinQ/0confirmado. RenderError0 en ambos,
un DebugCriticalFSA escrituraactual0 en cada uno. Repeticiónprecarga128pares/256PSOs,
computeademanda, falla gatecaliente por stalls persistentes; registrado como fallo
porusuario. No appviva ni nuevos límites/cambiosrenderer/commit. Evidencia
astc-ab/comparison-observed.json y detalle en docs/xbox_astc_sampling.md.
Velocidad ASTC Directo3oct: pipelines sin decoder importado recuperan4workers si
AppFree>=4GiB; decoder importado/bajo margen siguen serializados por memoria.
Gate referencia384,633->135,798ms(2,832x), DXIL idéntico; no es mejora decode/FPS.
Cache/pesos parciales/quad2x2/mip innecesario/constantes dieron mejoras puntuales
pero regresiones GPU o compilación1,16->3,8s: rechazados, decoder original retenido.
Probe final-safe/20261003-001213-debug pasa CS/PS23552queries extra por etapa,
60717aisladas/22369nativas/7168guest,32benches+4constantes,Render0/retorno0/Job5120.
Corregido lifetime de recursos al cambiar corpus (anteserror921/devicehung en
constant-before); copias terminan antes de reemplazar. Timings separan translate/
sign de PSO; labelrgba_native eshelper importado enGuestIntegration. Shaderrestore
requieremtimeactualizado paraNinja; after8stale no certifica variante. Sin ahorroRAM
nuevo ni mejora decode general/Series certificada; sincommit. Detalle y resultados
en docs/xbox_astc_sampling.md, conservaJIT115/staging256 yA/B sinlímite anterior.
Gate final Nativo: build incremental correcto, SPIR-V no-debug del decoder normal
idéntico, smoke homebrew debug11,516s/retorno0/Render0/Job5120 y sin app viva.
Artefactos final-default-smoke-{launcher,log,diag}.txt en astc-direct-speed.
Paquete Series ASTC Directo3oct solicitado: rama xbox-d3d12-astc-directo,
versión0.2.72.0, biblioteca/play1, Game D Game E y Game G
Game G incluidos desde los dumps locales. Nuevo parámetro -LibraryGames permite
incluir varios juegos sin game= ni selección automática. Config: astc=gpu,
astc_guest_sampling1, memory_limit_mib5120 (presupuesto caches; el límite OS de
Series lo impone la consola), jit_prewarm1, fastmem0/cpu_profile0/hotkeys0;
sin debug_layer, probe, duración ni entradas programadas. Build al día (no work).
MakeAppx y firma SignTool correctos, APPX22011659592B (~22GB/20,50GiB), certificado
yVCLibs junto al paquete en build-uwp/package-series-astc-directo-0.2.72.0/.
Inspección ZIP verifica2NSP, versión/config/DLLs/firma presente y hash del EXE
idéntico a los símbolos archivados build-uwp/symbols/0.2.72.0/. Informe package-info.json.
Keys/firmware/juegos solo dentro del paquete local ignorado, nada añadido a Git.
No instalación automática ni gate gameplay/Series: pendiente prueba humana con
modo Game en Device Portal. Sin commit ni push; no afirmar mejora RAM/FPS.
ROMs externas3oct: usuario rechaza APPX22GB con Game D/Game G. Investigación
primariaDolphinUWP(pickers/ruta),PPSSPP(pickers/FutureAccessList/StorageManager),
RetroArch(VFSFromApp) yMicrosoftdocumentada en docs/xbox_rom_storage.md.
Distribuciónpropuesta appseparada, internaLocalState/games yaenumerada yUSBNTFS
seleccionado/recordado sincopiar. ActualSeedUserData duplica20,132GiBjuegos en
primera instalación; no confundirSSD/RAM. Coreusa_wfopen: picker/tokenno certifica
acceso porruta; requeriráadapterFromApp/streamVFS ygateoffsets>4GiB/Series/permisos.
MicrosoftbroadFileSystemAccessnoXbox; no dependerdesktop-onlyhandleinterop.
SinimplementaciónUSB, sinpaquetenuevo/commit/push; artefacto0.2.72conservado.

ImplementaciónROMexterna3oct: bibliotecaAgregarcarpeta(Xmando/Oteclado/ratón),
FolderPickerasyncUI/FutureAccessList,16fuentes/paginación64/cap10000externas/depth5.
Identidadtoken+ruta evita colisiones; metadata/boot usanVFSUWP read-onlyFromApp
confallbackIRandomAccessStream,offset64bits/chunks1MiB/scratchacotado/TLSMTA.
Preflight yLoadfallido vuelvenbiblioteca; sincopiar juegos ni cambiar renderer.
IncrementalUWP/testsparser+scanner ygateAppContainer2procesosPASS:fixtureNTFSsparse
4297064705B,bytesreferencia/cruce4GiB/EOF/256readsconcurrentes/reauthorización/
tokenrestaurado/catalog72/NROloader enFromApp+WinRTforzado,retorno0/commit25MiB.
APPX0.2.73.0firmado13848490B,sinjuegos/keys/firmware/fixtures,en
build-uwp/package-series-usb-0.2.73.0;EXE/PDBarchivados/hashverificado.
ASTCDirecto/prewarm115/5120/fastmem0/libraryplay1conservados;0.2.72gigantepreservado.
Pickerexterno/USBNTFSACL/desconexión/gameplay/Seriespendientes;fixtureLocalState
no certificaUSBfísico. Sincommit/push;diseño/evidencia en docs/xbox_rom_storage.md.
Series Game D0.2.73/USB3oct: Load0/Run sí pasan; abort+144,390s porruntime_error
Create committed texture fallback0x8007000E trasCreateHeapOOM. ASTCshader5/13calls
superacap4; a142,494s crea compañera650,4375MiB paraarray1024/121capas/raw40,6875MiB,
luego guardmemoria conserva shadersnativos. Últimoheartbeat3994/5120MiB antes
ese salto, no pico fatal. Siguientearray512/121 raw10,44MiB/baseline166,4375MiB
precedeOOM; no se confirmó creación de esa segunda compañera. Excepción sale de
hilo1648 yabort: gateSeriesfallido, no fallo inicialROM/USB ni×1general demostrado.
Evidencia astc-series-results/20261003-0.2.73-game-d-oom; diagnóstico detallado en
xbox_astc_sampling.md. Pendiente cobertura/fallbackmemoria/manejoOOM sincrash;
este turno solo diagnóstico, sin cambiosruntime/paquete/commit.

Pausa usuario4oct: conservar JIT115MiB/core; desactivarlo no cumple jugabilidad.
ASTCDirecto selecciona bindings reales; arrayfont40,7MiB evita compañera650,4MiB.
Copias reinterpretadas porfamilia y reclaimstaging pasan gatesGPU de datos; Game D
warm libera256MiBring pero falla siguiente compilación con~304MiB libres frente
336 exigidos. Corrida automatizada colgada al cerrar archivada y terminada; se
retiró omisión de submissions y se añadió cancelación GPU antes de Pause en fallo.
Candidato192/256/384/576/768MiB y gatepresión1/2sitios compilan; NO ejecutados aún,
cierre corregido pendiente. Sin builds/juego activos, cambios sincommit en rama
xbox-d3d12-astc-directo. Paquete0.2.74 desactualizado, no entregar; Series y Game D
con115/core bajo5120 no certificados. Detalle en xbox_astc_sampling.md.
Gatearranque4oct: ASTCDirecto precarga variantes porclave/máscara y combina
baseline separada con entornos guest/perfilesJIT validados; mantiene115MiB/core.
PC menú120s/retorno0/Render0 conJob5120, incluso cachés normales vacías:52variantes,
0imports nuevos duranteRun, pico5049,512MiB/margen70,488. Cachés originales
restauradas/hashidéntico. NoFPS/gameplay/Series certificados. Atlas evita610,31MiB
contrafactual transitorios, final0,625MiB; noA/B ahorroproceso. SmokeNativo y
GPUrecovery/refreshbaseline pasan. Baselineactualizada aparte de cache aprendido,
111MiB disco/62msPC; perfileslocales preservados, cap262144 y115 intactos.
Paquete finalfirmado0.2.75.0/43,8MiB bibliotecaUSB+baselineGame D listo en
build-uwp/package-series-astc-startup-0.2.75.0, sinROM/keys/fw/debug/timer;
EXE/símbolos/seedSHA verificados.0.2.74 viejo no entregar. Series modoGame/logs y
escenas nuevas pendientes, margenpequeño. Sincommit/push; docs ASTC/cuaderno.
Series0.2.75 revisada4oct: preflight/Load/precache52/JIT115core pasan, Run134,4s.
Atlas40,6875MiB sincompañera650,4375 evita609,75MiB contrafactual; noA/B.
12variantesruntime nuevas; DEVICE_HUNG0x887A0006 a193,883s, DREDoff.
Después creacióntextura0x887A0005/excepciónescapa/abort194,766s; recovery incompleto.
NoE_OUTOFMEMORY enlogs, memoriareportada4852/5120 trasfallo noespicoOS.
GateSeriesFALLIDO, menú/gameplaynocertificados. Evidencia20261004-0.2.75-game-d-device-hung.
Solo revisión/docs, sinbuild/renderer/paquete/commit; preservarJIT115.

### Paquete diagnóstico DRED0.2.76 (4 octubre 2026)
Usuario solicita compilación Series con DRED. Ruta ya implementada: boot.cfg dred=1
activa AutoBreadcrumbs y PageFault antes de D3D12CreateDevice. Sin cambiosruntime
ni rebuild innecesario, EXE idéntico0.2.75; manifiesto sube0.2.76.0 para instalar.
Paquete build-uwp/package-series-astc-dred-0.2.76.0/eden-xbox.appx firmado correcto,
45976024bytes, bibliotecaUSB/play1/ASTCDirecto/prewarm115/5120/fastmem0 y baseline6files.
SinROM/keys/fw/debug_layer/probes/timer. ZIP confirma dred1/config/versiónsinBOM,
firma presente,6seeds idénticos al snapshot, EXE igualbuild y símbolos0.2.76.0.
package-info.json registraSHA2569c4f08097a53602dc47ed67c341dfd03ced73e4389585416518c301295e0d300.
CER/VCLibs al lado. Usuario instala modoGame y devuelve logs: buscar DREDon o
notavailable y breadcrumbs/pagefaults en siguiente fallo; soporte efectivoSeries
pendiente. No promete identificar shader exacto ni corrigeGPUhang/abortpor sísolo.
No lanzamientoPC/instalaciónautomática/commit/push. Baseline no aprende las12
variantesSeries únicamente desde log: faltan entornos/claves completas.
### Series0.2.76 DRED y carpeta única (4 octubre 2026)
Nuevoslogs archivados como astc-series-results/0.2.76-dred-{log,diag}.txt.
DREDon confirmado pero NO hay volcado: detección por CheckRemovedAfter/ReportRemovedAfter
no llama Device::ReportDeviceRemoved (solo scheduler lo hace). Abort en creación
recurso evita llegar al scheduler; activarDRED solo no bastó. Es fallo nuestro de
instrumentación.96variantes precalentadas desdecacheSeries aprendido,0runtime,
JIT115 cada core yRun193937ms. Atlasraw40,6875MiB/fallback0. DEVICE_HUNG detectado
229,302s del log alcrearimagen51 DXGI27/Texture2D1600x900 mip1 sample1 RTflag1,
NO prueba de causalidad de esa imagen. Committedfallback0x887A0005/excepciónescapa/
abort230391ms frontend. Últimomuestreo4887MiB no pico, sin0x8007000E.
La compilaciónruntime última deltrialanterior no es condición necesaria delhang:
esta vez0runtime y vuelveafallar. No concluir decoder culpable sintrazas.
Usuario exigeuna sola carpeta compilaciónSeries: build-uwp/series conAPPX/CER/
VCLibs/version.txt/EXE/PDBactual0.2.76; copiaAPPXhashverificada. Packagerdefaultseries,
local-run explícitamente mantienepackage (layoutPC registrado necesario), futuros
paquetesSeries reemplazanactual y eliminanstaging despuésfirma. Sinrebuild/runtimefix.
Limpieza recursiva de paquetesSeriesanteriores ysymbols rechazada porpolítica del
entorno; carpetasviejas siguenpresentes, no afirmar liberación deespacio. No intentar
eludir rechazo. Pendiente conectarvolcadoDRED a ruta detección recurso yrecovery.
### Repetición Series DRED14:27 (4 octubre 2026)
Descargas ahora eden_log (1).txt/eden_uwp_diag (1).txt; archivados planos como
astc-series-results/0.2.76-dred-retry-{log,diag}.txt sincarpeta compilaciónnueva.
DREDon yesta vez Device::ReportDeviceRemoved síse ejecuta: 0commandlists recorded,
0unfinished, sinpagefault reportado. No concluirGPUinocente/CPUcausa porlistavacía:
no tenemosbreadcrumbs útilesni garantíacobertura runtimeSeries enestacaptura.
Run206468ms, JIT115/core intacto. DEVICE_HUNG243,838s dellog alcrearbuffer655360B,
justodespuésatlas512x512/121capas raw10944512B/baseline174522368B. Punto detección
varía (antesimagen1600x900), nocausa probada. Recovery ahorallegaGPUjoin/servicios/
kernelcores/GPUdestruction, peroClose decommandlist falla0x80004005 durantedestrucción:
std::terminate/abort245453ms enfrontend. Sigue gateFALLIDO, sinretorno0.
Appmem4918/5120 trasfallo (201MiB libres reportados), sinpicoOSniOOM explícito.
Siguiente diagnostico requiere trazapropia comandos/shaders/resources ysubmit/fence
paraacotarúltimotrabajo; DREDvacío noidentifica comando. Cierre enerror debeevitar
Close/Finish normales sobredeviceperdido. Solo revisión/docs, sinbuild/runtimechanges.
### Diagnóstico activo Series: checkpoints ASTC y cierre con dispositivo perdido (4 octubre 2026)
Se implementa con dred=1 (off normalmente) un historial circular de128 operaciones
host: draw/indirect con VS/PS, ASTCbindings yvertices/instancias; dispatch conshader/
grupos; creaciónimagen conguestVA/formato/dimensiones/capas/mips; submit conPSO/tick
/fencecompletado; waitbegin/end. Es historia de registro/submission, NO ejecución
completada de cadaoperación. Se vuelca una vez antepérdida, incluso cuando eltripwire
detecta encreación de recurso, juntoaDRED. NoDXIL/PSOcache nativo persistido.
DREDvacío deja de interpretarse comoGPUinocente (comentario incorrecto eliminado).

Draw directo convariantASTC: BEFORE registraVS/PS yFinish drena trabajo previo;
RECORDED se escribe después de registrar eldraw; segundoFinish espera su fence;
DONE confirma tickcompletado. FalloantesRECORDED apuntaaltrabajoprevio/preparación;
RECORDEDsinDONE acota draw+bindings/barreras/queries/timestamps de esasubmission,
NO prueba por sísolo decoder culpable. Indirectdraws/dispatches solo historiaCPU,
no checkpointsindividuales; copiasinternas no quedan etiquetadas porrecurso todavía.
Sin shader/device feature nuevo, SM6.4/JIT115/5120 conservados. Historiabounded128
entradas; stringsdeformatosacotados. Checkpoints ylog porASTCdraw alteransync/coste:
prueba exclusiva diagnóstico, NO usarFPSdeesta corrida para medirrendimiento.

Cierre: DrainForShutdown noClose/Signal/Finish sobredeviceperdido ycapturaerrores
enruta sana; aplicadoRenderer/Scheduler/TextureCache/QueryCache. Device destructor
noWaitIdle si removido. QueryCache terminaqueries soloenDevice sano ylimpiacallbacks.
Flush detectapérdida antescallback/Close; scope-exit deDraw noFlushIfUploadHeavy
trasnotificación removido, evitando segundaexcepción enunwind. Tripwire reinicializa
porDevice, permitevolver biblioteca yabrir otrojuego. No suprimirsubmissions sanas
ni relajar fences/ownership; nofallback RGBAgigante. Siguependiente Seriesreal.

GatePC realdevice-removal: opciónsoloautotest gpu_failure_probe=removed usa
ID3D12Device5::RemoveDevice enGPUthread, reporta razón0x887A0005/historial, observer
libera waiter ysegundo callback, shutdowncompleta/retorno0/workerjoined2,656s.
Esperados RenderCritical/Error porfalloinyectado; noASSERT/terminate/abort niClose
fallido. Artefactos build-uwp/gpu-removal-gate-{log,diag,launcher}.txt. NoOOMreal ni
DEVICE_HUNG causal certificado: prueba retiroexplícito conhomebrew, sinworking
setGame D. Hookrequiere rendererD3D12, interfazconsultada, disabled enpaquete.

GateGame DPC diagnóstico: Job5120 verificado PID20060, JIT115 cada core, 120s Run,
menú frame_3.bmp visualverificado; 17843 BEFORE/RECORDED/DONE (todoscompletados),
0runtimeimports Directo, RenderError/Critical0, shutdown/retorno0/workerjoined153,219s.
PicoOS5314715648bytes=5068,5078125MiB, margen51,4921875MiB. Monitor iniciótarde62
muestras peroPeakPagedMemorySize es máximohistóricoOS deproceso; dedicadoGPUfuera
Job. No mejoraFPS/ahorroABySeries certificados. Logdiagnóstico~9,76MB. Evidencia
game-d-checkpoint-gate-{log,diag,launcher}.txt yseries-memory/memory-summary.json.
Primercandidato fallóbuild pordevice inexistente enRasterizer, corregido aguard
atómicotripwire; frontend tampoco puede incluirRendererD3D12/Mesa (headerssolo
video_core), hookusa wrapperdeclarado siguiendo frontendexistente. Buildfinal5ops PASS.

Paquete actual build-uwp/series/eden-xbox.appx reemplazado0.2.77.0 firmado,
45981851bytes; SHA256b8800d90ecd33fe401ddb85c031a4e7f9e366157db65727209aed31eae745cbe.
Configlibrary/play1/ASTCDirecto/dred1/prewarm115/5120/fastmem0, 6seedfilesGame D,
sinROM/keys/fw/debug_layer/gpu_failure_probe/run_seconds. ZIP/seed/EXE/PDB/versión
sinBOM/firma presente verificados; version.txt ysymbolsactuales enmisma carpeta,
layouttransitorio eliminado. package-info.json actualizado. SinSeries install/
commit/push, sinapp ni buildvivos al terminar. Usuario instala modoGame ydevuelve
logs paraacotarGPUhang. Documentaciónprimaria:
https://learn.microsoft.com/windows/win32/direct3d12/use-dred
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error
Noafirmar causaexacta ni cierreSeriescorregido hasta repeticiónreal.
### Series0.2.77: submission aislada que no termina (4 octubre 2026, logs15:05)
Archivados planos astc-series-results/0.2.77-checkpoint-{log,diag}.txt.
15 BEFORE/RECORDED,14DONE. ÚltimoDONEtick1531 a234,877911s conPS19ca3fdc7777ba0c.
SiguienteBEFOREtick1532 VSb4cf3ca379e7cfbe/PS9112b4082b675746, ASTCbindings1,
315vertices indexed/1instance. Finishprevio confirma fence1532. RECORDEDtick1533
a234,879074s; submitted1533 completed1532, waitBEGIN1533; NO DONE1533.
A245,089034s DEVICE_HUNG0x887A0006; fenceUINT64_MAX confirma dispositivo perdido,
NO finalización (waitEND no demuestra éxito). Intervalo10,20996s desdeRECORDED.
Ahora se acota la submission1533: BindDrawState/barreras/query/timestamps ydrawASTC,
con todo trabajo previo completado. Fuerte sospecha ruta de estePS, no prueba de
que ASTCdecoder sea la única causa: falta aislarbindings/descriptor/operaciones
auxiliares. HistoriaCPUdrawtick1532 se registra antes delFinishprevio: drawreal
estáen1533, no confundir ticks. 0compilacionesruntimeASTC. DREDcontinúa0listas.
JIT115/core conservado. Memoriareportada4787/5120 (332MiB margen) antes/trasfallo,
no picoOSniOOM explícito. Próximo foco PS9112b4082b675746, 2decoder_calls en logs
anteriores: identificaroperaciones/bindings yreproducir contenido/input exacto en
probe; contraste ruta decoder yvalidación acceso, sin fallback650MiB indiscriminado.
CierreSeries esta vezcorrecto: shutdowncomplete249266ms, retorno15(recuperación),
volver biblioteca249375ms con633MiB; sinCRASH/abort/terminate. Retorno15 esperable
falloGPU manejado, NO gateGame DPASS. Logsplaytimefile/nvmapwarning aparte no son
causaGPUhang. Solo revisión/archivo/docs, paqueteactual0.2.77 sinmodificación.
Matiz clave0.2.77: mismoPS9112b4082b675746 ya completó ticks1509 y1511.
Por tanto no falla todaejecución delshader: investigar diferencia de datos/binding/
coordenadas/estado en draw1533. No concluir simplemente shaderincompatibleSeries.

### Candidato decoder acotado y pesos bajo demanda (4 octubre 2026)
La submission1533/PS9112b4082b675746 de Series0.2.77 sigue siendo la última
localización, NO una causa exacta demostrada. El mismo PS terminó dos draws antes.
ASTC Nativo expande una vez y luego muestrea con hardware; Directo repite el decoder
por texel/tap. Un DEVICE_HUNG puede depender de coste/datos/binding, sin OOM explícito.
Se acota exclusivamente D3D12_ASTC_SAMPLE: ExtractBits evita índices fuera de los
128bits (padding de grupos trit/quint=0); DecodeIntegerSequence valida tabla/cantidad,
limita64 iteraciones y tiene default con salida; ReplicateBits16; lectura de color4
words; selección de rango finita; lectura de pesos valida result_index/64. Metadatos
GuestLoad validan footprint/dimensiones/mips/tamaño de buffer antes de división;
coordenadas no finitas y LOD NaN devuelven0. LOD infinito sigue clamp del sampler.
Bucles mip/taps tienen máximos2/4. Estas defensas no prueban que el dato de Game D
fuera inválido ni que un bucle infinito fuera la causa. Semántica completa para
coordenadas no finitas y LOD implícito degenerado no certificada por estos cambios.

Gate aislado PC con solo defensas: build19+6 operaciones PASS; probe274,453s,
RunHeadlessBoot0/workerjoined, RenderError/Critical0. 60717 queries contra CPU,
22369 referencia sampler, guest7168/stage y extended17images23552/stage, todos
mismatches0. Gates llamadas5/13, masks2/1/3/0 y presión de compilación1/2 sitios
con256/288MiB libres pasan.32 benchmarks. Artefactos planos
build-uwp/astc-bounded-probe-{log,diag,launcher}.txt; build-uwp/astc-bounded-build.txt.
No gate Series ni ahorro/FPS demostrados por este probe.

Segundo cambio candidato: Directo conserva la secuencia de pesos codificados y
UnquantizeTexelWeight se ejecuta solo al leer los cuatro vecinos necesarios por
plano (hasta8 con dual-plane), en vez de recorrer hasta64 pesos por texel. El segundo
plano no se calcula en bloques single-plane. DecodeIntegerSequence todavía decodifica
la secuencia completa; no afirmar que solo se leen cuatro valores del bitstream.
No caché RGBA adicional ni pérdida de calidad intencional. Native mantiene recorrido
original. Build18 operaciones PASS; probe completo posterior en curso, no certificado.
Header SPIR-V del decoder Nativo SHA256 idéntico antes/después de ambos cambios:
54b4dfa264e3c82e8ee3c2059e963e6b83b64999a62e8ec466414ccebf98817a.
Con dred1, PS9112 registra en historia circular textura guestVA/dimensiones/capas,
viewmip/base_layer/footprint y filtros/direcciones/LOD del binding Directo. Su volcado
al fallo permite comparar draws sanos y fallido si siguen dentro de128 entradas.
JIT115/core, presupuesto5120 y arquitecturaSM6.4 conservados. Sincommit/push.
Iteración pesos bajo demanda: primera versión pasó probe completo359,047s/retorno0,
colores/gates/presión correctos, sin RenderError/Critical; al cerrar después de
workerjoined hay FIRST-CHANCE0x80010012, no CRASH ni fallo de prueba registrado.
Bench sintético compara solo defensas vs pesos bajo demanda: mediana guest
1,171x compute/1,154x fragment (16casos/stage), pero ruta aislada0,870x/0,705x.
DXIL aislado559124->718792bytes (+28,56%) y mayor compilación; no mejora general
ni FPS demostrados. No se entrega esta forma: unquantización con un único callsite
mediante vecinos4 y planos2 acotados para evitar duplicación al inline de Mesa.
La fuente actual ya tiene ese bucle; build18 operaciones PASS y NativeSPIRV hash
idéntico. Probe completo final en curso. Artefactos intermedios planos
astc-selective-probe-{log,diag,launcher}.txt, final astc-weight-loop-*.txt.
Gate final pesos4/8 con un callsite: probe292,031s/retorno0/workerjoined,32 benches,
60717CPU+22369sampler+7168guest/stage+23552extended/stage, mismatches0; llamadas5/13,
masks2/1/3/0 y presión1/2 sitios256/288MiB pasan; RenderError/Critical0. Header
NativeSPIRV SHA anterior idéntico. DXIL compute aislado556436bytes vs559124 solo
protecciones y718792 primera versión on-demand. Comparación sintética de16casos
porstage: guest mediana1,19955x compute/1,20206x fragment; aislado1,16526x/1,03077x.
Referencia es decoder con defensas y pesos completos, NO versión0.2.77 sin defensas.
No garantía para otras cargas, FPS ni Series. Estadísticas astc-weight-loop-gates.json.

Game D PC PID6216, Job5120 verificado, 120s Run sin gameplay automatizado;
167,109s retorno0/workerjoined/shutdown completo;18102 BEFORE/RECORDED/DONE iguales,
RenderError/Critical0. Menú verificado en frame_1.bmp NUEVO de15:39:12, archivado
game-d-weight-loop-menu.bmp. frame_3.bmp de14:41:11 es de prueba anterior: NO usarlo
como evidencia de esta corrida. FIRST-CHANCE0x80010012 posterior a joined no CRASH.
JIT115MiB por core0/1/2, core3 sin perfil; code115,00 en los tres, 6717/6129/3684
bloques omitidos por presupuesto, precarga~19,94s. Presupuesto conservado, no afirmar
cobertura completa. PS9112b4082b675746 no aparece en este log PC: menú/cierre pasan,
pero NO se reproduce/certifica la submission específica de Series en PC.

Monitor297 muestras terminó sinapp: picoOS5366136832B=5117,546875MiB, margen2,453125;
maxsample5110558720B=4873,80859375MiB. PicoOS puede estar entre muestras; no sustituir
por máximo muestreado. Margen muy reducido, NO certificación de memoria estable;
GPU dedicada fueraJob y SeriesUMA diferente. Nada demuestra ahorro global A/B.
Artefactos planos game-d-weight-loop-gate-{log,diag,launcher,memory}.txt/json y
astc-weight-loop-probe-{log,diag,launcher}.txt; app/monitor ya terminaron.
Manifest pasa0.2.78.0 sinBOM; preparación de único paquete actual en build-uwp/series
en curso. DRED/checkpoints/binding sospechoso siguen activos, configuración misma;
Series pendiente, NO declarar cuelgue resuelto antes de probarla. Sincommit/push.
Entrega0.2.78.0: build-uwp/series/eden-xbox.appx firmado45982323bytes, SHA256
35f755d38f62a51ff2eb65f7967bc8a5d2b4b312eb635fa41af50720f52395f3;
EXEee76134bf00f0bcad07e98ff42b3c4ae6a2d5ca7beab7fc450c4ae5d97fa2dd8.
Audit ZIP/config/6seeds/EXE/PDB/versiónsinBOM/firma presente PASS. Única carpeta
actualseries con version.txt ysymbolsactuales, sinlayouttransitorio. Configlibrary/
play1/dred1/ASTCDirecto/prewarm115/5120, sinprobe/debuglayer/timer/ROM/keys/fw.
Paquete candidato para probar enSeries, NO fixSeriesdemostrado. Metadata incluye
flagsdecoder_bounded/selective_weights/single_call_site ySeries_testedfalse.
No app/monitor/build activo al terminar; sincommit/push. Install modoGame yrevisar
checkpointdelPS9112/bindings al devolverlogs. MargenPC2,45MiB sigue riesgomaterial;
no ahorro globalni estabilidad prolongada certificados.
### Series0.2.78: continúa DEVICE_HUNG, nuevo draw acotado (4 octubre 2026, Descargas16:36)
Diag confirma instalación0.2.78.0; archivos archivados planos en
build-uwp/astc-series-results/0.2.78-checkpoint-{log,diag}.txt y0.2.78-review.json.
16 BEFORE/RECORDED,15DONE. PS9112b4082b675746 completa1554/1556/1583; cuarto
RECORDED1585 a265,928864s noDONE. BEFORE1584 acaba todo trabajo previo:
submit1584 completed1583; submit1585 completed1584, waitBEGIN1585, waitEND con
UINT64_MAX. Primer DEVICE_HUNG0x887A0006 a271,065484s,5,136620s despuésRECORDED.
Acota draw+BindDrawState/barreras/queries/timestamps de1585, NO decoder único probado.
0.2.77 terminaba dos draws conesePS yfallaba tercero;0.2.78 terminatres yfallacuarto.
Counts/orden sugieren avance puntual, NO mejora causal/coste/FPS ni fix demostrado;
no garantizar igualdad de geometría/UV/contenido entre corridas por conteo de índices.

Historia ahora identifica binding11, guestVA601ecd000, atlas1024x1024/121capas,
11mips, viewmip0/base_layer0/viewlayers121, ASTC8x8. AddressU/V1/1=WRAP,
min/maglinear1/1, miplinear0, bias0/minLOD0/maxLOD13 (bits41500000).
Draw1583 sano y1585 fallido: mismos VSb4cf3ca379e7cfbe/PS9112,315índices/1instancia
ymetadatos de textura/sampler iguales. NO certifica mismo contenido/coords/capa/
vertexbuffer/constantbuffers/estadoRT. Sano1583 termina~0,986ms trasRECORDED; fallo
no termina. Siguiente foco datos/coords/capas/estado efectivo entre ambos draws y
replay de entrada exacta, no simplemente asumir shader incompatible o bucle infinito.
Protecciones ypesos bajo demanda NO resuelven cuelgue Series; no revertirni crear
nuevo paquete sin candidato razonado/prueba. DRED otra vez0listas/0unfinished:
no breadcrumbs para identificar instrucción/pagefault. SinOOM0x8007000E explícito.

RAM reportada enfallo4794MiB de5120,325MiB libres; no picoOS ni A/B de ahorro.
Rawatlas42663936B contra baselineNative682033152B (609,75MiB contrafactual recurso
noexpandido), no ahorroglobalmedido. JIT115 cada core0/1/2,~18,025s workers,
255312/256037/258485 aceptados,6832/6107/3659 omitidos por presupuesto, core3sinperfil.
Shaderwarm~207,125s, Run233985ms frontend. Recoverypasa: Shutdown275313ms,
retorno15 a275407ms, biblioteca275453ms/678MiB, sinCRASH/abort/terminate.
Retorno15 es errorGPU manejado, NO Game DPASS. playtime/nvmap/hresultUI no identifican
causaGPU, aparecen aparte/postfallo. Solo revisión/archivo/docs/metadata; sinbuild,
ni ejecuciónPC, ni cambiosrenderer, ni commit/push. Paqueteactual0.2.78 permanece.
Gate diagnóstico ASTC Directo 0.2.79 (4oct): captura opt-in de primeros4 draws PS9112,
inputsGPU/rawASTC/metadata/DXIL/registros, readback512KiB; sin expansión ni cambio
al decoder. PC120s/retorno0/menú,17639 checkpoints completos,4 capturas verificadas,
RenderError0,Job5120/JIT115 conservados. PCusaPS6224 distinto; Series pendiente.
Paquete actual único build-uwp/series firmado0.2.79. Se necesitan log/diag y archivos
astc-capture-9112b4082b675746-* (.txt/.bin) para comparar inputs del draw que cuelga.
0.2.78 sigue DEVICE_HUNG, causa raíz sin confirmar; StateBefore promotable sobreCOMMON
está permitido por Microsoft y no demuestra error de barrier. Sincommit; detalle
completo en docs/xbox_astc_sampling.md y cuaderno.
Series0.2.79 captura recibida:16BEFORE/RECORDED,15DONE; cuartoPS9112/tick1872
DEVICE_HUNG. Cuatro snapshots completos: entre tercero sano y cuarto fallido SOLO
cambia cbuf streamed slot1 de48B/12floats finitos, aspecto matriz3x4 aúninferido.
Resto capturadoidéntico (shaders/key/registros/meta/rawASTC/geometry/otroscbuf/SSBO).
Índices0..121+base329 caben hasta450 enambosVB. No prueba causa ni descartaRT/texturas
nativas/driver nocapturados. Siguienteexperimento replay variandoese cbuf. Recovery
retorno15/biblioteca647MiB pasa; RAMfallo4798/5120,margen321. Paqueteactual marcado
FAIL, sinbuild/paquetenuevo/commit. Evidencia0.2.79-* plana astc-series-results;
una solacopia raw40,65MiB por hashes idénticos. Detalle en diseño/cuaderno.
0.2.80 diagnóstico opt-in astc_draw_replay1: GPUusaVS/PS capturados, matrices48B
alternadas, referencia expandida antesDirecto, mismas capturas existentes/sindumps.
PC16draws/8comparaciones/retorno0/GBV limpio. PS242/138,clip69/105: no cobertura
enorme. Native t8/9/10NULL,RT/depth/cull controlados: NO replaycompleto/causa probada.
Diferencias salida26/37bytes,max2/3LSB pendientes; PASS es ejecución, no calidadexacta.
Paqueteúnico actualseries0.2.80 automático; actualizar sin desinstalar(conserva capturas)
y devolver2logs. Seriespendiente, decoder/JIT115/caches normales intactos,sincommit.
0.2.80Series falló antesdel draw porcapturasLocalState ausentes(retorno14), noGPUhang.
0.2.81 replay autocontenido20assets/atlasúnico enappx actualSeries; PCpackaged16draws
retorno0/Render0/Job5120, audit/firma/símbolos correctos. Seriespendiente, sincommit.

## Series 0.2.81: replay completado, calidad fallida (4 oct 2026)
Descargas eden_log.txt/eden_uwp_diag.txt: fuente packaged self-contained captures,
16 draws y ocho comparaciones completadas; retorno 0 y joined/exiting a 18,906 s.
No DEVICE_HUNG ni errores Render. PASS significa completion, no exactitud visual.
Draw2: 714 bytes distintos, error max147/255; draw3: 397--398, max187/255.
Referencia expandida ~9--10 us/draw; Directo ~867--937 us/draw. Son draws aislados,
no FPS de gameplay. La discrepancia Series es mucho mayor que PC (max2/3 LSB).
El replay sustituye t8/t9/t10 por NULL y controla RT/depth/cull; no reproduce todo
el estado del juego ni identifica la causa original del cuelgue de Game D.
Usuario decide conservar el prototipo en commit local, sin push, y volver a xbox
con USB y recuperación de fallos del juego. Sin nuevas copias de logs/capturas.