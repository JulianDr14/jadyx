# Magnesis: plan de diagnóstico diferencial PC / Series

## Estado confirmado por el usuario (20 octubre 2026)

Con .13 el objetivo funciona en PC; el fallo de enganche se reproduce en Series.
Esto delimita la reproducción, pero no identifica todavía driver, shader,
readback ni sincronización como causa. Las pruebas anteriores resumidas por el
usuario son antecedentes; no convertir ensayos sin cambio en exclusiones
universales de subsistemas completos.

## Antecedentes que sí resolvimos

Fuente: [Iluminación y tubos](xbox_deferred_lighting.md), apartados .43/.44 y
.46 a .51 y cierre del 14 de octubre.

- Iluminación: capturamos shaders e inputs reales de Series. Con el mismo VS,
  original NoPerspective y GS identidad dieron exactamente el mismo error.
  Representar el atributo mediante interpolación smooth y reconstrucción dio
  0/32768 puntos fuera de tolerancia, frente a 24731/32768 del original.
  Se aisló la ruta de interpolación/recorte al cruzar W=0. El lowering de
  producción se contrastó con esos inputs; el usuario confirmó la corrección .44.
- Tubos: replay del DXIL original con inputs exactos de consola reprodujo
  correctamente en PC tres MRT, pero discrepó en RT3 (329/1618 interiores).
  Seis sondas aisladas coincidieron entre plataformas: no bastaban para excluir
  el shader completo. Se instrumentó el CFG original y se encontró la primera
  divergencia en muestreo. Seis controles de ramas/descarte eliminaron la
  diferencia sin cambiar las entradas. Demote nativo solo generaba DXIL idéntico
  y se descartó como solución. La canonicalización conservadora de retornos .51
  corrigió el caso: cuatro MRT exactos PC/Series y confirmación visual del usuario.

No quedó probado un erratum ISA concreto del driver. Sí quedaron demostradas
rutas sensibles a la plataforma y controles que eliminaban la divergencia.

## Prueba propuesta para Magnesis

1. Registrar versión/EXE/configuración/GPU de ambas plataformas y misma partida,
   santuario y bloque. Conservar fastmem desactivado en Series. No cambiar CPU,
   async, formatos y shaders a la vez. Conservar el log PC .13 si está disponible.
2. Captura funcional corta y marcada por acciones, sin certificar FPS: sin poder,
   Magnesis apuntando fuera del bloque y apuntando al bloque. Registrar el momento
   de LB y el estado observado. Hacerlo en Series; el PC sirve de referencia.
   Usar el menú del mando para captura, no F7. Exportar cada toma antes de que
   una nueva pueda sustituir archivos y verificar ID/completitud.
3. Correlacionar lecturas CPU/readbacks con esas acciones. El diagnóstico .13
   muestrea los primeros 400 eventos y luego uno de cada 128; calcula la huella
   como máximo sobre 1 MiB. Puede perder el evento relevante. Ceros o hash fijo
   no prueban corrupción. Preparar una ventana acotada activable desde el menú
   que registre todos los eventos relevantes y declare omisiones, antes de
   depender de una captura de gameplay larga.
4. Identificar el recurso candidato y su productor real (draw, dispatch o copia),
   rango/subrecurso, formato, versión y fence. Conservar los bytes después de
   completar GPU y los bytes finalmente entregados al guest. Permite distinguir
   salida GPU incorrecta de descarga/conversión/sincronización incorrecta.
   La captura material existente está orientada a draws; no asumir que cubre
   toda la cadena compute/readback sin verificar y ampliar esa cobertura.
5. Reproducir en PC el productor con DXIL, recursos, constantes, samplers y estado
   exactos de la captura Series. Comparar con SU salida Series, no solo comparar
   hashes de dos partidas vivas: direcciones, cámara y tiempos pueden cambiar.
   Exigir igualdad para enteros y tolerancias justificadas para flotantes.
6. Si diverge el shader, medir intermedios dentro del shader original como en
   .48/.49. Si la salida GPU es correcta y los bytes guest difieren, aislar
   descarga, conversión y fence. Solo después introducir un control que cambie
   una variable; repetir finalmente con la corrección de producción en Series.

## Alcance de la revisión inicial

Plan documentado tras revisar antecedentes y código actual. No implementa aún
la ventana de captura, trazabilidad productor/consumidor ni replay de Magnesis.
No se compiló ni se generó un nuevo APPX. Los datos/shaders guest de futuras
capturas se conservan fuera del control de versiones. Sin commit/push.

## Implementación de la captura completa, candidato 0.3.6.14

Opt-in `gpu_capture_full=1`; el menú existente **Capturar gráficos y continuar**
arma dos intervalos completos de presentación. La captura empieza al terminar la
presentación actual y conserva los comandos y las dos siguientes presentaciones.
No equivale a un replay de todos los motores ni a una ventana larga de gameplay.
Las rutas de presentación acelerada y por CPU comparten ahora el mismo ciclo de
captura. Una petición adicional no cancela el contador de finalización vigente.

`gpu_capture_targets=after` (opcional) omite la copia de render targets y depth
*antes* de cada operación y deja solo la de *después*; cada omisión queda como
evento `targets_before_skipped`. En una escena con G-buffer de 5 RT a 1080p esas
copias eran ~143 de 156 s de captura. El estado previo de una operación se toma
de la copia posterior de la anterior; una escritura fuera de draws/clears entre
ambas (blit, copia del texture cache) no queda atribuida por separado. Los
chunks se identifican con `fnv64w` (FNV-1a sobre palabras de 64 bits); el
analizador sigue aceptando `fnv64` de capturas anteriores.

Cada toma crea `log/trace/full_<microsegundos>_<id>/`, sin borrar tomas anteriores:

- `events.jsonl`: secuencia y tiempo relativos, versión/configuración, adaptador y
  capacidades, draws/dispatches directos e indirectos, parámetros, registros,
  estados de pipelines, DXIL realmente utilizado, constantes y descriptores,
  samplers, vistas nativas y runtime constants. Los shaders de una caché también
  se capturan al usarse; no hace falta borrar la caché ni volcar todo al arrancar.
- `blobs.bin`: bytes completos de los bindings de buffers, texturas y todas las
  capas/mips/planos de las vistas seleccionadas por esos bindings, y attachments
  antes/después de cada productor. Guarda también readbacks al guest, payloads
  de swizzle, consultas y snapshots de memoria tras comprobar/esperar un CPU read.
  Estos últimos son snapshots de diagnóstico; no demuestran qué valor consumió
  posteriormente una instrucción del JIT.
- `summary.json`: fin de ventana, omisiones y errores. Si falta este archivo,
  la toma se interrumpió. `data_complete` no implica validez causal ni replay
  autónomo. El analizador exige además productores cerrados y cobertura completa
  de las dos imágenes presentadas.

Las vistas muestreadas se resuelven al recurso realmente enlazado: original,
reinterpretado, depth feedback o slice array. Las barreras de lectura de imágenes
restauran el estado sin marcar escrituras ni invalidar versiones de contenido.
Los buffers conservan su generación al restaurar el estado de diagnóstico.
Readbacks síncronos usan un único staging fijado de 16 MiB, reutilizado solo tras
`Scheduler::Finish`; se libera al terminar la toma. Se guardan bytes nativos,
sin conversión de color ni padding de filas de D3D12. La imagen presentada puede
copiarse por franjas, también con ventanas PC mayores de 16 MiB.

El archivo divide los payloads completos en bloques de 64 KiB y comparte bloques
idénticos entre snapshots; un hash solo acelera la búsqueda y se comparan los bytes
antes de reutilizarlos. Así un pequeño cambio del MRT no duplica toda la imagen.
El lector verifica checksums de cada bloque y calcula SHA-256 del recurso lógico.
No reutiliza las huellas parciales/muestreadas de .13 como evidencia exacta.

Límites declarados: 2 GiB de bloques únicos por toma, 128 MiB de metadatos y 200000
eventos; binding de buffer hasta 256 MiB (por trozos), lectura CPU hasta 16 MiB y
subrecurso de imagen hasta 16 MiB. MSAA nativo no se resuelve artificialmente.
Lo que excede esos límites o no puede exportarse se marca como omisión y la toma
no se acepta como completa. No ocultar esas omisiones comparando solo lo recibido.
Las copias DMA/buffer se identifican en los metadatos; no se promete un replay
autónomo de cada copia o de trabajo anterior a la ventana.

El modo espera fences y escribe a disco: altera tiempos, solapamientos y memoria.
No sirve para medir FPS ni para descartar carreras si el fallo desaparece al
capturar. Anotar cualquier cambio de enganche durante la toma.

### Prueba manual: cuatro tomas

Usar el mismo santuario, bloque y estado de partida. Mantener Magnesis activo,
posición y distancia parecidas; no pulsar A para agarrar/mover el bloque.

1. Series, mira junto al bloque sin apuntarlo: abrir el menú por mando,
   **Capturar gráficos y continuar**, confirmar y mantener cámara quieta hasta
   que termine la pausa de captura y el juego vuelva a responder.
2. Series, apuntando al centro del bloque: repetir. Anotar color/flechas y si el
   enganche cambia al hacer la toma. Conservar las dos carpetas, log y diag.
3. PC, fuera del bloque: mismo procedimiento.
4. PC, sobre el bloque: mismo procedimiento. En PC esperamos selección correcta
   según la prueba .13 del usuario; no dar por hecho el resultado del nuevo binario.

Anotar el orden y plataforma; el ID y adaptador permiten separarlas. Exportar la
carpeta `trace` completa, no solo una imagen o `events.jsonl`, junto con
`jadyx_log.txt` y `jadyx_diag.txt`. Los archivos de la prueba del usuario permanecen
privados/ignorados por Git. Las carpetas `full_selftest_*` son controles sintéticos,
no tomas del santuario.

APPX de Series: `build-uwp/package/jadyx.appx`, biblioteca y captura por menú,
fastmem desactivado, sin payload ni `memory_limit_mib` de PC, sin captura automática
ni self-test/debug layer. Para PC, tras cerrar la sesión anterior:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/xbox/start-full-capture-pc.ps1
```

El script comprueba que el EXE del layout coincide con el APPX, registra el layout
y añade 5120 MiB únicamente a su `boot.cfg` PC. El APPX firmado de Series no se
reempaqueta ni modifica. No vuelve a ejecutar `local-run.ps1`, que reconstruiría
el paquete de entrega. No sustituir el layout mientras esa instancia PC esté viva.

### Verificación y análisis

`tests/run-full-capture.ps1` verifica escritura real, pitch, recursos de varios
bloques, repetición/cambio de un byte, concurrencia, límites y ausencia de sobrescritura.
`tests/full-capture-parser.py <fixtures>` comprueba integridad, corrupción, truncado,
índices incompletos, fin ausente, declaraciones incoherentes y cobertura de presentación.

`gpu_capture_self_test=1` es un control opt-in del mecanismo de producción, sin datos
guest: RGBA8, UINT32, depth, volumen, reinterpretación, feedback y slice array.
El analizador `--self-test` contrasta 13 salidas de imagen y dos de buffer contra
los bytes originales del upload. Las pruebas se ejecutan en AppContainer PC con
debug D3D12; esto no certifica Series.

```powershell
python tools/xbox/analyze-full-capture.py <carpeta-full> --output informe.json
python tools/xbox/analyze-full-capture.py <Series-fuera> --compare <PC-fuera> --output comparacion-fuera.json
python tools/xbox/analyze-full-capture.py <Series-sobre> --compare <PC-sobre> --output comparacion-sobre.json
```

El informe devuelve error si la captura está dañada o incompleta. Las diferencias
de dos partidas vivas se tratan como candidatos: cámara, animación, direcciones,
padding semántico y tiempos pueden diferir normalmente. Para probar una diferencia
de ejecución se reejecutará en PC un productor concreto con **sus inputs exactos
de Series**. No declarar culpable al driver solo por comparar conjuntos de hashes.

Primera prueba sin `RunSeconds` detectó un desajuste previo del sentinel del
homebrew de patrón: retornó 3 pese a pasar las 15 comprobaciones de copias; no se
aceptó como cierre limpio. Se usaron después pruebas temporizadas de homebrew
(no gameplay). Una ejecución ex11 quedó suspendida por el host: sus 45 hilos
reportaron `Suspended`; se reactivó antes del cierre. No atribuir esa pausa a un
deadlock de captura ni contar su duración como tiempo de GPU. Resultados finales
y paquete se registran en el cuaderno al completar la entrega. Sin commit/push.

Referencia técnica: [Microsoft CopyTextureRegion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion)
(copias completas para depth/stencil, planos separados) y
[upload/readback de texturas](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data)
(footprints, pitch y acceso CPU después de sincronizar).

### PC 0.3.6.14: captura interrumpida (20 octubre)
El usuario informa cierre al capturar desde el menu. Sesion full_1792512873095791_1: blobs 824643668 bytes y eventos 45789184 bytes, sin summary.json. Ultimo evento observado 143807, ~52 s de captura, op745 leyendo una textura 1024x1024 con 83 capas y 11 mips. Heartbeats siguen hasta +133843 ms con commit ~3609 MiB; no hay cierre normal ni excepcion registrada en el final de ambos logs. No aparece evento Application 1000/1001 para Jadyx en la consulta de esta corrida; AppModel muestra activacion, no causa de cierre. Proceso ausente al revisar. Captura incompleta: no valida comparacion PC/Series. La captura exhaustiva produce una pausa muy larga y trabajo repetido; esto no prueba la causa del cierre. Logs conservados en build-uwp/test-results/magnesis/pc-full-capture-interrupted; binarios de captura originales conservados en LocalState. Pendiente distinguir cierre espontaneo de cierre manual durante la pausa y corregir el coste de captura antes de repetir en Series.

### 0.3.6.15: correccion del coste de captura (20 octubre)
El usuario confirma que cerro manualmente la app por la pausa, no un cierre
espontaneo. La .14 habia copiado 11671 subrecursos de la misma textura
1024x1024x83/11 mips antes de terminar siquiera un present.

La .15 agrupa subrecursos de cada binding en una unica asignacion readback de
16 MiB y espera una vez por lote, conservando todas las filas, capas y planos.
Las vistas SRV nativas de solo lectura se descargan una vez por recurso/version/
rango durante la captura. Los usos siguientes guardan una referencia explicita
a la operacion/binding original; los recursos se retienen hasta End para evitar
confundir direcciones recicladas. Transition a estados de escritura cambia la
version del contenido; estados escribibles, UAV, aliases y feedback no reutilizan
esta cache. Los outputs se siguen copiando antes/despues. La politica de memoria
del juego y fastmem no cambian. Metadatos transmitidos a disco: hasta 1000000
eventos/512 MiB (antes 200000/128 MiB); datos unicos conservan limite 2 GiB.

Gate GPU AppContainer PC/debug: 13 imagenes y 2 buffers previos correctos,
mas 581 subrecursos (83 capas x 7 mips) con patrones distintos, todos iguales
al upload. Pruebas del parser pasan referencias exactas y rechazo de referencia
a recurso equivocado, snapshot parcial o inexistente. Homebrews ex03/ex04:
2 presents completos sin omisiones/errores; ex04 reutiliza realmente una SRV
(op6 -> op3), captura 0,763 s, cierre retornado 0 y sin Render Error/Critical.
Estos controles no miden aun la duracion de la captura real del santuario ni
certifican Series. Informes full-capture-fixed-*-gate.json y logs bajo
build-uwp/test-results/magnesis/pc-full-capture-fixed{-texture,}-gate.

Entrega unica firmada 0.3.6.15: build-uwp/package/jadyx.appx; SHA256
7E9B6A10138B6D6D6C37D2B4AE27345E8636D04996AA43B1C43047EA761E8A87.
Biblioteca/play/captura menu, fastmem0, sin payload, self-test, debug ni cap PC.
Simbolos archivados en build-uwp/symbols/0.3.6.15. Firma/payload verificados.
Pendiente captura del santuario en PC y Series. Sin commit/push.

### Comparacion con contenido igual y candidato MRT 0.3.6.17 (20 octubre)

El usuario confirma que Magnesis funciona en PC con base y actualizacion, y falla
con ambas en Series. Se recibieron otras dos capturas PC con la misma actualizacion
1114112 (0.17.0) y DLC que consola. La actualizacion no explica el fallo.
Se validaron seis capturas completas (FNV de segmentos, SHA256 de payloads,
referencias y dos presents), sin omisiones/errores. Evidencia privada:
`build-uwp/test-results/magnesis/comparison-2026-10-20/`.
PC comparables: full_1792521031922060_1 y full_1792521125116076_2;
Series: full_1792514461077450_1 y full_1792514538508743_2.

Pase de seleccion actualizado: VS 17d43777e7441cf4, PS 0b7e4e46d61d3694.
DXIL identico entre plataformas (VS SHA256
072393073642e5683238f0909c8bc67c7b8efffd5b4fab5940bc00fd787268da;
PS 0d950756afb191dd73412cc10bdf6fbb1a7a30506e6ec9eeef962f6c21d7b66e).
Dibuja tres indices en viewport/scissor 1x1. Las texturas de IDs/profundidad
1600x900 contienen datos alrededor del centro en Series; no llegan vacias.
RT0 antes/despues y readback guest: Series todo cero; PC apuntando al bloque
contiene ID en los primeros ocho bytes. Por tanto, para este resultado la
perdida ya existe en la salida GPU, antes de la descarga al guest. El readback
8x8 RGBA32F que diferia del PC base tambien es cero en PC actualizado: no usarlo
como evidencia de fallo. Los IDs concretos de actores pueden variar entre sesiones.

Replay nativo con shaders, constantes, samplers, texturas, VB/IB y attachments
previos exactos: PC actualizado ops105/1169 reproduce sus tres salidas byte a byte.
Entradas Series ops203/1319 ejecutadas en PC producen ID 000021008ab0b037 frente
a cero de consola, ademas de diferencia en profundidad RT2. Pero el replay inicial
registra DOS errores D3D12: MRT con extensiones fisicas distintas (RT0 16x4
RGBA16_UNORM; RT1/2 8x4 RGBA32_FLOAT). No es una ejecucion valida para atribuir
sin mas el resultado al compilador o a un erratum de shader.

Candidato .17: para colores sin depth, single-sample, una capa y sin vistas de
reinterpretacion, normalizar dimensiones fisicas al minimo de los MRT activos.
Scratch GPU reutilizable por slot, copia previa de la interseccion y copia posterior
al recurso original; conserva columnas/filas extra del pitch. Sin descarga CPU ni
espera adicional; copias fuera de predicacion. Directo e indirecto integrados.
Otros casos conservan la ruta anterior; no se afirma compatibilidad general de
MRT/depth/MSAA. Recursos/descriptores retirados tras fence, mediante scheduler.

Gate ejecutando el normalizador REAL de TextureCacheRuntime enlazado a las
bibliotecas UWP: cuatro replays anteriores, cero warnings/errors D3D12. Las doce
salidas coinciden con el replay PC sin normalizar; las seis del PC actualizado
siguen identicas a la captura (incluidos bytes fuera del pixel escrito).
Informes normalized-replay-comparison.json y */production-replay.txt.
Build incremental UWP/link correcto y segunda pasada sin trabajo.
Esto prueba legalizacion y conservacion en PC; NO prueba aun que repare Series.

Fuentes: [OMSetRenderTargets D3D12](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-omsetrendertargets),
[restricciones MRT en D3D11](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets).
La evidencia de incompatibilidad D3D12 proviene de su propia capa de validacion,
no de asumir equivalencia normativa con D3D11.

Entrega unica .17: build-uwp/package/jadyx.appx, SHA256
75DDAAB65113F5DA1BF1CDE07AE2A61E4380A9F6739891EC539F92DC3BBD7195.
Firma CMS/metadatos/container y 741 bloques correctos; simbolos .17 archivados.
Biblioteca/play/menu captura y portal conservados, fastmem0, sin cap PC ni payload.
Pendiente instalar en Series, apuntar al bloque y comprobar enganche; si persiste,
capturar sobre el bloque y exportar logs para verificar mrt_extents_normalized y
salida RT0. Sin commit/push. No certificar causa unica ni arreglo antes de ese gate.

Correccion de entrega solicitada por usuario: el paquete vigente es 0.3.6.19 (sustituye .17), misma correccion MRT y configuracion. Reempaquetado/firmado, manifest0.3.6.19, CMS/metadatos/payload/container y741bloques validos. SHA256 5CEB5675589C6A5922894FD04F30430104E162D4B13D6A099223E11D0CA20357. Simbolos build-uwp/symbols/0.3.6.19. GateSeries pendiente; sincommit/push.

### Gate Series .19 negativo (20 octubre)
Usuario confirma sin mejora del enganche. Captura full_1792523765769457_1,
version0.3.6.19/SraKmd_arden; 176394eventos,1356763328bytes unicos, dos presents,
FNV/SHA/referencias validados, cero omisiones/errores de captura. En ops220/1249
hay mrt_extents_normalized 8x4 bound_mask7/copy_mask1: candidato SI aplicado,
no confundir fallo persistente con paquete viejo o rama no ejercitada.
RT0 GPU y readback guest siguen cero; shaders VS/PS byteidenticos a PC anterior.
Replay de ESTAS nuevas entradas mediante runtime real normalizado en PC:
ambas ops retornan ID0000210027b01831, RT1 cero identico; RT2 PC aed67d3f
repetido frente Series24dc7d3f repetido. Cero warnings/errors del replay PC.
Por tanto legalizar dimensiones MRT no basta; no declarar causalidad del fallo
original demostrada por los errores de validacion anteriores. Mantener distincion
entre defecto de binding corregido y fallo Magnesis aun abierto.
Siguiente aislamiento debe separar valores muestreados/coords VS y seleccion
PS (bucles y memoria local588i32); no atribuir aun a driver/derivadas/localmemory
sin controles intermedios. No necesita otra captura identica para demostrar
esta divergencia. No se prepara otra version especulativa en esta revision.
Informe series19-replay-comparison.json e integridad series19-integrity.json en
comparison-2026-10-20; logs preservados series-0.3.6.19-logs. El log agregado
incluye otros juegos y asserts/formatos no implementados; no certificar log global
limpio ni atribuirlos al pase de seleccion sin correlacion. Sincommit/push.

### Diagnostico preparado .20: controles aislados del pase de seleccion

Solicitud del usuario: preparar build para aislar donde diverge .19. Durante
captura completa, solo VS17d43777e7441cf4/PS0b7e4e46d61d3694, drawindexed3x1,
framebuffer4x4 y sidecar opt-in pick_probes/enabled.flag, ejecutar29controles.
El pase guest se ejecuta y captura antes, sin sustituir shader ni sus destinos.
Los controles usan cuatro imagenes scratch8x4, RTV propios, mismoVS/IA/CB/texturas/
samplers/viewport/scissor y PSO clonado. Rechazan etapas adicionales/UAV/storage/
depth/MSAA e interfaz distinta. Sin predicacion para separar tambien esa variable;
queries guest desactivadas durante controles, estado invalidado tras terminar.
Solo activos al capturar; no coste de PSO/readbacks por frame normal.

Pases (se guardan las tres salidas y DXIL de cada control):
0: PS original intacto, RT0UNORM/RT1-2float; baseline aislado8x4 sin predicacion.
1: PS original intacto, tres RTfloat; separa conversion/formato de RT0.
2: mismo CFG/math/memoria local, solo Sample->SampleLevel0 (dos instrucciones).
3: mismo CFG/math, RT1 expone numero de IDs distintos, profundidad minima y
componentes de ID seleccionado; modificar outputs puede alterar optimizacion,
por tanto su concordancia es control, no prueba unica de registros internos.
4..28:25 offsets [-2,2]x[-2,2], HLSL propio sin bucles/memoria local, mismo VS:
RT0 ID.Sample(centro+2*offset*Generic0.zw);
RT1 profundidad.Sample(centro+offset*Generic1.xy).r, UVid.xy, centro.x;
RT2 Generic0.zw/Generic1.xy. Mantiene filtros y Sample implicito originales.
Compara las muestras antes de atribuir fallo a seleccion/bucles/array588i32.
No es correccion del enganche ni identifica de antemano un erratum del driver.

Roles image_data pick_probe_<pass>_rt<0..2>, eventos pick_probe y pick_probe_dxil
con op/pass; shader0/1 originales tomados del pipeline vivo. Los otros27sidecars
son privados y preparados con tools/xbox/prepare-pick-probes.py; LLVM y DXIL guest
solo en test-results ignorado y este APPX diagnostico local, nunca en repositorio.
HLSL propio en tools/xbox/tests/pick-samples.hlsl. manifest.json explica offsets.
Falta/lectura invalida de sidecar registra omision/error; no aceptar captura como
prueba completa entonces. No altera politica de caches/JIT/CPU/fastmem.

Gate PC:29ejecuciones GPU nativas con entradas exactas de Series.19, bindings y
runtime normalizador real: cero warnings/errorsD3D12. Originalfloat yLOD0 coinciden;
25IDs concuerdan con lookup CPU independiente en textura, varyings reciprocos
1600/900 correctos y conteo nozero. Referencia pick-probe-pc-reference.json y
pick-probe-pc-gate/<0..28>/result.txt bajo comparison-2026-10-20.
Los controles2/3 LLVM ensamblados y validados con IDxcValidator; HLSL con DXCps6.4.
Build UWP incremental/link correcto. La integracion de captura en gameplay y
Series aun no ejercitada: gate nativo valida shaders/datos, no certifica ese flujo.

Entrega unica .20: build-uwp/package/jadyx.appx firmado; CMS/metadatos/payload/
container validos,63archivos/770bloques,27DXIL+flag+manifest sin LLVM ni juegos.
SHA256 09107D19086A2CAD9E82343983DFDA9A396B9E79FD5C60B18837B011D74039D1.
Simbolos build-uwp/symbols/0.3.6.20. Biblioteca/play/captura/portal,fastmem0,sin
limitePC ni debug en Series. No instanciaPC activa antes de reemplazar layout.
Prueba solicitada: en Series apuntar al bloque con Magnesis activa, capturar una
vez desde menu y exportar logs por portalJadyx. Deben aparecer29controles x3RT
por cada pase de seleccion capturado; despues comparar valores con PC exacto.
Sincommit/push; causa y arreglo siguen pendientes.

### Revision .20: captura integra, controles invalidos; correccion .21
Captura full_1792525037294177_1 deSeries.20:185029eventos/1180518165bytes,
FNV/SHA/referencias ydosframes verificados, cero omisiones/errores declarados.
Ops172/1333 tienen29controles cadauna, peroTODAS sus salidasfloat conservan
sentinel-99999, incluyendo PSpropio simple sin bucles. RT0UNORMpass0cero es
limpieza clampada. Por tanto NO usar esta toma para concluir fallo de muestreo,
LOD, arraylocal, shaderoriginal ni driver: los controles no generaron fragmentos.

Defecto del diagnostico: BufferCacheRuntime::ApplyGeometry consume pending_index
ypending_vertex_mask eneldrawguest; lasdescargas posteriores Finish resetean lista
ybindingsIA. BindDrawState solono repone geometria ya consumida. La pruebaGPU
nativa .20 enlazabaIAexplicitamente, no ejercitaba este flujo de capturagameplay;
fue insuficiente para detectar esa omision de integracion. Resultado negativo
registrado, no es un nuevo hallazgo sobre la causa de Magnesis.

.21: CapturePickProbes recibe los TracedBuffer deldraw yreconstruye vistasVB/IB
exactas (GPUVA+offset,size,stride/formato); lasenlaza explicitamente antes deCADA
control, independientemente deApplyGeometry. Rechaza geometriafaltante, indices
reescritos/8bit yfirst_indexnozero paraesta interfaz fija. Ademas cada readback
floatdecontrol verifica elpixel0: sentinel intacto añade pick_probe_no_fragment
yomision, impidiendo llamarcompleta una captura sin cobertura como.20.
Paseguest, shadersdiagnosticos29 ypoliticasnormales sinotroscambios.

GateGPU PC conreset real scheduler.Finish yrebindcompleto: seiscontroles
representativos0/1/2/3/16/28 sinwarnings/errors;18pixeles de tresRT identicos a
referenciaanterior. reset-geometry-gate.json y*/reset-result.txt. Esto valida
restauracion deIAenreplay, integracioncapturagameplay/Seriesaun pendiente.
Informe Series.20 series20-probes.json/series20-integrity.json encomparison-2026-10-20.
Sincommit/push; causaoriginal delenganche todaviaabierta.

Entrega.21 firmada/verificada63archivos770bloques, simbolos.21archivados. SHA256 CD2729B807A1306BEB0743844440F63F0E1CB50792D3DA34C20B2400F400605E. Buildincremental/linkpasan. Paqueteunico build-uwp/package/jadyx.appx; repetirunacaptura sobrebloque yexportarportal. No instanciaPCactiva alreemplazarlayout. Sincommit/push.

### .21: controles validos, divergencia localizada en seleccion con bucles/local (20oct)
Captura full_1792525986040490_1, version.21/SraKmd_arden:175258eventos,
1405270279bytesunicos, FNV/SHA/referencias/dosframes pasan, cero omisiones/errores.
Ops162/1225 completan29controles cadauna y escriben realmente (sentinel eliminado).
ReplayPC de58controles con EXACTAS entradasnuevas: cero warnings/errorsD3D12.

Hallazgo fuerte: las25muestras aisladas (pases4..28), suscoordenadas y cuatro
reciprocosdevaryings tienen valores float32 EXACTAMENTE iguales PC/Series en
ambasops:150salidasfloat4 comparadas, cerodiscrepancias. Las25muestrasID son el
mismoID nozero [0,33,45194,184] enUNORM16. Profundidad varia enelgrid correctamente.
Elshaderoriginal sigueIDcero enSeries tantoRT0UNORM comoFLOAT32; SampleLevel0
no modificael fallo. El controlconteos(pase3) retorna numeroIDs1 enPC y5enSeries,
ySerieseligeIDcero pese a las25entradasIDiguales. MinimoPC0,9914244413375854
(op162) /0,9914243817329407(op1225), Series0,9915003776550293 enambas,
que corresponde ala primera muestra[-2,-2], mientras loscontroles aislados si
obtienen lasprofundidades posterioresmenores. Originalbaseline0 yfloat1/LOD0pass2
muestranla mismadiferenciaminimo, no solola varianteinstrumentada3.

Se localiza divergencia enejecucion delprograma original conbucles/estadoarrastrado
ymemorialocal588i32; muestreoaislado, coordenadas, formatoRT0, LODypredicacion
nocorrigen la salida. No atribuir aun subcausaexacta a arraylocal/phi/driver ni
proponerunareescritura porhash comosolucion general: hacefalta control que separe
flujo/phis de accesoslocales. Elconteoexpuesto puede variaroptimizaciondelshader,
peroladiferenciabaseexistetambienenPSoriginalintacto.

Evidencia: series21-integrity.json, series21-probes.json,
series21-exact-probe-comparison.json, pick-probe-pc21-gate/<op>-<pass>/result.txt
bajocomparison-2026-10-20. Inputs yshaders extraidos series21-op162/op1225 privados.
No falta otra capturaigual para confirmar lo anterior. No paquete nuevo enesta
revision; .21 vigente. Causa especifica/fixSeries pendientes. Sincommit/push.

### .22: separar contador/tabla local/CFG mediante controles (20oct)
Usuario pide ejecutar siguiente aislamiento. Se anaden10controles al flujo.21
ya confirmado enSeries, manteniendo controles0..28 yrebindIA/sentinel. Todos los
nuevos parten delcontrol3 (PSoriginalconoutputsconteo), cambian una caracteristica:
29volatilesenload/storelocal;30contadorouter derivadoSSA 3-y conservandotabla;
31contadorouter aisladoenallocai32 separado;32sololoadvolatile;33solostorevolatile;
34inicializa588palabrasantesdeejecutar;35alloca201palabras (indicesusados0..200);
36alloca1024palabras;37elimina23bloques de salto vacio conunpredecesor sin cambiar
math/accesoslocales;38combinacion37+volatile. Los tresprimeros GEPconstantes200
sonlosunicosaliasesdirectosdelcontador; tablaIDs usa0..99/counts100..196 para25IDs.
Para30elforoutery=-2..2 carga5..1, equivalente a3-y, sin sustituir tabla ni samples.
Para37seactualizanPHI alpuentearbloques yse rechaza introduciredges duplicados;
LLVM numerico renombradoanombresparaensamblar sinrequerir slotsconsecutivos.
No esunoptimizadorproduccion ni correccion deenganche. Siuncontrolcambiaresultado
Series, todavia hayqueseparar elcambiodriverdeoptimizacion antesdeunfixgenerico.

Generador: tools/xbox/prepare-pick-loop-controls.py, recibe pick_probes/3.ll privado
ysalea misma carpeta privada de sidecars, extiendemanifest a39. NoLLVM/DXILguest
alrepo. Los10LLVM ensamblados yfirmados/validados conIDxcValidator.
GateGPU PC reset/rebind:66draws (baseline3 +10variantes, en6entradas), sinavisos
D3D12;180salidas completas delas60variantes coincidenbyteporbyte conbaseline.
Entradas: doscapturasreales.21, ceros, rayas, IDsunicos ydoszonas. Conteos0,1,2,25
cubiertos; rayas deperiodo2muestreadas cada2pixels producenunID (no contarcomo2).
Caso25verificausomaximo detabla ylimite201; resultadosminimo/seleccionconservados.
Evidencia loop-controls-pc-comparison.json yloop-controls-pc-gate/<case>-<pass>/
bajo comparison-2026-10-20. PasarPC no predicecualcontrolresolveraSeries.

Fuentesprimariasconsultadas:
[LLVM LangRef volatile](https://www.llvm.org/docs/LangRef.html#volatile-memory-accesses):
volatile restringeeliminar/reordenar esosaccesos, no es sincronizacionentre hilos.
[Microsoft DXIL](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst):
IRbasadoLLVM3.7 para consumo delcompiladordeldriver. Ningunafuenteafirma unerratum
especificodeldriverSeries; suconfirmacionrequiereelresultadohardware.

BuildincrementalUWP deunobjeto+linkpasa. APPX0.3.6.22firmado/verificado73archivos,
780bloques/CMS/metadatos/payload/container; simbolos.22archivados.
SHA256 1B7D0B913E7AE3BC6E3E1DA4A13F9C3C36A0412DBD669C59044FFE81B305954E.
Entregaunica build-uwp/package/jadyx.appx, sininstanciaPCactivareemplazando layout.
39controlesal capturar, fuera de captura ninguncontrol; fastmem0/biblioteca/play/
portal/capturamenusinotroscambios. InstalarenSeries, apuntarbloqueconMagnesis,
capturaruna vez yexportarlogs. GateSeries nuevo pendiente; sincommit/push.

### .22 revisada: contador separado recupera 25muestras, tabla sigue fallando
Captura full_1792526740700443_1, .22/SraKmd_arden,181521eventos/1480431733bytes,
FNV/SHA/referencias/dosframes verificados; cero omisiones/errores. Ops165/1299,
39controles en cadauna, cobertura valida. Los20DXIL decontroles29..38 descargados
coinciden EXACTAMENTE con sidecars validados PC, descartando version equivocada.

Baselineoriginal yconteos: SeriesIDcero,5IDs,min0,9915462732315063; PCIDcorrecto,
1ID,min0,9914668798446655. Las25muestras aisladas Series contienenunmismoIDnozero.
Control30 (sustituye solo loadcontadorlocal por3-y) y31 (aisla contador enalloca
escalar) CAMBIAN Series de5 a25IDs yrecuperanmin0,9914668798446655 EXACTOPC,
peroIDseleccionadosiguecero. PC mantiene1IDcorrecto ysalidasidenticas entodos.
Controles29/32/33(volatile),34(inicializacion),35/36(tamano201/1024),37(CFG23
forwardersmenos) y38(CFG+volatile) conservanelmismofallobase5IDs/minprimerafila.

Gateexacto nuevoPC:22draws (11controlesporop) sinwarnings/errors;los10controles
conservan resultadosbaseline enambasops. series22-exact-loop-comparison.json,
series22-probes.json,series22-integrity.json yloop-controls-pc22-gate son evidencia.

Interpretacion: separarelcontador delarray basta paraejecutarlascincofilas;los
bucles/actualizacionminimo pueden funcionar cuandoelcontador no depende delarray.
Quedafallo en tablaIDs/conteos:25entradasiguales deberianagruparseen1, Seriescrea25
yseseleccionacero. Este conjunto es compatibleconlecturasdelarraylocalque devuelven
cero (contadorarraysaldriaalterminarprimerafila; tabla noveseescritura,conteos
ceroimpidenescogerID), pero NO se hanexpuestovalorescrudosdetodaslasloads para
certificarlocomoerratumexactodeldriver. Volatile/inicializar/cambiartamano/simplificar
saltosvacios no reparan. Prioridad: representacion/lowering deaccesos dinamicos a
arraylocal enDXILSeries, preservandosemantica, no masLOD/color/readback/CPU.
Noafirmarqueloscontroles30/31sonarreglo deMagnesis: IDsiguiendo0. .22continua
vigente; no paquetenuevoenestarevision. Sincommit/push.

### Investigacion web 20oct: posible fallback para memoria local indexable
Solicitudexplicita: investigar internet antes deotra prueba. No se encontro una
referencia publica que confirmeesteerratum enSraKmd_arden/Series. La hipotesis
esmiscompilacion oincompatibilidad delpatronDXIL localindexable, basadaen.21/.22;
noes una limitaciongeneral admitida porDXIL niesta identificadoelfabricanteculpable.

Fuentesprimarias:
1. Microsoft DXIL specification, seccionesMemoryaccesses/Indexablethreadlocal/
Outofbounds: https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst
Threadlocal indexable se declara alloca enaddrspace0, accesosload/storeconGEP;
i32 admitido. El validadoryelcompiladordeldriver son etapasdistintas; validacion
correcta noejecutaelshader enSeries. OOBlocal C-like/undefined: unselect posterior
no legitimaria unload fuera dearray. Ennuestroshader contador200 esta dentro588;
el algoritmo25muestras usaID0..99,counts100..196,counter200. Hayque mantener
limites y comprobaroffsets si se cambia representacion, no asumirtodosloscerosOOB.
2. IssueMicrosoft#2817 https://github.com/microsoft/DirectXShaderCompiler/issues/2817
reporta diferencias entre indexadodinamico yconstante conDXILplausible; maintainer
confirmaqueindexadodinamicoeslegal yconsideraposibleerroreldriver. CASODISTINTO:
raytracingpayload/tamano, noPSlocaldeSeries. Soloantecedente deconsumo deDXIL;
no citarcomoconfirmacion delmismerorr ni trasladarsusolucionbuffercomoobligatoria.
3. Mesa nir_lower_indirect_derefs_to_if_else_trees.c (MIT, Inteloriginal):
https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/main/src/compiler/nir/nir_lower_indirect_derefs_to_if_else_trees.c
Fuenteprimaria mirror deMesa: reemplaza load/store indirecto con arbolbinario de
if/else; hojasusan indicesconstantes yloadseunen conphi. Despuespuede aplicarse
nir_lower_vars_to_ssa paraquitar memoria. Tamano/growthdebenlimitarse.
4. Mesa nir_to_dxil.c (MIT, Microsoft):
https://raw.githubusercontent.com/chaotic-cx/mesa-mirror/main/src/microsoft/compiler/nir_to_dxil.c
ContrastadoconnuestroMesa26.2.3fuera repo: optimize_nir usa ese lowering para
nir_var_function_temp conmaxarraylen4;588elementos nopasaneseumbral. emit_scratch
emitealloca yemit_load/store_deref usaGEP/load/store. Numeroslocales:optimize~6315,
emit_scratch~5945,deref_to_gep~3886. NoespruebadebugenMesa; confirmaqueelshader
llegaald riverconlarutaindexable. SPIRVEden DefineLocalMemory usaPrivatearrayde
structu32, EmitLoad/WriteLocal AccessChain; Mesa laacabaexpresandocomoalloca588i32.

Solucioncandidata prioritaria (aunNOimplementada): evitaraccesosindirectosalocales
paraestepatron medianteindicesconstantes + SSA, reutilizandoelpassMesa antesde
emitirDXILo representacionequivalente ennuestroSPIRV. Primercontrolsolocapturado:
originalkernel/mismastexturas/IA/samples, bitsexactos yalgoritmodeID/conteos;
reemplazar representacion deTODAlatabla+contador, no soloelcontador30/31.
ConfirmarDXILresultantesinGEPdinamico/allocaindexableparaellocaltransformado.
PCvalidador+replay0/1/2/25IDs debeproducirbitsidenticos; Seriesdebeagrupar25IDs
en1 ydevolverIDcorrecto, despuespruebadeenganche. Medircrecimientocodigo,registro/
compilacion/tiempodrawantesgeneralizar. No aumentarumbralglobalamenteciegamente:
arbol588porcadaaccesopuedeinflar muchoelshader. Intentar dividirvariables/rangos
probados parareducircoste; no recortar588a201generalmentesinpruebaderango.

Alternativassecundarias: accesosconstantesseparados/registrosconseleccion balanceada;
sila rutaenteraallocaligualfalla, scratchUAVporinvocacion comoultimorecurso, que
requiereasignacionprivada/unicidadfragmentos/helpers/indices/barreras/cap. No usar
unbuffersharedunico porPSni sustituirpor groupshared enpixelshader. Cambiar i32 a
floatconbitcast esotrocontrol posiblepero no respaldadoporerratumpublicoencontrado.
Noesperarresolucionporvolatile/zero-init/tamano/CFGvacio, ya negativos.22.

Resultado: investigaciondocumentada yvia concreta decompatibilidad basadaenpass
existente. Noactualizaciondedriver/SDKrequerida, noDLLMesa cambiada, nobuildnueva
por investigacion. .22siguevigente. Fix/Seriesno certificados; sincommit/push.

### .23: experimento DIRECTO en gameplay, sin array local indexable
Usuario pide probar yaenSeries. Para evitarrebuildMesa ypermitirunapruebacausal,
se prepara sustitucion PRIVADA del PSconocido; no esfixgenericointegrado.
Nuevo tools/xbox/lower-pick-local-array.py transforma LLoriginal privado:
588slotsallocai32 independientes;loads dinamicos usanlecturasconstantes yun arbol
balanceado deselect; stores dinamicos usanupdatecondicional porigualdad delindice.
Indicesconstantesaccedensoloslotrespectivo. Conserva588palabras, norecorta201,
texturas/samplers/IA/math/CFGoriginalexceptorepresentaciondellocal. Losloads extra
de slotsnoinicializados sepromuevenaundefSSA; elprograma validosoloobservaslots
escritos. No seafirmasemanticaOOB: GEPinbounds originalya haceOOBindefinido.

Luego IDxcOptimizer mem2reg/simplifycfg/dce yensamblado+IDxcValidator. Inspeccion
DXILfinal: CEROalloca yCEROgetelementptr. Toda lamemorialocaldeesteshader se
expresaenSSA. Primera variante conarbolesdebranches ymerge588folioso provoco
explosiondePHIs a5917664bytes; descartadaparaentrega. Versionselectcompacta:
489228bytes vsPSoriginal~5600, siguecrecimientoimportante. NocertificarcosteFPS/
compilacion/memoriaSeries ni extrapolar a todoslosshaders; driverpuederealizar
spillsinternos aunque IRno tengaarrays. Codigo/LLVM/DXIL derivadosignoradosprivados.

GateGPU PC:6entradas (dos.21reales,zero,stripes,unique,ties),formatosfloat:
RT0yRT2 completos bitidenticosalbaseline;RT1originalcero. Dosdrawsadicionales con
formatosrealesUNORM/float/float conservantodas6salidas completas. Todo conreset/
rebindIAreal y ceroD3D12warnings/errors. Casos0/1/2/25IDscubiertos. Informe
comparison-2026-10-20/direct-local-pc-comparison.json; logs direct-local-pc-gate/.
No espruebaderendimiento; gatehardwareSeriespendiente.

Activacion enGraphicsPipeline trascompilarDXIL yantesdecrearPSO: exactVSguest
17d43777e7441cf4/PS0b7e4e46d61d3694 y comparacion BYTEEXACTA delPScon
pick_local_direct/original.dxil. Soloentonces clonastages yreemplazaPS por
pick_local_direct/direct.dxil. Sinarchivos, errorlectura/limite1MiB, otroskeys o
DXILoriginaldistinto conservanrutaanterior yregistranmotivo. Log ACTIVE prueba
aplicacion; DiagnosticDxil ycapturasven bytesrealmenteejecutados, noeloriginal.
Noofuscar comofixgeneralporhash: estoesunexperimentooptin medianteassetsprivados
inmutables delAPPX. Restore eliminasidecars/reempaquetasinStageScript.

.23 cambiael shaderqueusaeljuego para probarenganche. Capturaconserva39controles:
0/1ahorausanPSreemplazadovivo;2/3/29..38siguencontrolesoriginalesdelossidecars,
loquepermitereferenciaoriginalque falla enlamismatoma. Metadataguesthashsigue
identificandooriginal; usarpayloadDXILylogACTIVE paraconocerrepresentacionejecutada.

Entrega build-uwp/package/jadyx.appx0.3.6.23, firmado/verificado75archivos789bloques,
CMS/metadatos/payload/container validos. Simbolos.23archivados, incremental1obj+link
pasa, sininstanciaPCactivaalreemplazarlayout. APPXSHA256
EC0F05601DCB1CD0DCDFA6DBC7EAEC9CB972D4B8B48CAD12177543AF131D441C.
PSoriginalSHA2560d950756afb191dd73412cc10bdf6fbb1a7a30506e6ec9eeef962f6c21d7b66e;
PSdirectSHA25658a216975ffbdf96514ed232de62ca17a68ade3decc13d2abb5086a796da3a9d.
Biblioteca/play/fastmem0/capturaportal mantenidos,sinlimitePC/debug/payloadjuego.
PruebaSeries:apuntarbloqueconMagnesisyverenganche; capturaruna vez yexportarlogs
paraconfirmarACTIVEyresultados incluso sifunciona. Sinfixcertificado/commit/push.

### Gate Series .23 negativo: sustitucion confirmada, salidas ID y depth cero
Captura full_1792527920883117_1, .23/SraKmd_arden,172520eventos/1390634477bytes,
FNV/SHA/referencias/dosframes verificados sin omisiones/errores. Ops129/1193,
39controles validos porop. PS vivo capturado:489228bytes SHA256
58a216975ffbdf96514ed232de62ca17a68ade3decc13d2abb5086a796da3a9d EXACTOcandidato;
pruebaactivacion aunque export noincluyelogtexto (jadyx_log.txt0, sinold.txt).
Noespaqueteviejo ni fallbackalPSoriginal.

Paseguest RT0 yRT2 enteros cero enambasops. Controles0/1, conPSnuevo, tambien
ID0/depth0 (floatnoes-99999: superancheckcobertura). Controles3(originalconteos)
conservan5IDs/minprimerafila~0,9909387;30/31originalcontadorseparado25IDs y
min0,9908820390701294/0,9908820986747742. La salidaoriginalno seconvirtióen0depth
por cambiarinputs: elbaselinecontroloriginal siguefuncionandocomoantes.

Replay GPU PC deESTAS entradas ynuevoPS, formatosrealesyreset/rebind: dosdraws
sinwarnings/errors, RT0ID[0,33,45089,24536] yRT2mincorrecto anterior. Series
siguezerosfrentePCcorrecto, inclusoenvariante sinalloca/GEP. Informes
series23-integrity.json,series23-probes.json,series23-guest-outputs.json,
series23-exact-direct-comparison.json yseries23-op*/production-replay.txt bajo
comparison-2026-10-20;bloboriginalenDescargas puede ser reemplazadoporfuturastomas.

Conclusion: laimplementacion DIRECTA .23 NOesunfix, empeoradepthademasdeID.
Evitarindexables conesta expansionSSA no basta. No declararsolucionnirLowerIndirect
probadaonegatodosupotencial: estaesunarepresentaciondistinta(588slots/selects,
optimizacionDXC,489KiB). Nopodemosdeducirsi nuevoceroprovienedetamanocomplejidad,
undef/phisodeldriver sincontrolreducido. Nohaybasesparadarunerratumespecifico
confirmado. Siguienteaislamiento debe ser reproducer pequeno ydeterminista de
store/loadlocal/indiceconstante-dinamico frenteSSA (sinundef), antesdeotraexpansion
grande ointegraciongenerica. .23siguepaqueteexperimentalvigente, nocertificarpara
release; no nuevopaquete enestarevision. Sincommit/push.

### .24: bateria automatica de84hipotesis/casos porunaejecucion
Usuario pideprobarvariasideasporbuild. Se prepara matrizautonoma determinista,
no otra transformacion grandesolo delshaderguest. 14escenarios x2estadios(pixel/
compute) x3rutas(DXC-O3,DXC-Od,glslangSPIRV->Mesa->DXILfirmado)=84casos.
Escenarios: scalarSSA;arrayconstante;storedinamico/loadconstante;storeconstante/
loaddinamico;ambosdinamicos;contadorenarray;buclesanidadosSSA;seleccionconarray
uint;seleccionconarrayfloatusandobitcast;seleccioncontadorseparado;seleccionsin
arrayscon1ID;seleccionsinarrayscon5IDs;ambosdinamicosentamanos201/1024 (normal588).
Entradasrootconst[5,200,0x3f123456,0o5]yexpected calculadosCPUenmanifest.
Todoslosarraysseinicializan, ciclosbounded(5x5/countermax8) yningunresultado
dependededatoslocalessin definir. n/indiceclampadosen shader ademasdelchequeohost.
Matrizfloat pruebarepresentacion bitcast, noaritmeticafloat decontador.

Codigo nuevo:
- tools/xbox/tests/local-memory-matrix{,-vs}.hlsl,prepare-local-memory-matrix.py:
compila56DXILdirectos y28SPIRV; CLItests/local-memory-matrix.cpp usaShaderCompiler
REAL deproduccionparatraducir/validar28Mesa. SPIRVsolo local, paquetellevaDXIL.
- diagnostics/d3d12_local_memory_matrix.{h,cpp}: ejecutormismoparaPCyapp; queue/lista/
root/RTUINT1x1/UAV/readbackpropios, noVBguest nitexturasguest. Compute limpiaUAV
con0xA5antesdecadacaso; graphicslimpiaRT0;expectednonzero permite detectar nodraw.
Esperafenceantesdereutilizarobjetos;guardSEHcreacionPSO,errorsporcaso,deviceRemoved/
timeoutaborta. case_begin seflushantesdecompilarparaconocerelcasodeunainterrupcion.
- RendererD3D12 alconstruirse detectalocal_memory_matrix/enabled.flag; staticonce
porproceso ejecutabateria antesdeguestdraws. Sinflagnoejecuta. LogJSONLpropio
local_memory_matrix_<timestamp>.jsonl enLogDir, asi portalJadyxloexporta.
Incluyerevision24/LUID,compiler/stage/parametros/expected/actual,bytesPSOcreate_us,
HRESULT/exceptionfault sihay,pass/countdebug; endpassed/failed/complete.

PCgateFINAL:84/84pasan, cero warnings/errorsD3D12, JSONexpected/actualexactos.
Tambien2negativos connop-PS/nop-CS producenfailed2,detectandoceroRT/sentinelUAV;
no cuentancomocasosdeentrega. Mesa28firmados/validados (CLItexto ya generalizado).
Shapesauditados:12de28Mesa y32de56DXCconservanlocalstorage;casosoptimizadossin
arrays soncontroles, noafirmartodos84ejercenalloca. Hash/bytes/alloca/GEP porcaso
enmatrix24-shapes.json. matrix24-pc-final.jsonl/negative.jsonl ensamecomparison.
CompilacionincrementalUWP ylinkcorrectos; primerC4244porasignacionencadenada
UINT->UINT16corregidaseparandocampos. PCgateejecutamismo.cppdeproduccion; arranque
AppContainer/Seriesautomatico aunpendiente. No certificarrendimientopor84microtests.

Entrega.24: build-uwp/package/jadyx.appx firmado/verificado160archivos867bloques,
CMS/metadatos/payload/container; simbolos.24. SHA256
CCC6C7B1A8E793697015FF2459E8F8A06746577C724641683BA68A127CEDDB3A.
SINcarpetapick_local_direct: .23overridefallidonoactivo; PSguestoriginal otravez.
Conserva39probesanterioresysoloencaptura,sisequisieracomparacioncomercialdespues.
No hacefaltacapturagrandepara84casos: instalar.24,abrirjuego,esperarquecargue,
volverabibliotecayexportarlogsportal. Uninformecontendra84resultadosyendcompleto.
Mismolibrary/play/fastmem0,nocapPC/debug,nopayloadjuego. InstanciaPCausenteantes
reemplazarlayout. Diagnosticoparamultihipotesis,nofixMagnesiscertificado.
Sincommit/push; revisarSeriesantesdedecidir siguienteparche.

### Gate Series .24: 66/84 pasan,18fallosPS reproduciblesfuera deljuego
Informe local_memory_matrix_1792529041240520.jsonl descargado:beginrevision24,
84nombresunicos/endcomplete=true,passed66,failed18. AdapterSraKmd_arden enlog.
Params/expected/file_bytes porcaso igualesalPCvalidado;84/84PC vs66/84Series.
NingunHRESULT/exceptionfault, cero debugwarnings/errorsdeclarados (Series sin
capadebug: ceronoescertificaciondebugallí). Todossonmismatchesdeterministas.
Compute:42/42pasan. Pixel:24/42pasan,18fallan. Las3rutasDXCO3/DXCOd/Mesadan
exactamentelosmismos6escenariosfallidosPS:
- storedinamico/loadconstante, ystoreconstante/loaddinamico: retorna0envezseed,
  canalescontrol seed+7/index200/n5correctos.
- contadorarray:count/sum0envez5/15;seedcorrecto.
- seleccionarrayuint/floatbits:distinct/chosen/iterations0,min100000(siniterar).
- seleccioncontadorseparado:distinct25/chosen0/iterations25/min0(envez1/seed/25/0).

PasanenPSlas3rutas:scalar,constant-array,ambosindicesdinamicos,ambosdinamicos
201/588/1024,buclesSSA,seleccionSINarraypara1y5IDs. Elcasodynamic_both_DXCOd
SIretienealloca1/GEP12 (noatribuirsupaseunicamenteaoptimizaciondeMesa);
dynamic_store/loadDXCO3retienenalloca1/GEP3. Reproduccionpequena legalconentrada
conocidafuera delshaderguest, independientedeMesa ydeasíncrono/cachesguest.

Hallazgo refinado: la matrizdirecta detectafallo alMEZCLARaccesosconstantes y
variables alarrayprivadoenPS; no afirmarquetodoarraylocal/indexadodinamicoesta
roto(pasanconst/constydinamico/dinamico). Escompatibleconproblema de coherencia/
alias/lowering delconsumidorDXILenrutaPSdeSeries. Nosehaidentificadola funcion
internadeldriver ni demostradoqueesa seaunicasubcausadecadacasocomplejodeMagnesis.
Las42pruebascomputeconmismasoperacionespasan; otroestadioevitalafallapequena.

Siguientealternativafundamentada: normalizarrepresentacion deaccesos paraevitar
mezclaconstante/variable (debeconservarindicesysemantica;controlenSeriesantesde
fixgeneral), oalgoritmode seleccionpequenosinarrays, queyapasaenPS, sinexpansion
588slots/489KiBfallida. Computeescandidatorobustoalternativo,perooffloadPSguestno
esautomaticamentesemantico porhelpers/derivadas/outputs; primeroaislarpicking.
Noimplementarparchesglobalesalazarapartirdeestos84casos.

Evidenciaarchivada matrix24-series.jsonl y matrix24-series-comparison.json bajo
comparison-2026-10-20;PCMismoexpected/params yfilesizecontrastados. No necesitarepetir
esteinformeparaestablecerlapatologia. .24vigente,no paquetenuevoenestarevision.
Sincommit/push; fixenganchependiente.

### Correccion de lectura .24 y candidato .25 sin memoria local (20 oct)
La lectura "falla al mezclar indices constantes y variables" es incompleta:
`selection_separate_counter` usa solo indices variables y tambien falla (25 IDs,
elegido 0). Patron compatible con los 84 casos: en PS de Series, una load del
array local devuelve 0 cuando el compilador no puede reenviar en registros el
valor recien escrito (`dynamic_both` pasa porque store y load usan la misma
expresion). Compute 42/42. Hipotesis no probada: el PS .23 (489 KiB, sin alloca)
pudo forzar spills del driver a la misma memoria y por eso tambien dio ceros.

.25: `tools/xbox/tests/pick-select-registers.hlsl`, HLSL propio, mismo algoritmo
sin tabla: cada muestra se vuelve a leer; la muestra i abre entrada si ninguna
anterior tiene ID igual (float ==), su conteo es 1 + iguales posteriores, orden y
desempate (primer maximo estricto), bits elegidos, minimo de profundidad y RT1
cero iguales al original. Bucles de limite constante y Sample fuera de flujo
divergente. DXC ps_6_4 -O3 -Gis -denorm ftz: 5580 bytes, cero alloca/GEP/fast,
mismos bindings t3/t4/s3/s4/cb2 y firma TEXCOORD0/1 -> SV_Target0..2. Coste:
25 x 27 fetches por pixel en un viewport 1x1.

Gate PC (mrt-replay-gate, runtime normalizador real): 8 casos (real162/1225 .21,
sus variantes UNORM, zero/stripes/unique/ties), PS original frente a nuevo: 24
salidas RT identicas byte a byte, cero warnings/errors D3D12.
`select-registers-pc-comparison.json` y `select-registers-pc-gate/`.

Activacion: mismo hook `DirectPickLocalExperiment` (hash VS/PS + DXIL original
byte exacto SHA256 0d950756...7b66e); sidecar direct.dxil SHA256
F6184B68818CD57EA240908EF1313B3A8F9019F4D12CDB1D741CB865F1BA101E. Sin bateria .24.
Entrega build-uwp/package/jadyx.appx 0.3.6.25, 79 entradas, SHA256
14128EAE30BD9B9684783AC81F0142C8C64962C647368AE1E2B60AF73B915723; simbolos
build-uwp/symbols/0.3.6.25. Library/play/captura menu/gpu_capture_full, fastmem0,
39 controles solo en captura. Gate Series pendiente: apuntar al bloque con
Magnesis y ver enganche; capturar y exportar logs. Sin commit/push.

### Gate Series .25 POSITIVO (20 oct)
Usuario confirma que Magnesis engancha en Series. Log: `direct-local experiment
ACTIVE` (3 PSO), cero Critical. Captura full_1792531484612036_1: 170224 eventos,
cero omisiones/errores. Ops 103/1130 ejecutan el PS nuevo (SHA256 F6184B...).
Readback guest RT0 16x4 UNORM ya no es cero (`0000210041b0c0b2`, mismo patron de
ID que PC) y RT2 profundidad 0,9914 (`62ce7d3f`). Causa: lecturas de array local
indexable en pixel shader devuelven 0 en Series cuando el valor no se reenvia en
registros; el pase de seleccion perdia su tabla de IDs. Arreglo actual: sustitucion
opt-in de ese PS por la version sin memoria local; falta generalizarlo (lowering en
el recompilador para PS con arrays locales) antes de release. Logs en
series-0.3.6.25-logs. Sin commit/push.

### Arreglo general: pool UAV de memoria local en pixel shaders (0.3.6.29)
Sustituye la sustitucion por hash (`DirectPickLocalExperiment`, eliminada). Al
arrancar, un PS de prueba con array privado de 512 palabras escribe y relee un
valor (`d3d12_local_memory_scratch.cpp`); si se pierde (Series lee 0), el perfil
activa `fragment_local_memory_scratch_lanes` y los PS con memoria local mayor de
64 palabras la guardan en un storage buffer (u0 space 28, UAV raiz solo pixel) en
lugar de un array privado. En PC la prueba pasa y nada cambia. Override boot.cfg:
`ps_scratch=auto|uav|native`.

Diseno (`shader_recompiler/local_memory_scratch.h`): cada wave toma un slot de un
bitmap al entrar (un lider elegido por ballot, atomicas solo una vez por wave) y lo
libera cuando sale su ultima lane viva (epilogo o demote); con una sola salida y
sin demote no se siguen lanes. Datos intercalados por lane (accesos coalescidos),
indices acotados, busqueda limitada con slot de overflow (sin cuelgues). Pool
perezoso, crece por filas de 64; el antiguo se libera tras la GPU.

Gate PC `tools/xbox/tests/local-memory-scratch.cpp` (`run-demote-helper.ps1
-Fixture local-memory-scratch`): 588 palabras con salida unica, con demote y control
privado; SPIR-V valido, enlace Mesa y DXIL firmado sin alloca en los casos pool.
Gate Series 0.3.6.29 POSITIVO: prueba lee 0 -> pool; PS de seleccion compila,
cero PSO rechazados, pool 640 palabras x 64 lanes x 129 slots (20642 KiB), 30 fps
estables.

### Pool solo para acceso indexado y slots adaptativos (0.3.7.8 - 0.3.7.10)
Regresion en un juego 2D a pantalla completa: parpadeo, pixeles corruptos y GPU al
80% (X) / 90% (S). Causa: sus PS tenian mas de 64 palabras de memoria local solo
por spills a offsets constantes, entraban al pool y 128 slots no bastaban para las
waves en vuelo: las waves giraban buscando slot (GPU alta) y acababan en el slot
de overflow compartido (datos corruptos). 0.3.7.8 con `ps_scratch=native` lo
confirmo (juego correcto, Magnesis roto como se esperaba).

1. Gate (0.3.7.9): `DefineLocalMemory` solo usa el pool si algun
   `LoadLocal`/`WriteLocal` tiene offset no inmediato. Lo accedido a offsets
   constantes se vuelve registros en el compilador del host y conserva sus
   valores (matriz .24). Gate Series POSITIVO: el juego 2D no crea pool; Magnesis
   crea el de 640 palabras y funciona.
2. Slots adaptativos (0.3.7.10): el numero de slots ya no esta compilado en el
   shader. Cabecera del pool: [0] waves que tomaron slot (y arranque rotativo de
   la busqueda), [1] waves en overflow, [2] waves que recorrieron todo el bitmap
   antes de ganar slot, [3] palabras de bitmap (slots/32, la escribe el host al
   crear el pool). El shader lee [3], acota a [4, 64] (128..2048 slots) y calcula
   la mascara de busqueda y el stride de filas `(slots+1)*lanes`. Slot de overflow
   = `slots`; `INVALID_SLOT` marca la wave sin slot para el release.
   El host (`LocalMemoryPool`) copia la cabecera a un readback mapeado en la
   primera draw con pool de una lista, como mucho cada 250 ms, sin esperar nunca a
   la GPU; con la muestra completada calcula deltas y duplica los slots si hubo
   overflow o si esperaron >= 16 waves y mas de 1 de cada 32. Tope 128 MiB y 2048
   slots (aviso en log). Nunca encoge; nunca recompila shaders.

Coste: PS sin memoria local indexada, cero (array privado, como upstream). PS con
pool: una carga de la cabecera por lane y las atomicas por wave que ya tenia; en
CPU una comparacion de tick por draw con pool y una copia de 16 bytes por muestra.
Gate PC: anade `constant_offsets` (espera array privado); 4/4 PASS.
