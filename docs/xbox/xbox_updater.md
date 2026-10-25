# Actualizaciones de Jadyx

## Estado y alcance — 19 de octubre de 2026

Rama `feature/self-updater`. Candidato 0.3.6.0, sin commit ni push. Implementa
consulta de releases, manifiestos firmados, descarga reanudable, pantalla en
biblioteca y asistencia de instalación desde navegador. **No certifica instalación
interna en Xbox**, reinicio automático ni superioridad global frente a NXbox.

Por instrucción del usuario, la ruta principal ahora está conectada a
`PackageManager.AddPackageAsync` con `ForceApplicationShutdown` y capability
`packageManagement`. **Actualizar** descarga/verifica y solicita el despliegue
sin segundo botón cuando la admisión de almacenamiento está libre. Es un candidato
habilitado para probar; todavía no está certificado en Series ni se promete
reapertura por el solo hecho de haber enviado la solicitud.

La asistencia móvil/PC queda como recuperación ante rechazo del sistema. No pide
ni almacena credenciales del Device Portal. El backend de portal interno y su
asistente de credenciales no son necesarios para la vía nativa principal.

## Qué hace NXbox y diferencias verificables

Se revisaron `src/eden_uwp/ui/updater.cpp`, `update_version.h`,
`docs/nxbox-updates.md` y el workflow de releases de NspxMiguel/NXbox.
Consulta `/releases/latest`, compara cuatro componentes, selecciona el asset por
nombre y descarga en bloques de 256 KiB a TemporaryFolder. Valida tamaño y usa
HTTPS Basic/CSRF/multipart contra Device Portal, configurado mediante `portal.json`.
Reintenta HTTP 409 hasta 30 veces. La aceptación del POST no verifica el reemplazo.
Su documento reconoce el riesgo de loopback y la ausencia de validación en consola.
No se atribuyen resultados de ejecución no observados.

Jadyx añade selección estable/pruebas, manifiesto firmado, hash SHA-256,
reanudación con ETag fuerte/If-Range, estado persistente, pausa/cancelación,
velocidad/ETA, posponer 24 horas y omitir una versión. El envío aceptado nunca se
presenta como actualización confirmada. La confirmación exige que la identidad del
paquete instalado tenga la versión objetivo al siguiente arranque.

## Diseño e interfaces

- `Updates::Updater`: propiedad de la biblioteca; worker cancelable MTA,
  `Status`, `Check`, `Download`, `Pause`, `Cancel`, `SetChannel`, `Dismiss`,
  `Install`, `VerifiedPackage` y `RecordAssistedRequest`.
- `Snapshot` copia el estado, error, release, progreso, velocidad, fecha del último
  check correcto e historial acotado a 20 eventos. El render no realiza red.
- `network/http` contiene el transporte compartido. `Mods::Web` es un alias
  compatible; el comportamiento MD5 de mods se conserva. El actualizador usa
  `Fetch` acotado y `DownloadVerified`, separados de la política MD5 anterior.
- `LocalState/updates/state.json` se reemplaza con ReplaceFileFromAppW o
  MoveFileFromAppW. Conserva manifiesto exacto, firma hexadecimal, canal,
  último intento automático, fecha del check correcto, omisión y objetivo.
  No contiene credenciales. Las descargas parciales llevan identidad y ETag.
- La consulta automática se limita a una cada seis horas entre arranques;
  `Buscar ahora` fuerza comprobación. Se recorren páginas de releases, con ETag,
  hasta 1000 releases; superar ese límite informa fallo, no un resultado completo.
- Se rechazan drafts, manifestaciones incompatibles y versiones iguales/antiguas.
  Pruebas acepta estable o preview, siempre la versión numérica superior.
  Cambiar a Estable nunca realiza downgrade. Releases históricas sin metadata
  permanecen disponibles manualmente.
- `update.json` UTF-8 exacto + `update.sig` RSA PKCS#1 SHA-256 binario. La clave
  pública fijada es la del certificado JadyxDev existente, no una clave nueva.
  Solo la clave pública se incluye en el código. La rotación requiere una entrega
  manual previa que incorpore la nueva confianza; no se acepta una clave de la red.
- Schema 1: version/tag de cuatro componentes, channel stable/preview,
  identity Name=Jadyx/Publisher=CN=JadyxDev/x64, min_os=10.0.17763.0,
  vclibs_min=14.0.33519.0, diagnostic=false, URL exacta del asset de nuestro fork,
  size hasta 4 GiB, SHA-256 hexadecimal, notes y published. Nuevos requisitos
  desconocidos requieren actualizar el cliente, no una aceptación implícita.
- La descarga hace streaming con 256 KiB, valida tamaño/rangos y verifica hash
  completo con 64 KiB. Compara además identidad/versión del AppxManifest real y
  exige AppxSignature.p7x. La confianza/firma APPX y dependencias finales las
  valida Windows durante el despliegue; no se afirma verificar toda la firma APPX
  en el cliente antes del despliegue.

## Recorrido y convivencia

Ajustes → Sistema → Actualizaciones. Consulta, canal, actualizar/reanudar,
pausar, cancelar, preparar instalación, recordar mañana y omitir versión.
Textos en español, inglés y portugués de Brasil. El aviso de biblioteca es discreto.
Las notas muestran un resumen y un lector completo con desplazamiento;
el historial tiene vista propia y conserva los eventos originales en el journal.

La pantalla entrega instantáneas al canvas. Salir hacia otra acción pausa la
descarga antes de despachar esa acción. La suspensión solicita cancelación y une
el worker. Al abandonar la biblioteca, la destrucción une el worker antes de
arrancar el guest. El coordinador usa la admisión del portal antes de descargar,
cancelar o preparar instalación; las subidas se deshabilitan durante descarga,
verificación y preparación.

Preparar instalación registra el objetivo y ofrece asistencia. Se inicia la
conexión de red existente en Gestor de archivos, se empareja el navegador y se usa
Actualizar Jadyx en su página. `POST /api/v1/update/download`, autenticado con la
sesión existente y controles Host/Origin, entrega un ticket aleatorio de dos
minutos, de un solo uso, para `GET /api/v1/update/package`. La respuesta transmite
el archivo en bloques de 64 KiB y mantiene un handle sin compartir escritura o
borrado. El token de sesión no aparece en la URL. No hay descarga pública.

Desde móvil/PC, el usuario guarda el paquete y lo instala por Device Portal
sobre Jadyx, sin desinstalar. El navegador ofrece un enlace HTTPS al Device Portal del mismo host. No consulta
el resultado remoto ni almacena sus credenciales.
Si el próximo arranque conserva la versión antigua, muestra instalación no
confirmada. No elimina datos ni reintenta instalaciones automáticamente. Si la
nueva app no arranca, la recuperación es una entrega manual posterior de versión
superior; no hay rollback automático ni downgrade prometido. Pulse conserva su
validación propia de compatibilidad por revisión.

## Publicación

`create-update.ps1` inspecciona el APPX, rechaza payload/diagnósticos conocidos,
comprueba CMS y clave pública del firmante, calcula SHA-256 y firma la metadata
con el certificado del almacén o el PFX efímero de CI. No exporta claves privadas.
No sobrescribe metadata existente. `verify-appx.ps1` verifica CMS, hashes firmados AXPC/AXCD/AXBM/AXCT/AXCI,
CRC y todos los bloques de 64 KiB. Esto no sustituye el gate real de instalación.

El workflow Release, solo desde main en JulianDr14/jadyx, admite cuatro componentes,
genera metadata antes del tag, crea una release borrador con todos los assets y
la publica al terminar la subida. Conserva el build/firma y la activación manual
existentes. No se ha ejecutado Actions ni publicado una release durante este trabajo.
El empaquetador falla si falta el certificado; deja de generar automáticamente
otro firmante. El instalador sigue en `build-uwp/package/jadyx.appx`, símbolos por
versión y evidencias en `build-uwp/test-results`.

## Gates y límites

- Objetos UWP del núcleo, política, canvas, coordinador, portal y tres catálogos:
  compilación y enlace correctos. APPX 0.3.6.0 normal firmado en entrega única y
  símbolos archivados. ZIP CRC, ejecutable idéntico al build/símbolos, CMS del
  firmante y firma RSA real de la metadata verificados. Evidencia en
  `build-uwp/test-results/updater-0.3.6.0`.
- Harness desktop con OpenSSL real: firma válida/alterada, comparación numérica,
  40 manifiestos firmados inválidos, canales y acciones por estado pasan.
- Transporte Windows.Web.Http real con servidor local: descarga íntegra,
  cancelación, reanudación, corte, servidor que ignora Range, rango incorrecto,
  tamaño falso, hash inválido y GET acotado pasan tras cerrar el handle de lectura
  antes de borrar un archivo corrupto. Los fixtures Python usan newline='' en
  identidades: CRLF de Windows invalidaba su binding y reiniciaba la descarga,
  ocultando que no se estaba ejercitando Range.
- Gate reproducible: `tools/xbox/tests/run-updater-gates.ps1`, con dependencias
  existentes del build UWP y entorno MSVC desktop para ejecutar el harness.
- Metadata del APPX local generada en test-results; ninguna publicación.
- Smoke real AppContainer: biblioteca PID6508 Responding, consulta GitHub
  completada y journal publicado. Sin release nueva con metadata firmada, no se
  ofreció una actualización. Se cerró la biblioteca de prueba, sin guest.
- PowerShell 7 no cargó Appx (0x80131539); el smoke se ejecutó con Windows
  PowerShell 5 mediante su ruta System32 explícita.
- Pendientes: despliegue dentro de AppContainer, suspensión real, disco lleno
  real y navegación/visual con mando,
  gate Series X/S entre dos versiones con
  conservación de datos. No se certifican esos escenarios mediante el harness HTTP.

## Fuentes

- [NXbox: diseño y límites](https://github.com/NspxMiguel/NXbox/blob/main/docs/nxbox-updates.md).
- [NXbox: implementación](https://github.com/NspxMiguel/NXbox/blob/main/src/eden_uwp/ui/updater.cpp).
- [Microsoft: PackageManager.AddPackageAsync](https://learn.microsoft.com/en-us/uwp/api/windows.management.deployment.packagemanager.addpackageasync).
- [Microsoft: Device Portal REST](https://learn.microsoft.com/en-us/windows/uwp/debug-test-perf/device-portal-api-core).
- [Microsoft: limitaciones UWP Xbox](https://learn.microsoft.com/en-us/uwp/extension-sdks/uwp-limitations-on-xbox).


Prueba solicitada19oct: entrega local0.3.4.0 con código actualizador actual, override de empaquetado; manifest fuente0.3.6.0 conservado. APPX firmado en entrega única, CRC/EXE idéntico correctos; símbolos anteriores preservados, nuevos en symbols/0.3.4.0/updater-test-20261019. Release pública v0.3.5 tiene tag de tres componentes y carece de update.json/update.sig: cliente la omite; no prueba descarga/despliegue real hasta publicar metadata compatible. Sin publicación/commit/push. Downgrade sobre instalación superior puede ser rechazado; no desinstalar ni borrar datos para forzar prueba.

## Revisión de calidad y candidato 0.3.6.0 (19 octubre 2026)

Rama `feature/self-updater`, sin commits, pushes ni publicación. Separación entre
política firmada, almacenamiento atómico, validación de paquete, transporte HTTP,
coordinador y presentación. Runtime inyectable para pruebas; la fábrica productiva
mantiene clave y origen fijos. No hay red ni hash en el hilo de renderizado.
Acciones de pantalla en array fijo; notas e historial envueltos se cachean por
revisión/idioma; instantánea consultada a 2 Hz. Buffers de transporte 256 KiB y de
hash/archivo 64 KiB, memoria independiente del tamaño del APPX.

- Suite final desktop: política/40 metadatos inválidos, transporte real,
  13 escenarios de servicio y 14 estructuras APPX correctos.
- Prueba integral privada con APPX firmado real 0.3.6.0 (15.485.031 bytes):
  pausa, Range, hash, XML, recuperación tras cierre sin nueva descarga y
  confirmación por versión instalada inyectada pasan. Reanudación y validación
  ~0,685 s en servidor local; no representa Internet ni CPU de Series.
  10.000 instantáneas Status: media ~0,57 microsegundos en este PC.
- Portal nativo: autenticación, paquete verificado, ticket ligado a versión/hash,
  uso único, reemplazo/reemisión y expiración real de 121 s correctos. Stream
  de 8 MiB conserva handle que impide borrado. Regresiones de importación pasan.
- Navegador: tres idiomas, recuperación busy y rechazo de URL externa/nombre
  incorrecto pasan; regresiones de cola de 5 GiB y claves/firmware pasan.
- Verificador de publicación rechaza ejecutable, firma y directorio ZIP alterados.
  Paquete final: 34 archivos, 738 bloques y cadena firmada verificados.
- Encontrado y corregido: el lector ZIP secuencial rechazaba descriptores ZIP64
  de MakeAppx aunque los ZIP de fixtures pasaran. Callback seek cancelable permite
  a libarchive leer el directorio central; el APPX real ahora es parte del gate.
- Encontrado y corregido: ReplaceFileFromApp puede fallar transitoriamente con
  Win32 1175. Reintentos acotados (máximo 280 ms), conservando journal anterior;
  sin espera normal cuando no hay fallo. No se ignora un fallo persistente.

La instalación interna sigue deshabilitada en producción. Asistencia móvil/PC
es la vía disponible; no se certifican actualización Series, conservación de datos,
interrupción por suspensión real ni ausencia de todos los fallos. Antes de anunciar
superioridad o completar los gates originales hace falta actualización real entre
dos versiones en Xbox y recorrido manual con mando. No publicar como instalación
autónoma probada.

Fuentes adicionales: [bloques APPX](https://learn.microsoft.com/en-us/uwp/schemas/blockmapschema/element-block),
[estructura de firma MSIX SDK](https://github.com/microsoft/msix-packaging/blob/master/src/inc/internal/AppxSignature.hpp),
[validación MSIX SDK](https://github.com/microsoft/msix-packaging/blob/master/src/msix/unpack/AppxSignature.cpp).

Gate final: 25/25 repeticiones adicionales del coordinador (13 escenarios cada
una) correctas. Smoke AppContainer final biblioteca PID2688 Responding, sin guest;
cerrada antes de reempaquetar. Entrega normal 0.3.6.0 restaurada, símbolos EXE
idénticos al build. SHA-256 APPX:
`ecac303c3e5b0e81d128976839b85753a37b35839f70b0a8cca71c2bf342587f`.
Metadata firmada concordante y evidencia integral en
`build-uwp/test-results/updater-release-delivery` y `updater-final-e2e.log`.
Runner admite `-CandidateDirectory` para repetir integral/integridad además de la
suite base. Workflow YAML parseado; Actions no ejecutado ni certificado por ello.


## Investigación de autoinstalación solicitada (19 octubre 2026)

Objetivo principal reiterado: aviso en biblioteca, un botón, descarga/verificación,
reemplazo del paquete y reapertura de la nueva versión sin móvil/PC. La asistencia
es recuperación y no cumple por sí sola ese objetivo.

Microsoft documenta actualización propia sin Store mediante PackageManager y
reapertura UWP con opciones ForceApplicationShutdown/ForceTargetAppShutdown:
https://learn.microsoft.com/en-us/windows/msix/non-store-developer-updates
La página es general de Windows/MSIX: no es una certificación específica Xbox.
Indica también que la gestión del propio paquete debería funcionar sin declarar
packageManagement; la referencia de AddPackageAsync enumera esa capacidad.
Por esa diferencia y por Xbox, comprobar ambas configuraciones en gate controlado,
sin inferir permisos por la sola presencia de la API.

Evidencia primaria de implementación Xbox más concreta: Nativra, commit
`a5cf731fbcd21826648b1b296a589e2bc4847d70`, `uwp/Kiosk/Updater.cs`, líneas 139–165:
https://github.com/NspxMiguel/nativra/blob/a5cf731fbcd21826648b1b296a589e2bc4847d70/uwp/Kiosk/Updater.cs
El autor afirma haber medido registro por usuario correcto al actualizar un
paquete ya instalado en su consola (diferente de instalación nueva). Usa
AddPackageAsync(bundle local, null, ForceApplicationShutdown), comprueba
IsRegistered y errores. También informa fallo de acceso al portal propio por
loopback UWP, por lo que PackageManager pasó a ser ruta principal. Esto es un
reporte del proyecto, no prueba independiente ejecutada por Jadyx ni garantía
de compatibilidad con todos los OS de Series. Comentarios antiguos del encabezado
siguen hablando de portal; el flujo efectivo llama primero PackageManager.

NXbox reconoce explícitamente en su documentación actual que su aceptación de
Device Portal no confirma despliegue ni reapertura y que no probó el despliegue
en consola. No usar su código como prueba de que ese recorrido funciona:
https://github.com/NspxMiguel/NXbox/blob/main/docs/nxbox-updates.md
Microsoft sí documenta instalación y lanzamiento remoto en XboxWdpDriver,
pero desde cliente externo, no como permiso de loopback para una UWP:
https://microsoft.github.io/WindowsDevicePortalWrapper/md_XboxWDPDriver.html

Conclusión: hay una vía nativa documentada y evidencia Xbox reportada para
perseguir el objetivo; no hay base para declarar que Xbox lo prohíbe. Tampoco
hay todavía prueba propia de actualización+reapertura. Priorizar gate nativo,
no el asistente web, antes de publicar la función como autónoma. El candidato
actual conserva Install(false); no se cambió ni recompiló con esta investigación.
Antes de habilitar: declarar/evaluar capacidad, URI local canónica, verificar
IsRegistered además de ExtendedErrorCode, dos versiones de igual identidad/
certificado y marcador de datos; observar cierre/reapertura real en Series y
confirmación al nuevo arranque. Una reapertura de proceso no requiere reiniciar
la consola. Si el OS solo aplica la actualización pero no relanza, reportarlo
como resultado parcial y mantener asistencia sin prometer la experiencia completa.


## PackageManager habilitado por solicitud del usuario

`package_deployment.{h,cpp}` contiene el adaptador nativo; el coordinador recibe
su función mediante Runtime para poder probar aceptación, rechazo y errores sin
instalar nada en el PC del usuario. La fábrica productiva inyecta DeployPackage,
sin booleano que derive siempre a asistencia. Manifiesto con rescap
packageManagement, identidad/certificado/version0.3.6.0 conservados.

Flujo: Download(true) guarda intención solo en memoria, verifica el paquete y
queda Ready. La biblioteca vuelve a reservar admisión contra importaciones/escrituras
antes de InstallIfReady. Se guarda target ANTES de invocar Windows. El worker MTA
solicita el reemplazo y comprueba ExtendedErrorCode+IsRegistered; si la instancia
antigua termina mientras la operación sigue en vuelo, conserva estado Requested,
sin afirmar registro ni éxito final. Solo el arranque con target confirma éxito.

No Cancel() de la operación Windows por suspensión/cierre del proceso antiguo:
ese cierre puede ser causado precisamente por la instalación. Pause durante
Preparing/Requested no revoca despliegue. Cancel/SetChannel y navegación/juegos
bloqueados mientras Windows puede reemplazar el paquete. Antes de enviarlo, pausa,
cancelación o salida revocan la intención automática; no persiste para provocar
instalaciones inesperadas al siguiente arranque. Rechazos guardan HRESULT e historial
y habilitan asistencia, sin reenvío automático.

El archivo de descarga ahora es `updates/jadyx-update.appx`: no se entrega una ruta
con extensión `.part` al broker APPX. La marca interna de verificación sigue siendo
obligatoria. URI file local con componentes escapados, incluidas carpetas con
espacios, # o %, sin aceptar rutas UNC. No copia todo el paquete en RAM ni duplica
el archivo para desplegarlo. Consulta de operación cada50ms solo en worker.

Para probar el reemplazo real, la versión fuente debe ser menor que la objetivo:
instalar0.3.6 sobre0.3.6 no es una actualización y se rechaza deliberadamente.
Se puede preparar una base privada0.3.5.1 con este mismo código y comprobar hacia
0.3.6.0 firmada, sin downgrade de una consola que ya tenga0.3.6. No desinstalar.
Para el gate Series registrar versiónOS, capabilities, target, HRESULT/IsRegistered,
versión tras reemplazo, si el OS reabre la app y un marcador/guardados intactos.
Una instalación aceptada sin reapertura es gate parcial, no experiencia completa.


### Resultado del candidato nativo

Build incremental UWP/enlace correcto. Suite final: política/HTTP, 19 escenarios
de coordinador, 14 fixtures APPX, 40 metadatos inválidos y navegador correctos;
10/10 repeticiones adicionales de los19 escenarios pasan. Los escenarios nativos
inyectan el resultado del OS: no confundirlos con despliegue PackageManager real.
Incluyen falta de admisión, guardado previo target, IsRegistered=false, error de
acceso, error aun con registered=true, solicitud en vuelo, no reenvío, protección
de archivo/canal mientras Requested, arranque antiguo no confirmado y nuevo
confirmado. No se instaló una actualización real desde dentro de Jadyx en PC.

Registro/arranque AppContainer con capacidad packageManagement correcto,
PID9404 Responding en biblioteca, sin guest. Windows rechazó re-registrar0.3.6.0
con capacidades diferentes (0x80073CFB); smoke usó revisión temporal0.3.6.1,
sin desinstalar ni borrar datos, y luego restauró registroPC0.3.6.0 mediante
ForceUpdateFromAnyVersion. La revisión temporal no es una entrega ni publicación.
Esta prueba demuestra registro/arranque, no el permiso de autodespliegue en Xbox.

Entrega final única `build-uwp/package/jadyx.appx`: versión0.3.6.0 normal,
34archivos/738bloques, CMS/AXPC/AXCD/AXBM/AXCT/AXCI/payload correctos; tests de
alteración payload/firma/directorio rechazados. EXE idéntico a build y símbolos
`build-uwp/symbols/0.3.6.0`. SHA256:
`bfbfeb5e0163f1f979de35daafaa6e012d90ebccf4c996e9dd88c9243a25bfe1`.
Metadata local concordante en `build-uwp/test-results/updater-packagemanager-delivery`.
Integral HTTP real+APPX firmado+pausa/Range/restart pasa (15.487.776bytes,
~0,659s local; Status media0,87usPC). En esa prueba la respuesta de despliegue
es simulada y la versión instalada está inyectada; no certifica reinicio real.
Evidencia `updater-packagemanager-{gates-final,build-final,smoke,e2e-final,package-verification}.log`.
Sin commits, pushes ni publicación. GateSeries/reapertura/datos pendiente.

## Prueba de actualización con versión instalada falseada (20 octubre 2026)

Para probar aviso, descarga y verificación cuando el release
publicado (p. ej. 0.3.7.0) no es mayor que el paquete instalado, el paquete de prueba
lleva en `boot.cfg` la línea `update_installed=X.Y.Z.W`. El updater (`ProductionRuntime`
en `src/jadyx/updates/updater.cpp`) la lee de la carpeta instalada y usa esa versión
como "instalada" en vez de la del manifiesto.

Comando (solo paquetes locales, nunca publicados):

```
.\tools\xbox\package-appx.ps1 -Library -Version 0.3.7.1 -StageScript ..\eden-data\stage-userdata.ps1 -BootCfg update_installed=0.3.6.37
```

- `-Release` rechaza cualquier línea extra de `boot.cfg`, así que el parche no puede
  colarse en un release. Aun así, comprobar que el `boot.cfg` publicado solo tenga
  `library=1` y `play=1`.
- Con la versión falseada, instalar de verdad 0.3.7.0 encima de 0.3.7.1 sería un downgrade
  (0x80073D06). Para probar la instalación completa hace falta un release mayor que el
  paquete instalado.

### Servidor local de releases

Falsear la versión no basta para probar la instalación: instalar un release inferior es un
downgrade. Para eso el paquete de prueba lleva `update_server=http://<ip-del-PC>:8787` en
`boot.cfg`; el updater manda ahí las peticiones a `api.github.com/repos/JulianDr14/jadyx/` y
`github.com/JulianDr14/jadyx/` (mismas rutas, sin prefijo). El manifiesto sigue firmado con las
URLs de GitHub y pasa por la misma política que en producción.

1. Paquete "release" mayor que el instalado, sin keys (el firmador las rechaza):
   `package-appx.ps1 -Library -Version 0.3.7.2`, copiar `jadyx.appx` a
   `build-uwp\update-server\v0.3.7.2\Jadyx_0.3.7.2_x64.appx` y firmar `update.json`/`update.sig`
   con `create-update.ps1` (requiere PowerShell 7) o un firmador equivalente con el certificado
   `82FAA237…` del almacén. Comprobar con
   `openssl dgst -sha256 -verify src/jadyx/updates/release_public_key.pem -signature update.sig update.json`.
2. Paquete instalado (las keys y el firmware ya están en LocalState de la consola):
   `package-appx.ps1 -Library -Version 0.3.7.1 -BootCfg update_server=http://<ip>:8787`.
3. `py tools/xbox/tests/update-server.py build-uwp\update-server` (puerto 8787; el firewall de
   Windows debe permitir la entrada).

`-Release` rechaza `update_server=` igual que `update_installed=`.

## Por qué Xbox no se actualiza sola (21 octubre 2026)

Se probó en una Series en Dev Mode (0.3.7.0 y 0.3.7.3 → 0.3.7.4, con rastreo en la app) y
ninguna vía de instalación interna sirve. Las pruebas y el rastreo se retiraron del código; en
Xbox `runtime.deploy` queda vacío y la instalación siempre es guiada.

- **PackageManager desde la app (`AddPackageAsync` + `ForceApplicationShutdown`):** el Device
  Portal registra Jadyx para `XBOX\DefaultAccount` (RID 503), pero la app se ejecuta como
  `XBOX\UserMgr0` (RID 1002). La instalación llega al 97 %, Windows cierra la app y la versión
  nueva queda registrada **solo para UserMgr0**; la de DefaultAccount sigue en la vieja (el
  Device Portal muestra las dos). Desde entonces la consola ya no abre Jadyx. El modo diferido
  (`DeferRegistrationWhenPackagesAreInUse`) registra para el mismo usuario: mismo problema.
- **Instalar para todos los usuarios** (`FindPackages`, aprovisionar): `0x80070005`; requiere
  administrador y la app no lo es.
- **Device Portal desde la app:** 127.0.0.1, localhost y la IP propia dan timeout. UWP trata la
  IP de la consola como loopback y la aísla; en Xbox no hay exención.
- **Device Portal desde una página web** (p. ej. el portal de Jadyx abierto en el PC): el Portal
  no envía cabeceras CORS y responde `403 CSRF Token Invalid` a la petición previa del
  navegador, así que una página no puede subirle el paquete.
- **Programa auxiliar en el PC** (descarga y llama al Portal, que sí responde fuera del
  navegador): funcionaría, pero obliga a mantener otra aplicación; descartado.
- Proyectos similares (Nativra, NXbox) usan esas mismas vías y no documentan una actualización
  que deje ambas cuentas en la misma versión.

**Recuperación** si una consola quedó con versiones divididas: instalar por el Device Portal una
versión **mayor** encima, sin desinstalar (desinstalar borra los datos de UserMgr0).

**Limitaciones de la vía guiada:** hace falta un móvil o PC en la misma red, y elegir el archivo
y pulsar instalar dentro del Device Portal es manual. Todo lo demás (buscar, descargar,
verificar la firma, servir el paquete y abrir el Portal) lo hace Jadyx.

### Instalación guiada

1. **Consola:** con la versión verificada (fase Ready), *Instalar desde el móvil o PC* abre la
   pantalla de instalación sobre la página de actualizaciones. El portal de red arranca solo y
   la pantalla muestra la dirección, el código de 4 dígitos, los tres pasos y el aviso de no
   desinstalar. B vuelve a la página de actualizaciones; Detener apaga el portal sin salir.
2. **Navegador:** mientras la consola está en Assistance, `/api/v1/status` lleva
   `update.offered=true` y la página, tras emparejar, abre directamente en *Actualizar Jadyx*
   (*Ir al contenido* la aparta y deja un aviso abajo). La página pide el ticket por adelantado
   (dura 2 minutos y sirve una vez; se renueva a los 90 s y tras cada uso) para que un solo
   clic en *Descargar e instalar* baje el appx y abra `https://<host>:11443` en otra pestaña.
3. **Device Portal:** aceptar el certificado autofirmado, Add → archivo → Next → Install.
   Cuando la consola deja de responder tras la descarga (3 sondeos fallidos o sesión perdida),
   la página pasa a *Instalando la actualización*.
4. **Reapertura:** si la versión instalada coincide con la objetivo guardada, la biblioteca
   muestra una vez *Jadyx se actualizó a X* hasta la siguiente entrada del mando.
