# Instalar y preparar Jadyx en Xbox

Necesitas una Xbox Series X|S ya en Modo de Desarrollador y las claves, el firmware y los juegos
extraídos de tu propio Switch. Jadyx no proporciona estos archivos.
El Device Portal de Xbox instala la aplicación. Jadyx Portal recibe tus archivos después.
Son páginas distintas. Los nombres en inglés de abajo son los botones de Xbox y del portal.

1. Descarga el `.appx` de Jadyx de los archivos de la versión.
2. Descarga `Microsoft.VCLibs.x64.14.00.appx` de la misma versión.
3. Abre **Dev Home** en Xbox.
4. Anota la IP que aparece en **Remote Access**.
5. Abre `https://<IP-de-Xbox>:11443` en el navegador de un dispositivo de la misma red.
6. Abre **Apps > Add** en Device Portal.
7. Selecciona el `.appx` de Jadyx como aplicación.
8. Selecciona `Microsoft.VCLibs.x64.14.00.appx` como dependencia.
9. Inicia la instalación y espera a que termine.
10. Selecciona Jadyx en **Dev Home** en Xbox.
11. Pulsa **View** en el mando para abrir **View details**.
12. Cambia **App type** a **Game** (también en **Apps > Jadyx > App type** de Device Portal).
13. Abre Jadyx de nuevo después de cambiar el modo.
14. Abre **Ajustes > Archivos > Conexión de red > Iniciar servidor** en Jadyx.
15. Abre la dirección que muestra Jadyx en el navegador de un dispositivo de la misma red.
16. Introduce el código que muestra Jadyx.
17. Importa tu `prod.keys` en **Sistema > Claves** de Jadyx Portal.
18. Importa tu ZIP de firmware o los archivos NCA extraídos en **Sistema > Firmware**.
19. Confirma las versiones que muestra la revisión de importación.
20. Sube un NSP de juego base en **Juegos**.
21. Sube actualizaciones en **Biblioteca > tu juego > Actualizaciones**.
22. Sube DLC en **Biblioteca > tu juego > DLC**.

`Jadyx.cer` es **opcional** en Device Portal. En la prueba medida en una Series X, el paquete con
certificado propio se instaló sin instalar ni confiar en el `.cer`. Conserva el archivo por si tu
herramienta de instalación lo pide; no es un requisito normal.

El modo Game ofrece 5120 MiB. El modo App tiene poca memoria para el emulador. Un aviso permanente
en la biblioteca explica el cambio cuando Xbox informa de un límite menor. La lista de preparación
aparece cuando faltan claves utilizables o archivos de firmware; quien ya configuró conserva la
biblioteca normal.

`title.keys` contiene claves por título para tus copias cifradas con tickets; no sustituye a
`prod.keys`. Si las claves no leen un firmware o una actualización más reciente, extrae claves
compatibles de tu consola con ese firmware o posterior. El índice de generación de claves no es
la versión del firmware. Cuando no se puede descifrar la versión, Jadyx no puede nombrarla.

## Por USB

1. Pon las claves en una carpeta del USB.
2. Pon el ZIP del firmware o los NCAs extraídos en otra carpeta.
3. Selecciona la carpeta correspondiente en **Ajustes > Archivos**.
4. Añade la carpeta de juegos externos en esa misma pantalla.

El selector elige carpetas, incluso cuando contienen un ZIP. No selecciones una carpeta que mezcle
firmware y juegos. La aplicación importa firmware y claves al almacenamiento interno.

## Copia manual por Device Portal

1. Cierra Jadyx.
2. Abre **File explorer > LocalAppData > Jadyx > LocalState** en Device Portal.
3. Copia `prod.keys` y, si hace falta, `title.keys` a `jadyx/keys`.
4. Copia los NCAs del firmware ya extraídos a `jadyx/nand/system/Contents/registered`.
5. Copia los juegos base a `games`.
6. Abre Jadyx de nuevo.

Selecciona el paquete instalado de Jadyx si el portal muestra el nombre de la familia del paquete.
La copia manual no revisa la importación; prefiere el importador de la aplicación o Jadyx Portal.

## Si parece atascado

Mira los bytes recibidos y confirmados. **Validando** comprueba el paquete; **Añadiendo** publica
el juego. Tras 60 segundos sin progreso al importar firmware, el aviso muestra el archivo actual.
Revisa la conexión USB y el espacio libre antes de reintentar. Cuando falta almacenamiento,
el error muestra GB decimales libres y necesarios, incluido el espacio temporal. Al extraer ZIP,
la estimación cubre la entrada actual, no todo el archivo por adelantado.

Si envías una actualización o DLC a Juegos, el aviso indica la página correcta de la biblioteca.
Elimina la transferencia pendiente antes de enviarla de nuevo en la página indicada.

Para actualizar Jadyx, instala el paquete nuevo sobre el antiguo. **No desinstales primero**: eso
borra claves, firmware, partidas y juegos internos. Los diagnósticos están en `LocalState/jadyx/log`.
