# Portal local de importación

## Estado: 19 octubre 2026

Integrado en `main` mediante merge6e2983386b, junto con portugués brasileño y ajustes
de idioma de juegos, versión0.3.4.0. El árbol del merge coincide con la punta integrada
432985f2a4; receptor/store/validador NSP conservan el código probado.
Conexión/emparejamiento y
subida de juegos base NSP implementados. El usuario confirmó explícitamente que la
conexión y el emparejamiento desde móvil fueron en Series. El gate de importación
real/rendimiento en esa consola continúa pendiente. Claves y firmware ZIP web siguen
pendientes; USB conserva su importador. Link incremental UWP completado.

| Gate | Estado |
|---|---|
| Objetos UWP servidor, store, validador, biblioteca/canvas/catálogos | PASS |
| Pairing/origen/intentos/reinicio/cierre TCP | PASS; cierre incompleto ~0–114ms con polling cancelable |
| Transferencia TCP32MiB y comparación de todos los bytes | PASS, callback de validación sintético |
| Cortes/reinicio/checkpoints rotos/colisiones/rename/cancelar validación/fallo de I/O | PASS |
| Offset >4GiB | PASS: prefijo sparse+MiB final real; no equivale a transferir5GiB |
| Página: sintaxis, SHA256 y huella es/en | PASS; cliente mock5GiB prueba bloques4/16/64, lost ACK, resume, pausa y cola |
| Visual y recorrido completo desde navegador real | Pendiente |
| Conexión/emparejamiento LAN en Series | Confirmado por el usuario |
| NSP real AppContainer HTTP/validación/publicación | PASS: 7.268.474.425 bytes, comparación íntegra, original intacto |
| Reinicio del servidor y reanudación real >4GiB | PASS: instancia nueva recupera offset4.311.744.512; no reinicio de proceso |
| Biblioteca/arranque del juego importado | Pendiente con UI real/Series |
| Series: memoria, CPU, rendimiento4/16/64MiB | Pendiente |
| Web claves/firmware ZIP | No implementados todavía |

Prueba Series en curso19oct: el usuario comunica352,0/13960,6MiB confirmados,
14,8MiB/s y ETA890s. Es una muestra puntual; finalización, reanudación y velocidad
media completa siguen pendientes. ETA ahora se presenta en horas/minutos/segundos
(890s →14min50s;3661s →1h1min1s), con horas sin límite24 y sin estimación para valores
no finitos. Este cambio se sirve en la siguiente instalación; la página ya cargada
durante esa transferencia conserva el código anterior.

Preparación0.3.4.0: build incremental desde main y arranque biblioteca AppContainer
PC Responding, diag scanning/waiting for user selection. Pruebas localization-catalogs,
system-settings, text-entry y cliente web mock pasan. El smoke PC no ejecuta guest ni
certifica visual/Series. Evidencia pc-0.3.4.0-library-{diag,log}.txt en test-results/network-portal.
Entrega única biblioteca normal build-uwp/package/jadyx.appx, símbolos0.3.4.0.

## Candidato de rendimiento Wi-Fi,19oct (entrega de prueba0.3.4.1)

Usuario aclara PC y Xbox ambos por Wi-Fi. Banda/canal/AP y tasa completa desconocidos;
14,8MiB/s (~124Mb/s) sigue siendo muestra puntual. Con radio/canal compartido existe
contención entre ambos tramos; no demuestra cuello de CPU/disco. Siguiente comparación:
mismo archivo/ubicación, cablear solo un equipo, luego ambos si es posible; conservar
la conexión durante la transferencia actual. Comparar ?chunk=16/64 por separado con
tres corridas completas, misma red y archivo; mantener16 por defecto hasta ventaja≥5%.

Código httplib0.46: body se lee en≤16KiB. PortalStream hacía poll y SocketStream::read
volvía a hacer otro antes de recv. Candidato delega directamente a read, reintenta
Timeout del poll100ms hasta idle30s, conserva cancelación y propaga errores. No cambia
macros globales/ODR, buffers TCP ni framing. El ACK sigue payload.Flush → checkpoint
flushed/rename → respuesta. Sin paralelizar bloques ni ampliar RAM por archivo.

Snapshot y status.transfer.performance exponen solo último bloque confirmado:
confirmed_offset/bytes/total_us/write_us/flush_us/checkpoint_us/reader_calls. No se
persisten ni gobiernan recuperación. Relojes solo por fase/escritura1MiB, no callback.
total menos escritura/flush/checkpoint incluye red, copias, locks, apertura/truncado y
espacio: no llamarlo tiempo de red puro ni CPU. Perfil no avanza si falla checkpoint.
En consola del navegador emparejado, con candidato instalado, consultar sin mostrar
sesión: `(await (await api('/api/v1/status')).json()).transfer.performance`.

El ejecutable anterior al cambio también falló el rechazo413 de POST2048bytes con
errorRead del cliente. Cierre inmediato SD_BOTH puede resetear TCP con datos pendientes.
Candidato tras respuesta Connection:close hace SD_SEND y drenaje no bloqueante acotado
100ms/1MiB, polls10ms, cancelable. Nunca espera todo el body anunciado. Patrón
contrastado con Microsoft y notas de httplib; dependencia conservada0.46.0.

Gates nativos pasan, incluidos cliente pausado350ms (>poll100ms), 413, 409 sin body
16MiB anunciado, cancelación, offsets/checkpoints/cortes/rename/bytes y perfil asociado
al ACK. Cierre parcial0–100ms. Objetos UWP network_portal/nsp_upload pasan. Smoke
32MiB ~153–155MiB/s loopback: no A/B causal ni mejora certificada en Series.
Último bloque16MiB ejemplo: total100650us, Write4901us, Flush31679us, Checkpoint37427us,
1025callbacks. Datos PC, no inferir costes Xbox. Evidencia perf-before.txt (fallo
previo), perf-after.txt, perf-repeats.txt en test-results/network-portal.
Entregado para prueba en APPX0.3.4.1 a petición del usuario, sin commit/push. Build
incremental/link correctos, smoke AppContainer PC llega a biblioteca y proceso21104
Responding; no certifica gameplay ni recorrido web Series. Paquete firmado y verificado
en build-uwp/package/jadyx.appx, solo library=1/play=1, sin gate ni datos de usuario.
Exe/PDB en symbols/0.3.4.1; diag/log y registro de empaquetado/hash en test-results/network-portal.
Mantener16MiB de inicio para comparar con la muestra anterior; probar64 después con
?chunk=64 y un nombre nuevo. Mejora sostenida Series todavía pendiente.

Fuentes contrastadas:
- [Microsoft: recv](https://learn.microsoft.com/windows/win32/api/winsock/nf-winsock-recv).
- [Microsoft: FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers).
- [Microsoft: cierre graceful](https://learn.microsoft.com/en-us/windows/desktop/WinSock/graceful-shutdown-linger-options-and-socket-closure-2).
- [cpp-httplib: cierre acotado](https://github.com/yhirose/cpp-httplib/releases).
- [Cisco: Wi-Fi compartido](https://www.cisco.com/c/en/us/td/docs/wireless/controller/9800/technical-reference/wireless-rf-reference-guide.html).

## Conexión y pantalla Xbox

NetworkPortal pertenece a ShowGameLibrary. Inicio manual, parado por defecto y
antes de jugar/suspender/salir; snapshots protegidos por mutex. Inicio/parada/renovación
serializados. Capacidad privateNetworkClientServer, internetClient conservada. Se
liga IPv4 concreta RFC1918/link-local, puerto8080, uso exclusivo. GetAdaptersAddresses
enumera Ethernet/Wi-Fi operativas con gateway primero; no se usa NetworkInformation,
limitado en Xbox. Solo los gates admiten loopback. Conexión/emparejamiento LAN Series
confirmados por el usuario.

Código 4 dígitos CSPRNG/5min consumido al emparejar, sesión256bits en RAM. Cinco intentos
por minuto globales; renovar no reinicia limitador. No código/sesión en logs, URLs o
almacenamiento persistente del navegador. HTTP LAN sin cifrado, sin UPnP ni CORS abierto;
Host exacto IP:puerto, Origin exacto cuando existe y obligatorio al emparejar. CSP,
no-store/no-referrer/nosniff. Sin explorador arbitrario de archivos ni descarga de claves.

El usuario no veía el código: fila8 fuera de7 filas visibles. Iniciar ahora abre página
completa: URL, código112px agrupado4+4, Volver/Generar/Detener, mando/teclado/puntero con
foco exclusivo y sin fondo animado. Volver mantiene conexión; fila activa reabre página.
Caducidad/consumo muestran estado. La pantalla añade recepción/progreso del NSP.

## Contrato HTTP

Cuatro workers/cola8, keepalive2s/10000requests, TCP_NODELAY. No buffers TCP gigantes
ni cambios al autotuning. Pairing≤1024bytes, controles≤4096, PATCH≤64MiB; no aceptar
Transfer-Encoding ni Content-Encoding. Ids aleatorios32hex y destinos decididos por servidor.

- GET /: HTML/CSS/JS locales es/en, sin CDN ni framework.
- POST /api/v1/pair: JSON code, Origin obligatorio, sesión una vez.
- GET /api/v1/status: Bearer, stage=nsp, uploads=true, snapshot de transferencia/espacio.
- OPTIONS/POST /api/v1/uploads: tus1.0 creation; Upload-Length y Upload-Metadata con
  filename/fingerprint Base64 de texto (nunca Base64 del juego).
- HEAD /api/v1/uploads/{id}: offset confirmado/longitud; GET equivalente sin body.
- PATCH .../{id}: ContentReader, Content-Length acotado, Upload-Offset estricto,
  application/offset+octet-stream;204 tras escritura/checkpoint.
- DELETE .../{id}: termination; parcial/journal, nunca un juego instalado.
- POST .../{id}/finish, /pause, /rename: extensiones propias; validar, pausar sin borrar
  y resolver colisión. No anunciar extensiones tus adicionales.

Sin directorio configurado, harness conserva modo connection-test sin subidas. Error
al preparar almacenamiento hace fallar Start de forma controlada. No tumbar la app.

## Ruta rápida de NSP

Browser usa Blob.slice y bloques16MiB; una subida activa y cola. Progreso recibido y
confirmado separados, velocidad de los últimos8 bloques confirmados y ETA de recepción
(sin validación). Reintenta cortes consultando HEAD. Pausa, elimina parcial o renombra.
Después de reiniciar app/browser: emparejar y seleccionar el mismo archivo para reanudar.

Servidor agrupa recv pequeños en buffer1MiB y WriteFile secuencial. FlushFileBuffers
una vez por bloque HTTP, no por paquete. Datos se flushean antes del journal. Mantener
buffering del sistema: no-buffering requiere alineación/restos y no garantiza ventaja.

Dos checkpoints con generación/tamaño/offset/identidad de archivo y volumen. Recuperar
la generación válida y truncar bytes no confirmados. Checkpoint roto conserva anterior.
Antes de publicar se guarda intención; MoveFileFromApp sin replace renombra al destino,
sin segunda copia completa. FileId reconoce rename completado aunque falte el último
checkpoint. No certificar pérdida de alimentación sin prueba específica.

Destino LocalState/games; parcial fuera del escáner en LocalState/jadyx/transfers/nsp.
Nombre Windows/UTF8 estricto, sin rutas/dispositivos, ≤100 unidadesUTF16/200bytesUTF8.
Longitudes/offsets uint64, límite1TiB. Comprobar espacio completo+64MiB de reserva al crear
y espacio de cada bloque; no reservar físicamente todo el archivo. Free space es la
consulta del filesystem; cuotas específicas de Xbox todavía sin certificar.

Huella SHA256 de nombre/tamaño/mtime+primeros/últimos64KiB. JS fallback probado porque
WebCrypto no existe en HTTP LAN inseguro. Es muestreo, no hash íntegro: cambios en medio
que conserven metadatos/extremos no se detectan por esta huella. No afirmar integridad
criptográfica completa por fingerprint/TCP ni por la validación estructural siguiente.

Hasta pausar/completar, excluir gameplay y mutaciones locales. Al terminar, coordinador
cancela/join metadatos y valida en worker MTA; solo después rename/complete/rescan. Stop
cancela recepción/validador y preserva offset. No cambia JIT/cachés/presupuesto de memoria.

Validador prefiltra PFS0/count≤4096 antes de construir PartitionFilesystem. Usa NcaReader,
NCA(meta) y CNMT existentes: Application con Program, referencias/tamaños/tipos/titleIds
coherentes y ningún NCA extra. Rechaza update/DLC y mezclas. Lee headers/metadatos, no
relee todo el payload ni compara todos los hashes CNMT. No importa tickets ni muta
KeyManager: el loader normal del NSP procesará sus tickets al arrancar.

## Trampas de sockets y cierre

En httplib0.46.0 Server::~Server es default; stop solo cierra si ya corre. Reservar un
puerto con bind_to_any_port sin escuchar deja socket vivo: primer harness conectaba al
listener abandonado, errorRead. Cambiado a reserva Winsock cerrada. Servidor derivado
posee listener ligado incluso si fallan hilo/pool. Decommission libera wait_until_ready
si arranque falla. SO_EXCLUSIVEADDRUSE evita compartir silenciosamente puerto ocupado.

Registro de sockets activos: shutdown al detener y cola descartada; quitar/cerrar bajo
mismo mutex impide tocar descriptor reciclado. Override HTTP plano ligado a0.46.0.
WSAPoll Windows puede no despertar al shutdown local: se observaron3895ms con waits2s,
1901–1910ms con recv1s. PortalStream nuevo mantiene polls100ms y espera idle cancelable30s;
cierre observado0–114ms en harness, sin certificar aún suspensión real Xbox.

0.46.0 intenta drenar body incluso después de respuesta Connection:close. PATCH rechazado
por offset/disco puede no enviar el resto. Hook por request en worker (TLS con scope)
corta lecturas después de esa respuesta y fuerza cerrar ese socket, sin afectar otros.
Fixture TCP de offset inválido/body16MiB ausente exige409 y cierre inmediato. Revisar
expresamente estos overrides al actualizar httplib.

## Comandos y validación pendiente

Pruebas pequeñas, sin keys/firmware/juegos en repo:

```powershell
& tools/xbox/tests/run-network-portal.ps1
& tools/xbox/tests/run-frontend.ps1 -Tests localization-catalogs
node tools/xbox/tests/network-portal-page.cjs
```

Harness sintético usa callback falso: prueba bytes/reanudación/publicación, no CNMT real.
Fixtures solo en build-uwp/test-results/network-portal, con limpieza confinada al pasar.
Prueba pesada opcional del usuario (transfiere por TCP y compara todos los bytes5GiB):

```powershell
.\build-uwp\test-results\network-portal\network-portal.exe --large
```

Build amplio y apertura biblioteca PC, a cargo del usuario:

```powershell
tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target jadyx
powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 -NoBuild -Library
```

Desde móvil/PC, conectar, elegir NSP propio y nombre, Subir/reanudar. Interrumpir Wi-Fi;
reiniciar app y navegador, emparejar y reseleccionar archivo. Comprobar biblioteca y
arranque, colisiones, cancelación y pausas. Medir memoria/CPU, recepción, validación y
publicación por separado. No usar estas pruebas para certificar FPS.

Desktop sintético observó143,09/134,03 y116,56MiB/s con32MiB (última ventana0,27s,
cierre84ms). Son smoke tests muy cortos, loopback/mismo proceso/archivo sintético y
con compilaciones concurrentes: no son A/B ni tasa sostenida Xbox/LAN. Series cableada: mismo NSP/red/consola, abrir
http://IP:8080/?chunk=4 (o16/64), tres transferencias completas por tamaño, nombres distintos
o retirar el anterior externamente. Elegir mejor mediana; conservar16MiB si ventaja<5%.

Entrega consola 0.3.3.0 en paquete único build-uwp/package/jadyx.appx, biblioteca sin
diagnósticos activados y símbolos por versión. Gate de importación/rendimiento Series pendiente.

## Gate con NSP real en AppContainer (19 octubre)

Opt-in `network_import_gate=1`, sin efecto en la biblioteca normal. Revisa los NSP de
LocalState/games con el validador de producción y elige el menor válido >4GiB. Transfiere
por HTTP loopback a una carpeta exclusiva LocalState/network-import-gate-*, bloques16MiB,
buffer cliente1MiB y buffer receptor1MiB. Tras pasar4GiB destruye servidor/cliente,
reconstruye el store desde los checkpoints, empareja de nuevo y confirma HEAD antes de
continuar. Finaliza con ValidateUploadedNsp y rename; compara todos los bytes con el original,
comprueba tamaño/mtime del origen y elimina solo su fixture al pasar. No añade el juego
a la biblioteca de uso normal. La comparación completa existe solo en el gate.

Primera corrida: cinco de ocho pasan. Dos rechazos eran XML auxiliar .cnmt.xml, ahora
admitido sin interpretar (≤4MiB, id hexadecimal y debe corresponder a un NCA Meta
presente). CNMT cifrado sigue siendo la autoridad. La cuarta fuente sigue `incomplete`;
su Meta tiene3616bytes. No se relajaron las comprobaciones de tamaños ni se cambió el
archivo del usuario. Después de corregir XML, siete de ocho fuentes pasan.

La primera transferencia15.237.759.824bytes recuperó offset4.311.744.512, pero Windows
suspendió todos los hilos en segundo plano; al reactivar expiró la recepción y el gate
falló. Después del retorno15 hubo error WinRT durante cierre (0x80010012 y excepción
C++ no controlada). Evidencia pc-real-nsp-suspended-{diag,log}.txt; no certifica velocidad
ni suspensión completa de la app. Producción sí instala su manejador de suspensión en
ShowGameLibrary; el gate es anterior a esa pantalla.

Segunda corrida mantuvo la app activa mediante reactivación periódica. Mario Kart8 base,
7.268.474.425bytes (6,769GiB): recepción62,130494s,111,567717MiB/s incluyendo recreación
de servidor; publicación/validador real correctos, comparación íntegra idéntica, original
intacto, fixture eliminado y RunHeadlessBoot returned0 a94,703s. No es una comparación
4/16/64 ni una medición LAN/Series. Uso WinRT estable ~61MiB; pico privado muestreado
desktop61.333.504bytes y peak working set30.023.680bytes, métricas distintas del uso
WinRT. Sin guest ejecutándose. Reinicio de instancia no equivale a reiniciar todo el proceso.

Evidencias ignoradas en build-uwp/test-results/network-portal/pc-real-nsp-{diag,log}.txt
y pc-real-nsp-memory.json. Reproducir en PC con Jadyx en primer plano:

```powershell
powershell -ExecutionPolicy Bypass -File tools/xbox/local-run.ps1 -NoBuild -BootCfg network_import_gate=1 -TimeoutSec 240
```

El comando restagea un paquete de diagnóstico: después volver a empaquetar con
`powershell -ExecutionPolicy Bypass -File tools/xbox/package-appx.ps1 -Library` para
entregar biblioteca normal. En Series probar desde navegador real, verificar biblioteca
y arranque, cortes de red/reinicio completo y medir bloques4/16/64 con tres repeticiones.

## Claves y firmware web: candidato del 19 de octubre de 2026

Rama `feature/portal-system-imports`, sin commits nuevos ni push. El usuario informa
que 0.3.4.1 mantiene la misma velocidad que 0.3.4.0 (~14.8 MiB/s), con PC y Xbox
ambos por Wi-Fi. No hay mejora de transferencia Series demostrada.

`SystemUpload` reutiliza el transporte durable por bloques, con un lote exclusivo
de claves o firmware. El servidor fija todos los destinos. Recibe ZIP o archivos
sueltos (el navegador también puede seleccionar carpeta de NCA), recupera archivos
terminados y offset confirmado, y requiere revisión nueva después de reiniciar.
Los NCA sueltos pasan al staging por rename; ZIP se extrae allí, sin copia posterior
del firmware completo. Las claves individuales conservan el límite de 4 MiB;
ZIP de claves 16 MiB comprimido/8 MiB extraído; firmware 4/8 GiB y 4096 NCA.

`system_import` comparte validación/publicación entre USB y portal. La biblioteca
detiene y une sus workers de metadatos antes de los lectores core/recarga de claves.
La revisión mantiene el sistema instalado intacto; confirmación explícita antes de
publicar. Los ficheros preparados se vacían a disco antes del cambio de directorio.
Rollback y recuperación usan `.previous`. Un fallo al limpiar temporales después
de publicar no convierte una instalación completada en un fallo. Suspensión devuelve
los archivos sueltos preparados a la entrada recuperable y exige revisión otra vez.

Se obtiene la versión de `/file` (256 bytes) del título SystemVersion mediante los
lectores reales de NCA/RomFS, compara major/minor/micro y revisiones, y exige MiiEdit.
Las claves no tienen una versión de firmware oficial: se muestran índices de generación,
se advierte si desaparece una generación declarada aunque el índice máximo no cambie,
y se comparan header/key-area efectivos contra el material que abrió el firmware actual.
No se recarga KeyManager para previsualizar. Firmware que no puede validarse con las
claves instaladas se rechaza con indicación de importar claves compatibles primero.
La advertencia de incompatibilidad de claves puede confirmarse; un NCA inválido no.

Libarchive usa ZIP seekable exclusivamente, con callbacks de lectura/seek de 64 KiB:
el build existente es de solo lectura y no incluye `archive_read_open_filename_w`.
No se debe añadir un opener inexistente ni aceptar el fallback ZIP streamable para
un archivo truncado sin directorio central. Extracción con buffer de 1 MiB, límites
de bytes declarados y efectivos, espacio libre por bloque; rechaza rutas peligrosas,
enlaces, cifrado, nombres duplicados y archivos ajenos al tipo seleccionado.

Verificación inicial: objetos UWP nuevos y frontend compilados; harness nativo pasa
reanudación/reinicio de lote, paso por extracción ZIP simulada, advertencias explícitas,
publicación y cancelación sin tocar destino. Navegador simulado pasa cuatro combinaciones
claves/firmware × ZIP/sueltos y confirmación de advertencias, además de los escenarios NSP.
Los mocks no certifican Xbox. Gate real AppContainer completado: NCA/SystemVersion
instalados, ZIP de claves y firmware con carpetas internas, rechazo de ZIP truncado,
traversal y duplicados, advertencias por pérdida de generaciones/material incompatible,
claves inválidas, cancelación y recuperación de publicación. Retorno0 a1.594s; uso
WinRT59MiB. Se detectó y corrigió el uso de separadores mezclados al pasar la ruta
de staging a `StorageFolder::GetFolderFromPathAsync`: normalizar con `make_preferred`,
como ya hacía el importador USB. Los errores esperados del gate aparecen como
first-chance; el cierre aún registra el conocido0x80010012 después del retorno0.
Evidencia: `build-uwp/test-results/network-portal/pc-system-import-{diag,log}.txt`.

Compilación incremental UWP y gate de catálogos es/en/pt-BR correctos. Harness TCP
autenticado también pasa los endpoints reales de lote, exclusión NSP y rechazo de
confirmación sin aceptar advertencias (validador simulado). Entrega **0.3.4.3**,
solicitada por el usuario: APPX firmado en `build-uwp/package/jadyx.appx`, biblioteca
normal, sin fixtures ni diagnóstico en `boot.cfg`; símbolos en `symbols/0.3.4.3`.
Pendiente Series: portal real PC/móvil, firmware completo y anterior, pares de claves
antiguas/nuevas, desconexión/reinicio/suspensión y confirmación visual de avisos.

Comandos de los gates:

```powershell
powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-network-portal.ps1
powershell -ExecutionPolicy Bypass -File tools/xbox/tests/run-frontend.ps1 -Tests localization-catalogs
powershell -ExecutionPolicy Bypass -File tools/xbox/local-run.ps1 -NoBuild -BootCfg file_manager_gate=1
```

El gate de archivos crea únicamente una copia privada de claves y dos NCA ya instalados
en LocalState; no cambia los originales ni los incorpora al paquete o repositorio.

### Límites previstos y servicio compartido

Servicio común USB/red de validación/publicación. prod.keys y title.keys opcional≤4MiB
cada uno. ZIP≤4GiB, extraído≤8GiB y≤4096NCA; libarchive en consola y staging directo de
instalación. Admitir carpetas, rechazar rutas peligrosas/enlaces/cifrado/duplicados;
medir bytes reales/espacio/cancelación. Mantener NCA/SystemVersion/MiiEdit y rollback,
join metadatos antes de publicar/reload claves. Portal añadirá esas dos tarjetas;
actualizaciones/DLC quedan en su gestor actual.

## Fuentes primarias contrastadas

- [Microsoft: sockets UWP](https://learn.microsoft.com/en-us/windows/uwp/networking/sockets).
- [Microsoft: limitaciones Xbox](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox).
- [cpp-httplib0.46.0](https://github.com/yhirose/cpp-httplib/blob/v0.46.0/README.md), contrastado con header local.
- [tus1.0](https://tus.io/protocols/resumable-upload), core/creation/termination.
- [Microsoft: FlushFileBuffers y coste de llamadas frecuentes](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers).
- [Microsoft: buffering y alineación no-buffering](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering).
- [libarchive](https://www.libarchive.org/), versión local3.8.9 para fase siguiente.
- [libarchive: contrato del lector y callbacks](https://github.com/libarchive/libarchive/blob/master/libarchive/archive.h).
- [SwitchBrew: SystemVersion y estructura del fichero](https://switchbrew.org/wiki/System_Version_Title).
- [SwitchBrew: criptosistema](https://www.switchbrew.org/wiki/Cryptosystem) y [generación de claves en 17.0.0](https://www.switchbrew.org/wiki/17.0.0).
- Código propio: uwp_file_manager, uwp_library, PartitionFilesystem, NcaReader/CNMT,
  content_inspector (omite Application por diseño), mod_archive y network_interface.

Corrección posterior de interfaz 0.3.4.3: el título de claves y el error compartían
la propiedad JS keys; la segunda definición sustituía el título aun sin archivos.
Se separa keysTitle y se comprueba el título inicial en es/en/pt-BR para las cuatro
combinaciones claves/firmware ZIP/sueltos. Gate navegador pasa; texto redundante
de privacidad eliminado del error. Recompilación incremental y paquete de la misma versión.

Guardado solicitado por el usuario: portal separado en commits locales de transporte,
validación compartida, lotes recuperables, API, integración UWP, página web y pruebas.
La solución ajena en d3d12_query_cache.cpp queda sin incluir; no se hace push.

## Descarga completa de logs y capturas — 0.3.6.16 (20 octubre 2026)

El usuario confirma que la captura .15 termina correctamente y solicita exportar
la carpeta log completa porque la descarga de Device Portal no funciona. La nueva
opcion del portal de Jadyx es **Comprimir y descargar logs**; se usa desde el PC
emparejado, manteniendo la app en biblioteca. La actualizacion sobre la instalacion
existente conserva LocalState y las capturas anteriores; no es necesario repetirlas.

La biblioteca configura el root mediante GetEdenPath(LogDir), independiente de la
version del paquete. Incluye archivos binarios y texto, subdirectorios, vacios y
trace/full_*. No explora otras carpetas ni acepta rutas del navegador. Rechaza
reparse points/enlaces y archivos que no puede leer; no anuncia un archivo listo
si hubo un fallo. Tamaños de archivos fijados al inventariar: un log que siga
creciendo aporta su prefijo inventariado; truncados/lecturas cortas fallan. No es
una instantanea atomica entre todos los archivos. Capturas cerradas son inmutables.

Formato interoperable PAX tar + Zstandard, usando zstd estatico existente, sin
agregar dependencias. Nivel15, ventana8MiB, hashLog21/chainLog22, un worker de
compresion, checksum de frame habilitado. Se verifica una estimacion de memoria
del compresor <=64MiB; buffers de lectura1MiB y salida ZSTD_CStreamOutSize, mas
el inventario de rutas. No se carga un archivo completo ni el archivo descargado
en un Blob del navegador. No se promete un ratio para datos ya comprimidos.

El worker mantiene scanning/compressing/ready/failed/cancelled y progreso de bytes,
archivos y tamaño comprimido. Crea LocalState/jadyx/transfers/log-export/logs.partial
y publica logs.tar.zst solo al cerrar completamente. Esa ruta queda fuera de log,
no se incluye a si misma. Conserva un unico archivo preparado para reintentos;
la siguiente preparacion lo sustituye. Reserva32MiB de disco, inventario<=100000
entradas y contenido<=1TiB; comprueba espacio durante la escritura. Cancelar/parar
el portal pide stop y une el worker, eliminando el parcial, sin modificar los logs.

API, con los mismos controles Host/Origin/emparejamiento del portal:
- POST /api/v1/logs/export: 202, preparacion asincrona; 409 si ya esta ocupada.
- DELETE /api/v1/logs/export: solicita cancelacion.
- GET /api/v1/status: campo logs con progreso/estado; no devuelve secretos/rutas.
- POST /api/v1/logs/download: archivo listo -> ticket aleatorio, URL y nombre.
- GET/HEAD /api/v1/logs/archive?ticket=...: descarga nativa en bloques64KiB;
  Content-Disposition y Content-Length; Range/206/416 con proveedor de httplib.

Ticket de alcance exclusivo a logs,30min, reutilizable para Range/reintentos;
nueva preparacion, nuevo emparejamiento o Stop lo revocan. Durante una respuesta
se retiene handle readonly sin compartir escritura/borrado: no puede sustituirse
el archivo bajo un lector activo. El enlace no transporta el Bearer de sesion.
Pagina es/en/pt-BR con progreso/cancelacion y descarga automatica una vez lista,
mas enlace Guardar archivo si el navegador no inicia automaticamente.

Validacion completada:
- Harness TCP nativo: pairing/auth, no listo, un worker, descarga zstd completa,
  Range exacto/HEAD/416, revocacion, cancelacion sin parcial y reinicio; conserva
  los gates anteriores de actualizacion, subidas NSP y lotes de sistema.
- Python tarfile contrasta todos los bytes y nombres, rutas UTF-8 largas para
  PAX, archivos/carpetas vacios y exclusion del archivo externo sintetico.
- Navegador simulado es/en/pt-BR: progreso5GiB, cancelacion/error y descarga
  nativa unica, sin leer el archivo como Blob. Suite previa de pagina pasa.
- UWP incremental/link correctos. Gate real AppContainer HTTP/range/zstd,
  3archivos sinteticos/1048607bytes ->1502bytes, retorno0 a187ms; extraccion
  posterior tarfile y comparacion integra correctas. Este ratio es sintetico,
  no una medida de las capturas reales ni certificacion Series/LAN.

Comandos: tools/xbox/tests/run-network-portal.ps1,
node tools/xbox/tests/log-export-page.cjs,
node tools/xbox/tests/network-portal-page.cjs.
Opt-in AppContainer log_export_gate=1, ausente del paquete final. Evidencias:
build-uwp/test-results/log-export-{native-gate,build-gate,package,package-verification}.txt
 y network-portal/pc-log-export-gate. Pendiente descarga real Series al PC.

Entrega diagnostica0.3.6.16, biblioteca/play/captura menu/full, fastmem0, sin
payload/gates/self-test/debug/cap PC: build-uwp/package/jadyx.appx.
SHA256 5C50A80140BC8069C19E808CB9CB4CF381B9D5617DFC8E991C6AF122E9D20490.
Firma/payload/container verificados, simbolos symbols/0.3.6.16. Sin commit/push.

Trampas del gate: Python Windows usa cp1252 por defecto; editar UTF-8 con encoding
explicito para no mezclar codificaciones en el HTML. Fixture PAX largo se mantiene
dentro de MAX_PATH del harness desktop; no certifica rutas Windows >260 caracteres.
El batch comprueba errorlevel distinto de0, incluidos codigos negativos de excepcion;
el main del harness informa excepciones en vez de un terminate opaco.

Fuentes primarias: [Zstandard streaming y parametros](https://facebook.github.io/zstd/zstd_manual.html),
[cpp-httplib: proveedores de contenido](https://github.com/yhirose/cpp-httplib#send-content-with-a-content-provider).
Libarchive configurado en este proyecto solo para lectura; por eso el escritor
PAX usa el contenedor estandar y se valida con un lector independiente.

## Compresion multihilo y descarga en streaming de logs — 0.3.6.17+ (20 octubre 2026)

El portal solo exporta con la consola quieta en modo red, asi que la compresion usa
todos los nucleos. Sustituye la configuracion de .16 (nivel15, un worker, 64KiB):
- Zstd multihilo (ZSTD_MULTITHREAD ya compilado; hilos _beginthreadex validos en UWP):
  un worker por hardware_concurrency, max16, recortado a ~256MiB totales (estimacion
  monohilo + 3 jobs por worker). Si nbWorkers no se acepta, sigue monohilo.
- Nivel7 (lazy), ventana4MiB, overlapLog6; jobs = total/(nucleos*4) entre1 y8MiB para
  que una carpeta pequeña tambien reparta trabajo. Checksum de frame igual.
- Lectura 4MiB, salida 1MiB, espacio libre consultado cada16MiB con margen48MiB
  (la reserva de32MiB se mantiene entre consultas).

Descarga sin esperar al final: POST /api/v1/logs/download se concede tambien mientras
comprime (campo streaming). El ticket queda ligado a la ejecucion (run) de esa
exportacion. GET del archivo en curso responde chunked siguiendo el parcial: el
escritor hace flush antes de publicar compressed y notifica una condition variable.
El renombrado parcial->final ocurre bajo el mismo lock en que el lector elige archivo.
Un fallo/cancelacion corta la conexion; nunca entrega un archivo truncado como completo.
Range mientras comprime -> 409 (se reintenta cuando esta listo). Ya listo: longitud
fija, Range/206, ETag fuerte para reanudar. Lecturas posicionadas de1MiB (OVERLAPPED,
sin puntero compartido) y SO_SNDBUF4MiB en el socket de la descarga. El nombre del
archivo se fija al iniciar. Pagina: la descarga nativa arranca junto con la exportacion;
el enlace Guardar archivo permanece para reintentar.

Validacion: harness TCP nativo con streaming real (chunked) byte-identico al archivo
final, ETag, Range/HEAD/416, cancelacion tolerante a la carrera con el export rapido
(reintenta hasta cancelar; cada intento acaba en cancelled o ready), 5/5 ejecuciones;
PAX tarfile pasa; UWP compila/enlaza. Test de pagina log-export-page.cjs actualizado,
no ejecutado (sin node en este PC). Pendiente medir tiempo/tamaño reales en Series.

## Biblioteca, actualizaciones/DLC y eliminación — 0.3.6.30 (20 octubre 2026)

Rama feature/portal-library-content. La pestaña Biblioteca del portal aparece cuando la
consola publica su vista (PortalLibrarySource, solo lectura, generación por escaneo).
- GET /api/v1/library: {revision, games:[{id,name,developer,version,removable}]}; id 16 hex.
- GET /api/v1/library/{id}/cover: JPEG (img-src 'self' blob:); id mal formado -> 404 sin
  llegar a la vista.
- GET /api/v1/library/{id}/content: {contents:[{id,kind,name,version,title,bytes,enabled,internal}]}.
- tus /api/v1/content/uploads: metadata filename/fingerprint + kind (update|dlc) +
  application (16 hex). kind inválido -> 400 metadata; juego desconocido -> 404. Al terminar,
  el worker de almacenamiento llama a FinalizeContent con tag "<kind>:<id>" y la importación
  es la del gestor de contenido (ImportUploadedContent: copia interna, base presente, sin
  downgrade, paquete rechazado entero si trae contenido de otro juego).
- DELETE /api/v1/library/{id} y DELETE /api/v1/library/{id}/content/{64 hex}: 202 {removal};
  404 game; 409 busy (subida abierta, importación de sistema u otra eliminación).
  status.removal {serial,game,content,phase,error}: pending -> running -> complete|failed.
  Mientras hay una eliminación pendiente las subidas quedan cerradas.

Eliminación compartida consola/portal (mismo worker serie, mismas funciones):
- Opciones del juego -> Eliminar juego (segunda pulsación confirma) o portal -> juego ->
  Eliminar juego (dos pulsaciones). RemoveGame borra dumps internos, partidas, updates/DLC
  del catálogo y sesiones Pulse; conserva mods/trucos. Juegos fuera del almacenamiento
  interno -> error external (no se borra nada).
- Update/DLC individual: gestor de contenido de la consola o botón por elemento del portal
  (RemoveContent). Tras cada trabajo se reescanea; la generación del catálogo forma parte de
  la clave de metadatos, así la versión mostrada y ResolveLaunchContent ven el cambio.

Validación: harness TCP nativo (library-tests.h, vista sintética: listado, carátula,
admisión kind/juego, tag al instalador, cola de eliminación, exclusión de subidas, fallo
visible); test de página network-portal-page.cjs (es/en/pt-BR, mock HTTP, ejecutado con
Electron de VS Code en modo node); UWP compila/enlaza. Pendiente: gate en Series con
update/DLC reales y eliminación desde ambos lados.
