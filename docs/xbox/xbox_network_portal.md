# Portal local de importación

## Estado: 19 octubre 2026

Rama `feature/local-import-portal` desde main, con commits locales autorizados y sin push.
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

## Conexión y pantalla Xbox

NetworkPortal pertenece a ShowGameLibrary. Inicio manual, parado por defecto y
antes de jugar/suspender/salir; snapshots protegidos por mutex. Inicio/parada/renovación
serializados. Capacidad privateNetworkClientServer, internetClient conservada. Se
liga IPv4 concreta RFC1918/link-local, puerto8080, uso exclusivo. GetAdaptersAddresses
enumera Ethernet/Wi-Fi operativas con gateway primero; no se usa NetworkInformation,
limitado en Xbox. Solo los gates admiten loopback. Conexión/emparejamiento LAN Series
confirmados por el usuario.

Código8 dígitos CSPRNG/5min consumido al emparejar, sesión256bits en RAM. Cinco intentos
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

## Siguiente: claves y firmware web

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
- Código propio: uwp_file_manager, uwp_library, PartitionFilesystem, NcaReader/CNMT,
  content_inspector (omite Application por diseño), mod_archive y network_interface.
