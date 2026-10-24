<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Jadyx: de la instalación a tu primer juego

[English](guide.md) · [Português (Brasil)](guide.pt-BR.md)

Sigue las tres partes en orden. Necesitas una Xbox Series X o S ya activada en **Modo de desarrollador**, un mando y un ordenador en la misma red local. Device Portal es la web de administración de Xbox; Portal Jadyx es otra web, iniciada dentro de Jadyx para importar archivos.

Prepara solo dumps de **tu propia Switch y de juegos que posees**: `prod.keys`, `title.keys` cuando haga falta, firmware y juegos. Jadyx no incluye ninguno. No pidas ni compartas esos archivos en los canales de soporte.

## Parte 1 · Instala Jadyx

1. Descarga `Jadyx_<version>_x64.appx` y `Microsoft.VCLibs.x64.14.00.appx` de la [página de versiones de Jadyx](https://github.com/JulianDr14/jadyx/releases/latest). Guarda ambos en el ordenador.
2. En Xbox, abre **Dev Home**. En **Remote Access**, activa **Xbox Device Portal** y configura usuario/contraseña si te lo pide. Anota la dirección mostrada, normalmente `https://192.168.x.x:11443`.
3. Abre esa dirección exacta en el navegador del ordenador. El aviso de certificado es normal en esta página local: continúa solo después de comprobar que es la dirección de tu Xbox. Inicia sesión con las credenciales de Remote Access. Deberías ver Device Portal.
4. En su página **Home**, elige **Add**, selecciona el `.appx` de Jadyx y después **Next**. Cuando pida dependencias, añade `Microsoft.VCLibs.x64.14.00.appx` y pulsa **Start**. Espera a que termine: Jadyx debe aparecer en Dev Home. El `.cer` **no es necesario en el Modo de desarrollador de Xbox**; el mantenedor confirmó la instalación con APPX + VCLibs en Series X el 23–24 de octubre de 2026.
5. En Dev Home de Xbox, selecciona Jadyx, abre **View details** y cambia **App type** a **Game**. Compruébalo antes de abrirlo. Como App, dispone de aproximadamente **1 GB** de memoria; como Game, Jadyx recibe unos **5 GB (5120 MiB)**. Es RAM, no espacio para archivos.

Para actualizar después, instala el nuevo APPX encima del existente. **No desinstales antes**: eso borra claves, firmware, partidas y juegos internos de la app.

> **Próximamente:** una instalación más sencilla. Por ahora, sigue los pasos de APPX + VCLibs anteriores.

## Parte 2 · Añade tus claves, firmware y juegos

El almacenamiento interno del Modo de desarrollador puede tener solo unos pocos GB libres, aunque Xbox tenga un SSD grande. Deja los juegos grandes en una unidad USB; las subidas por el portal usan almacenamiento interno.

### Claves y firmware: Portal Jadyx

1. Abre Jadyx. Usa los botones superiores del mando (LB/RB) para llegar a **Configuración** y abre **Gestor de archivos**. Selecciona **Portal Jadyx**, cuya acción es **Iniciar portal**. Aparece una pantalla con una dirección local y **Código de conexión**.
2. En el ordenador, abre la dirección que muestra Jadyx, no la de Device Portal. Introduce el código. Ambos dispositivos deben estar en la misma red local. Si caduca, elige **Generar código de conexión** en Jadyx y usa el nuevo código.
3. En el navegador, abre **Sistema > Claves**. En **Seleccionar archivos**, elige tu `prod.keys`, más `title.keys` si tu dump lo necesita, o un ZIP que los contenga. Pulsa **Subir y revisar**. Revisa versiones y advertencias, después **Confirmar instalación**. No aceptes una advertencia sin comprobar los archivos de origen.
4. En **Sistema > Firmware**, elige tu ZIP de firmware o los archivos `.nca` extraídos (el navegador también ofrece **O elige una carpeta extraída**). Pulsa **Subir y revisar**, revisa el resultado y después **Confirmar instalación**. Importa las claves primero: deben poder leer ese firmware. Espera a **Instalación completada** antes de salir.
5. De vuelta al Gestor de archivos de Jadyx, comprueba **Claves listas** y **Firmware: … archivos**. El contador confirma que hay archivos; no garantiza que todos los juegos funcionen.

Las claves se importan a `LocalState\jadyx\keys` y la app las carga desde allí. Para copiar tus propias claves manualmente por Device Portal, cierra Jadyx, abre **File explorer > LocalAppData > paquete de Jadyx > LocalState > jadyx > keys**, sube `prod.keys` y, si hace falta, `title.keys`, y vuelve a abrir Jadyx. Una carpeta `keys` cualquiera en la raíz del USB no es esa ubicación.

### Juegos: USB para archivos grandes, o almacenamiento interno

- **USB (recomendado para juegos grandes):** copia tus dumps completos `.nsp` o `.xci` a una carpeta de la unidad; también se admite homebrew `.nro`. Usa un sistema de archivos que permita archivos de más de 4 GiB, como NTFS; FAT32 no los permite. Conéctala a Xbox como almacenamiento multimedia. **No elijas “games & apps” ni aceptes formatear**, porque borra la unidad. En **Configuración > Gestor de archivos > Agregar carpeta de juegos**, elige esa carpeta en el selector. Los juegos permanecen en USB; mantenlo conectado al jugar. También se leen las subcarpetas.
- **Subida por navegador:** en Portal Jadyx, abre **Contenido > Juegos > Seleccionar archivos NSP**, elige un `.nsp` de juego base y pulsa **Subir / reanudar**. Espera la validación y su incorporación a la biblioteca. Esta ruta copia al almacenamiento interno y acepta juegos base NSP, no XCI ni actualizaciones/DLC.
- **Alternativa por Device Portal:** con Jadyx cerrado, sube los dumps por **File explorer > LocalAppData > paquete de Jadyx > LocalState > games**. Vuelve a abrir Jadyx después. Es `LocalState\games`, separado de `LocalState\jadyx`.

Para tus actualizaciones/DLC, añade primero el juego base. En la biblioteca, pulsa **Menu** sobre él, elige **Actualizaciones** o **DLC** y después **Añadir contenido…**. Elige **USB / carpeta original · sin copiar** o **Almacenamiento interno**, selecciona un paquete NSP/XCI completo, revísalo y confirma. Mantén conectado el contenido externo. Solo una actualización está activa; los DLC se pueden activar individualmente. No subas actualizaciones como juegos base.

## Parte 3 · Juega

1. Vuelve a **Biblioteca**. Si falta un juego recién copiado, pulsa **X** para volver a buscar. Selecciona el juego y pulsa **A** para jugar. Deja conectado el USB si el juego o su contenido seleccionado está allí.
2. Espera mientras se compilan los shaders. La primera visita a una zona puede tener tirones; compatibilidad y rendimiento varían según el juego. Jadyx es una beta.
3. Mantén **View + Menu** para abrir el menú con el juego pausado. Elige **Continuar** para seguir o **Volver al inicio** para regresar a la biblioteca; espera a que termine antes de cerrar la app.

**Series S:** usa el mismo instalador y la opción Game. Hay informes de la comunidad en Series S en el [README](../../README.md#progress), pero los resultados de Series X no garantizan el mismo rendimiento o compatibilidad en S. Indica tu modelo en los informes.

### Si algo falla

- **Petición de dependencia / falta Microsoft.VCLibs:** vuelve a la Parte 1 y proporciona el APPX x64 de VCLibs como dependencia, no como paquete de Jadyx.
- **“The existing keys cannot read this content” (las claves existentes no pueden leer el contenido):** revisa/importa tu propio `prod.keys` compatible. Si un dump de actualización está protegido por derechos y no contiene ticket, su clave de título correspondiente debe estar en tu propio `title.keys`; `prod.keys` por sí solo no la proporciona. Reimporta ambos en Claves y reintenta. Comprueba también que el dump esté completo. Nunca publiques las claves.
- **Firmware ilegible/incompleto:** importa claves compatibles primero, después un dump completo del firmware de tu consola. Una carpeta con NCA de juegos no es firmware.
- **Subida fallida / falta de espacio:** comprueba el espacio interno libre del Modo de desarrollador en Device Portal. Las subidas pueden fallar cuando ese disco está lleno. Libera archivos que ya no necesites, conserva las partidas y usa USB para juegos grandes. La capacidad anunciada del SSD de Xbox no es el espacio disponible para la app.
- **`bad allocation` / la app se cierra:** comprueba primero **App type: Game**. Incluso como Game, un juego exigente puede superar el límite de aproximadamente 5 GB.
- **Juego USB ausente/ilegible:** comprueba la conexión y selecciona de nuevo la carpeta con **Agregar carpeta de juegos**. Comprueba que la copia haya terminado; la ruta por sí sola no da permiso a la app.
- **Pantalla negra o cierre tras estas comprobaciones:** recoge los dos logs siguientes; una pantalla negra por sí sola no identifica la causa.

### Recoge los logs

Abre **Device Portal > File explorer > LocalAppData** de Xbox, selecciona el paquete de Jadyx y después **LocalState > jadyx > log**. Descarga **ambos**:

- `LocalState\jadyx\log\jadyx_diag.txt`
- `LocalState\jadyx\log\jadyx_log.txt`

Envíalos al [soporte de Jadyx](https://discord.gg/3BhyyXGxpd), con versión de la app, modelo Series X/S, si está como Game, qué hiciste y qué ocurrió. Nunca incluyas claves, firmware ni juegos.
