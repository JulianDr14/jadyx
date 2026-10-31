# Generación de cuadros (frame generation) en el present D3D12

Cuadros intermedios entre los del juego, generados en la presentación del renderer D3D12. El
objetivo principal es que un juego atascado entre 15 y 20 fps (por ejemplo, uno que la
sincronización de la consola deja en 20 porque solo permite 20 o 30) se vea a **30 fps con un
ritmo estable**. El secundario es que un juego que ya va a 30 se vea a 60.

Se activa en la biblioteca: **Ajustes › Gráficos › Generación de cuadros** (modo `auto`) para
todos los juegos, o en las opciones de un juego, pestaña **Gráficos**, solo para él. Encenderla
apaga FSR y encender FSR la apaga: las dos no corren juntas. Si un juego pide una y los ajustes
generales la otra, gana la del juego. No se cambia desde el menú en partida.

`boot.cfg` tiene prioridad sobre esos ajustes (para pruebas):

```
frame_gen=auto    # media <28 fps -> 30, <56 fps -> 60 (banda de 1 fps); por debajo de 11, apagado
frame_gen=30      # siempre a 30 si el juego va por debajo de 28.5
frame_gen=60      # siempre a 60 si el juego va por debajo de 56
frame_gen=off     # por defecto
```

Requiere `async_presentation` (el hilo de presentación propio). Sin él, el modo se ignora.

## Por qué un diseño propio

| Opción | En la Series (UWP Dev Mode) |
|---|---|
| FSR 3 frame generation | No: necesita typed UAV loads de RGBA16 y un swapchain proxy de Hwnd; el nuestro es de CoreWindow |
| Interpolación con motion vectors del juego | No: el emulador no tiene los vectores del juego |
| AFMF, Lossless Scaling (LSFG) | Son de PC (driver o app de escritorio); la idea sí aplica: flujo óptico sobre la imagen final |
| El frame gen del emulador anterior de Xbox | El repositorio se retiró y ningún fork lo conserva; además apuntaba a 60, no a 15-20 -> 30 |

Lo que hay es interpolación con **flujo óptico sobre la imagen presentada**, como LSFG o AFMF. La
lógica de ritmo está hecha para ratios no enteros (20 -> 30, 15 -> 30, 24 -> 30).

## Dónde corre

```
GPU thread (Composite)                       Present thread (D3D12Present)
  CompositeLayers -> PresentFrame (FSR)        espera el swapchain (waitable)
  PresentManager::Present                      FrameGen::Next(ahora): pacer elige t
    FrameGen::OnFrame (pacer, modo)              real   -> copia el slot al back buffer
    RecordIngest en la lista del scheduler:      blend  -> compose (compute) -> back buffer
      copia -> slot, luma, movimiento          Present(sync_interval)   30 fps: 2, 60 fps: 1
    Flush, Commit; el frame vuelve al pool
```

- **FSR o frame gen, nunca los dos**: si el filtro FSR está escalando la imagen (la salida es
  mayor que la del juego), el frame gen se apaga en ese mismo frame y FSR sigue su camino de
  siempre. Con otro filtro, o si FSR no escala, el frame gen trabaja sobre el PresentFrame ya
  escalado por el blit, al tamaño de la salida (hasta 4K).
- A 4K el movimiento se estima a **1/4** de resolución (960x540), igual en píxeles que 1080p a
  1/2: el coste no depende de la salida. Por debajo de 1440p se estima a 1/2.
- El hilo GPU nunca espera a la pantalla: el frame se copia al historial y vuelve al pool al
  momento. Con una cola de presentación propia (`present_queue=1`) espera en CPU solo si el slot
  que reutiliza aún se está leyendo, lo que casi nunca pasa.
- Menús del emulador y pantallas de arranque no pasan por el frame gen: cortan la continuidad.

## Reloj de contenido y ritmo (`d3d12_frame_gen_pacer`)

- Cada frame del juego lleva el índice de vsync en que se compuso
  (`HardwareComposer::m_frame_number` -> `HwcLayer` -> `FramebufferConfig::vsync_index`). El
  tiempo de contenido es `vsync_index / 60`. Así un juego a 20 tiene frames exactamente cada 3
  vsyncs, aunque el host los entregue con jitter.
- **Reloj de cola**: un juego sin vsync fijo (25 fps sale en 2 y 3 vsyncs alternos) anima por sus
  propios tiempos, no por los vsyncs. Cada frame lleva también la hora del host en que el juego lo
  encoló (`QueueBuffer` -> `BufferItem::queue_time_ns` -> `HwcLayer` -> `FramebufferConfig`). Con
  16 frames se comparan las varianzas de los intervalos de vsync y de cola: si la de cola es 4
  veces menor (y la de vsync pasa de 2 ms), la cola es el reloj de contenido; vuelve a vsync si
  deja de ser 2 veces menor. El contenido de cola se mantiene entre 2 vsyncs antes y el vsync en
  que se compuso. Un juego atado al vsync conserva siempre el reloj de vsync. Coste: el retardo
  sube lo que varía la cuantización (hasta ~15 ms a 25 fps), a cambio de quitar el judder.
- **Frames solo del overlay**: si en una composición solo trajo buffer nuevo el overlay del
  sistema (corre a su ritmo), no es un frame del juego. Mientras el frame gen corre y el juego
  presentó en los últimos 250 ms, ese frame no entra al pacer ni al historial: su vsync partiría la
  cadencia y se interpolarían dos imágenes iguales. El overlay sale con el siguiente frame.
- **Offset host-contenido**: sigue la envolvente inferior de las llegadas. Sube con una deriva
  lenta (0.5 ms por segundo) y baja como mucho 1 ms por frame, para que una llegada temprana no
  se vea como un salto. Si llegan 3 frames más de 150 ms tarde (un tirón del host) o más de
  150 ms antes (el reloj del juego saltó adelante), se reinicia y el retardo vuelve a calcularse
  desde cero: el tirón no se queda como latencia.
- **Retardo por slots listos**: el pacer anota en qué slot de salida estuvo listo (ingest
  terminado en la GPU) cada frame. Un par A -> B solo necesita B en el primer slot posterior al
  tiempo de A: hasta entonces se puede mostrar A. El retardo es el mínimo que cumple eso para los
  últimos 48 pares, dejando fuera los 2 peores (≈ percentil 96): un frame tardío suelto se pasa
  repitiendo uno en vez de sumar latencia a todos.
  - 20 -> 30 espera dos tercios de intervalo (~33 ms más que sin frame gen); 15 -> 30 y
    30 -> 60, medio. Antes era un intervalo entero, más el jitter, más 4 ms.
  - Un frame más de un intervalo tarde sobre su par cuenta como retención, no como latencia.
  - Sube como mucho 2 ms por cuadro de salida. Baja el 1 % del exceso por cuadro (mínimo
    0.25 ms), o el 15 % si sobran menos de 4 ms: así los frames reales caen justo en sus slots
    sin el *phase lock* que había antes.
  - Solo cuentan los frames que la GPU ya terminó: con cola de presentación propia, el hilo de
    presentación no espera detrás del render del juego.
- **Tiempo de contenido monótono**: el tiempo mostrado nunca retrocede, ni siquiera en un
  reinicio tras un tirón. Se queda quieto hasta que el reloj lo alcanza.
- **Slot tardío**: si no ha llegado el frame siguiente, se muestra el último real. El retardo no
  sube por esto: una pausa no es jitter.
- **Slots de salida**: a 30 fps, `Present(2)` sobre 60 Hz. El reloj de slots avanza un periodo
  fijo y solo se corrige si el hilo se despierta a más de medio periodo de lo esperado.
- **Latencia del swapchain**: a 30 fps se deja un solo frame en cola delante de la pantalla (cada
  uno está 33 ms en pantalla); el segundo solo sumaba sus 33 ms a la latencia de entrada. A 60 y
  en passthrough se mantienen 2. DXGI no baja la cuenta del objeto waitable al bajar
  `SetMaximumFrameLatency`, así que `Swapchain::WaitForFrame(latencia)` retiene la cuenta sobrante
  (la espera sin presentar) y la devuelve al subir.
- **Fase**: `t` cae entre dos frames A y B. Se muestra el frame real si la fase está a menos de
  0.08 de 0 o de 1. 20 -> 30 queda en real, 2/3, 1/3, real…, y 15 -> 30 en real, 1/2, real….
- **Elección de la tasa**: por el intervalo **medio** de los últimos 16 frames, no la mediana. Un
  juego que mezcla frames de 2 y 3 vsyncs (entre 20 y 30) tiene una mediana que salta de 30 a 20,
  y el modo auto saltaba con ella entre 30 y 60. Cada umbral tiene una banda de ±1 fps: la tasa
  actual se mantiene hasta que el juego la pasa claramente.
- **Cambio de modo**: activar o apagar el frame gen debe mantenerse 24 frames; un cambio entre
  30 y 60, 90 frames (cada cambio se ve como un salto de cadencia). `off` se aplica al instante,
  también en un redibujado sin vsync nuevo: el menú del emulador sobre el juego en pausa llega con
  vsync 0 y, si no, el hilo de presentación seguía mostrando los frames del juego encima.
- **Pausa**: si el juego deja de presentar (más de 4 intervalos o 0.3 s), el hilo deja la última
  imagen y se duerme hasta el siguiente frame.

## Estimación de movimiento (`d3d12_fg_luma.comp`, `d3d12_fg_motion.comp`)

- **Pirámide de luma R8** de 4 niveles por slot.
- **Block matching bilateral** (simétrico, estilo 3DRS) de bloques de 8x8. Un bloque del cuadro
  intermedio que se mueve `v` se compara como A en `-v/2` contra B en `+v/2`. El campo queda
  definido sobre la rejilla del cuadro intermedio, sin huecos al deformar.
- **Nivel 3** (el más grueso): cada bloque busca en ±4 texels alrededor de cero y otros ±4
  alrededor del vector que tenía el par anterior (un giro sigue girando). El término de
  suavidad, que prefiere ese vector, lo elige sobre los alias de un fondo repetitivo.
- **Movimiento global**: el mismo pase suma, para cada vector de ±20 texels (±320 píxeles a
  1080p: un giro de cámara rápido a 15-20 fps), el coste de todos los bloques; un workgroup por
  bloque reparte los 1681 vectores entre sus 64 hilos. Un pase de un workgroup elige el vector
  que mejor encaja en todo el cuadro: un giro mueve casi todo el cuadro con un solo vector,
  mientras que el mejor vector lejano de un bloque suele ser un alias. Un vector lejano tiene que
  ganar por 3/255 a los cercanos (alrededor de cero y del global del par anterior). Cada slot
  guarda su vector global.
- **Niveles finos**:
  - Candidatos: el vector grueso del bloque y de sus 4 vecinos, el vector temporal (el campo del
    par anterior, porque el movimiento continúa), el global y cero con bonus (HUD, escenas
    quietas).
  - Refinado de ±1, y de ½ texel en el nivel 0.
  - Penalización de suavidad hacia el vector grueso (el global no la paga).
- **Bordes**: los texels cuya pareja cae fuera del cuadro no cuentan; en un paneo o un giro son
  contenido que uno de los frames no tiene. Si quedan menos de un cuarto del bloque, el vector
  cuesta 10/255, y el global 2/255: el movimiento de cámara explica el borde mejor que un
  parecido que encaja dentro por casualidad.
- **Sin ramas por texel**: el coste de bloque pondera los texels de fuera en vez de saltarlos.
  Con un `continue` las 128 lecturas no se agrupaban y la ingesta pasaba de 1.3 a 3.7 ms.
- **Corte de escena**: el nivel 0 suma su error en un contador atómico por slot.

## Composición (`d3d12_fg_compose.comp`)

- **Hilos**: uno por texel de luma, que escribe un bloque de 2x2 píxeles (4x4 a 4K).
- **Vector por píxel**: el campo vive en el cuadro intermedio (fase 1/2), y lejos de esa fase
  una trayectoria rápida lo cruza bloques más allá de `p`. Por eso se buscan los 9 bloques
  alrededor del bloque que cruza el vector del bloque propio, más el propio y cero. Se elige por
  el coste de una cruz de 5 texels de luma (A en `-t·v` contra B en `+(1-t)·v`), más una
  penalización si la trayectoria no cruza el bloque del vector.
  - Los empates los gana el bloque que la trayectoria cruza, no el cero: dentro de un objeto
    plano que se mueve, cero encaja igual de bien y dejaría sus detalles donde estaban. Las
    zonas quietas tienen campo cero, así que lo conservan.
  - Con un extremo fuera del cuadro el coste es 1/255 si el vector es el global (±1 texel) y
    6/255 si no; el píxel toma el frame donde está dentro.
- **Sin fantasmas**: mezclar A y B con un vector malo muestra las dos imágenes. Por eso el píxel
  se inclina hacia el frame más cercano (una sola imagen deformada) cuando:
  - no encaja en ningún frame (algo que uno tapa y el otro descubre; coste de 8 a 28/255);
  - los dos colores deformados no coinciden (diferencia de 0.12 a 0.30), aunque la luma sí: es la
    comprobación de consistencia bidireccional de la interpolación clásica;
  - el error medio del cuadro es alto (de 0.4 a 1 vez el umbral de corte): escenas rápidas o con
    partículas mezclan menos en todo el cuadro.
- **Corte de escena**: si el error medio supera 28/255, se muestra el frame más cercano tal cual.

## Memoria y coste

- **Slots**: 4 de historial al tamaño de la salida (RGBA8), más la salida. A 1080p son unos 42 MB;
  a 4K, unos 170 MB más la luma (pequeña).
- Los recursos se crean la primera vez que el frame gen se activa, no al arrancar.
- **Coste por frame del juego**: copia, pirámide, el pase ancho del movimiento global y 4
  pasadas de búsqueda sobre 960x540 como máximo.
- **Coste por cuadro generado**: una pasada de composición sobre la salida, más la copia al back
  buffer.

## Archivos

- `src/video_core/renderer_d3d12/d3d12_frame_gen_pacer.{h,cpp}`: el ritmo, sin D3D12.
- `src/video_core/renderer_d3d12/d3d12_frame_gen.{h,cpp}`: recursos, ingest, salida.
- `src/video_core/renderer_d3d12/d3d12_present_manager.{h,cpp}`: el hilo de presentación con
  slots generados.
- `src/video_core/host_shaders/d3d12_fg_{luma,motion,compose}.comp`.
- `src/jadyx/app/uwp_boot.cpp`: la clave `frame_gen=` y los ajustes gráficos de cada juego.
- `src/jadyx/settings/graphics_settings.h`, `graphics_page.h`: Ajustes › Gráficos y la pestaña del juego.

## Límites conocidos

- **60 Hz**: se asume una salida de 60 Hz (`Present(2)` = 30 fps). Una TV a 120 Hz mostraría el
  modo 30 a 60.
- **Artefactos**: en el HUD sobre fondos en movimiento y en bordes de objetos rápidos que se
  tapan, como en cualquier interpolación sin vectores del juego.
  - Objetos que se mueven más de ~32 píxeles por frame (1080p) respecto al fondo: la búsqueda
    cercana no llega y el vector global solo cubre la cámara.
  - Giros de más de ~150 píxeles por frame sobre patrones periódicos pueden elegir un alias.
  - Junto a un objeto quieto durante un giro rápido queda una franja descubierta (0.4-0.6 % de
    los píxeles en las pruebas).
- **Latencia**: el retardo de reproducción (dos tercios de intervalo a 20 -> 30) se suma a la de
  entrada. Bajarlo más exige extrapolar (predecir el siguiente frame en vez de esperarlo), con
  artefactos de desoclusión.
- **Sin probar en consola**: falta la prueba en la Series.

## Pruebas (`tools/xbox/tests/run-frame-gen.ps1`)

El gate usa las librerías UWP de producción sobre la GPU del PC con la capa de depuración. No
usa WARP: los shaders son SM 6.4 y el WARP de Windows 10 llega solo a 6.2.

- **Pacer**: 15, 20 (jitter de 4, 12 y 30 ms), 24 (2:3), 30 -> 60, 20 forzado a 60, tirón de
  400 ms, hueco de 500 ms y reloj adelantado 500 ms. Ningún retroceso ni retención; error de paso
  máximo 2.2 ms (2:3) y por debajo de 0.9 ms en el resto.
  - Picos de 80 ms cada 4.5 s a 20 fps: el retardo se queda bajo 70 ms (retenciones solo en los
    picos).
  - Juego a 25 fps sin vsync fijo: con tiempos de cola toma el reloj de cola y el judder baja a
    menos de 1 ms y a un tercio del de vsync; un juego a 20 fijo con tiempos de cola dispersos
    conserva el reloj de vsync.
- **Interpolación** (objeto que cruza un fondo con textura), error en la banda del objeto:

  | Salida | Fase 1/2 | Fase 1/3 | Fase 2/3 | Fundido simple |
  |---|---|---|---|---|
  | 1280x720 | 11.6 | 12.0 | 12.0 | ~48-50 |
  | 3840x2160 | 13.7 | 13.8 | 13.5 | ~48-50 |

  Par quieto y corte de escena: error 0. Ejecución virtual 20 -> 30: 53 salidas, 30 generadas.
- **Coste** (Radeon Pro 5300M, reloj de pared; la consola dará otros números):

  | Salida | Ingest por frame del juego | Composición por cuadro generado |
  |---|---|---|
  | 1280x720 | 0.7 ms | 0.4 ms |
  | 1920x1080 | 1.4 ms | 0.8 ms |
  | 3840x2160 | 1.9 ms | 4.6 ms |

- **Suite** (`tools/xbox/tests/run-frame-gen-suite.ps1`): casos de ritmo de juegos reales y de
  imagen en la GPU. Giros de cámara de 64 y 128 píxeles por frame: 0.02-0.47 % de píxeles malos
  (un fundido simple da 3.5-20 %). Siguen fallando objetos de 64 o más píxeles por frame, el giro
  de 200 y algunos casos de estrés del pacer (picos del host, doble buffer con GPU al límite).
- **Regresión**: `run-present-queue`, `run-game-menu`, `run-frontend` y `run-recorder-d3d12`.
